"""Telegram <-> ESP32 (serial) <-> FlashForge Adventurer 3 relay, with .gx upload.

Env vars:
  TELEGRAM_BOT_TOKEN   bot token (required)
  TELEGRAM_CHAT_IDS    comma-separated allowlisted chat IDs (required)
  SERIAL_PORT          default COM2 (Linux: /dev/ttyUSB0 or /dev/ttyACM0)
  SERIAL_BAUD          default 921600 (must match SERIAL_BAUD in flashBot.ino)
"""
import os
import re
import struct
import sys
import threading
import time

import requests
import serial

TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
ALLOWED = {c.strip() for c in os.environ.get("TELEGRAM_CHAT_IDS", "").split(",") if c.strip()}
SERIAL_PORT = os.environ.get("SERIAL_PORT", "COM2")
SERIAL_BAUD = int(os.environ.get("SERIAL_BAUD", "921600"))

if not TOKEN or not ALLOWED:
    sys.exit("Set TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_IDS (comma-separated) first.")

API = f"https://api.telegram.org/bot{TOKEN}"
FILE_API = f"https://api.telegram.org/file/bot{TOKEN}"

ALLOWED_EXT = (".gx", ".gcode", ".g")
MAX_BYTES = 20_000_000    # matches MAX_UPLOAD_BYTES in the sketch (and Telegram's 20 MB bot limit)
MAX_NAME = 60
FRAME = 4096
CMD_TIMEOUT = 60          # ESP32 worst case: Wi-Fi + socket reconnect (~40 s) then the command
READY_TIMEOUT = 60
ACK_TIMEOUT = 20          # per frame (printer Wi-Fi can stall)
DONE_TIMEOUT = 30         # M29 confirmation

# Unsolicited lines the ESP32 prints while booting; never the answer to a command.
BOOT_PREFIXES = ("~Connecting", "~Waiting for printer session", "~Printer connected, session started")

port = None


def redact(x):
    return str(x).replace(TOKEN, "<token>")


# ---------- Telegram helpers ----------
def say(chat_id, text, timeout=15):
    try:
        requests.post(f"{API}/sendMessage", data={"chat_id": chat_id, "text": text[:4000]}, timeout=timeout)
    except requests.RequestException as e:
        print("sendMessage failed:", redact(e))


def say_bg(chat_id, text):
    """Fire-and-forget. Used mid-upload: the ESP32 aborts if the laptop stalls for 8 s."""
    threading.Thread(target=say, args=(chat_id, text, 5), daemon=True).start()


def download(file_id):
    """getFile, then GET the file into memory. Returns bytes."""
    info = requests.get(f"{API}/getFile", params={"file_id": file_id}, timeout=15).json()
    if not info.get("ok"):
        raise RuntimeError("getFile failed: " + str(info.get("description")))
    path = info["result"]["file_path"]
    r = requests.get(f"{FILE_API}/{path}", timeout=120)
    r.raise_for_status()
    return r.content


def sanitize(name):
    name = re.sub(r"[^A-Za-z0-9._-]", "_", name)
    if name.startswith("."):
        name = "upload" + name           # the ESP32 rejects names starting with '.'
    if len(name) > MAX_NAME:
        stem, ext = os.path.splitext(name)
        name = stem[: MAX_NAME - len(ext)] + ext
    return name


# ---------- Serial helpers ----------
def read_reply(timeout):
    """Return one '~' reply (continuation lines up to the blank line) or None on timeout.
    Boot-time chatter from the ESP32 is skipped."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = port.readline().decode(errors="ignore").strip()
        if not line:
            continue
        print(line)
        if line.startswith("~"):
            reply = line
            while True:
                more = port.readline().decode(errors="ignore").strip()
                if not more:
                    break
                reply += "\n" + more
            if reply.startswith(BOOT_PREFIXES):
                continue
            return reply
    return None


def wait_for_esp(total=90):
    """After opening the port the ESP32 may reboot and wait for the printer's Wi-Fi.
    Wait until it is up (or answers a Ping) before relaying anything."""
    t0 = time.time()
    booting = False
    last_note = 0
    while time.time() - t0 < total:
        line = port.readline().decode(errors="ignore").strip()
        if "Printer connected" in line:
            print("ESP32 ready.")
            return True
        if line.startswith(("~Connecting", "~Waiting for printer")):
            booting = True
            if time.time() - last_note > 10:
                print("ESP32 is waiting for the printer (is it on?)...")
                last_note = time.time()
        elif not line and not booting and time.time() - t0 > 3:
            port.write(b"Ping\n")             # already running, so it was not rebooted
            if read_reply(10):
                print("ESP32 ready.")
                return True
            return False
    return False


def open_port():
    global port
    port = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.5, write_timeout=10)
    time.sleep(2)
    port.reset_input_buffer()
    if not wait_for_esp():
        print("WARNING: ESP32 did not report ready; continuing anyway.")


def reopen_port():
    try:
        port.close()
    except Exception:
        pass
    for _ in range(15):
        try:
            open_port()
            return True
        except serial.SerialException:
            time.sleep(2)
    return False


def command(text):
    port.reset_input_buffer()
    port.write(text.encode() + b"\n")
    return read_reply(CMD_TIMEOUT)


# ---------- Upload ----------
class UploadError(Exception):
    def __init__(self, msg, esp_aborted=False):
        super().__init__(msg)
        self.esp_aborted = esp_aborted   # True: ESP32 already left upload mode, don't send cancel


def frame(data):
    port.write(struct.pack(">H", len(data)) + data)


def cancel_upload():
    try:
        port.write(b"\xff\xff\n")        # the \n ends the line if the ESP32 was not in upload mode
        read_reply(5)                    # swallow "~ERR upload cancelled"
        port.reset_input_buffer()
    except Exception as e:
        print("cancel failed:", redact(e))


def find_line(reply, prefix):
    """ESP32 may prepend notes like '(reconnected to printer)', so look at every line."""
    lines = [ln.lstrip("~").strip() for ln in reply.split("\n")]
    for i, ln in enumerate(lines):
        if ln.startswith(prefix):
            return "\n".join(lines[i:])
    return None


def expect(timeout, ok_prefix):
    r = read_reply(timeout)
    if r is None:
        raise UploadError(f"ESP32 not answering (check USB) while waiting for {ok_prefix}")
    err = find_line(r, "ERR")
    if err:
        raise UploadError(err, esp_aborted=True)
    ok = find_line(r, ok_prefix)
    if ok is None:
        raise UploadError("unexpected reply: " + r.replace("\n", " | "))
    return ok


def upload(chat_id, name, data):
    size = len(data)
    port.reset_input_buffer()
    port.write(f"Upload {size} {name}\n".encode())
    expect(READY_TIMEOUT, "READY")

    sent, n, next_pct = 0, 0, 20
    started = time.time()
    try:
        for off in range(0, size, FRAME):
            chunk = data[off:off + FRAME]
            frame(chunk)
            n += 1
            expect(ACK_TIMEOUT, "ACK")
            sent += len(chunk)
            pct = sent * 100 // size
            while pct >= next_pct and next_pct < 100:
                say_bg(chat_id, f"Uploading {name}: {next_pct}%")
                next_pct += 20
        frame(b"")
        expect(DONE_TIMEOUT, "UPLOAD DONE")
    except UploadError as e:
        if not e.esp_aborted:
            cancel_upload()
        raise
    except serial.SerialException:
        cancel_upload()
        raise
    return time.time() - started


def handle_document(chat_id, doc):
    raw_name = doc.get("file_name") or "upload.gx"
    size = doc.get("file_size") or 0
    if not raw_name.lower().endswith(ALLOWED_EXT):
        say(chat_id, "Rejected: file must be .gx, .gcode or .g")
        return
    if size > MAX_BYTES:
        say(chat_id, f"Rejected: {size / 1e6:.1f} MB is over the 20 MB limit")
        return
    name = sanitize(raw_name)
    say(chat_id, f"Uploading {name} ({size / 1e6:.1f} MB)...")
    try:
        data = download(doc["file_id"])
        if not data or len(data) > MAX_BYTES:
            raise RuntimeError("downloaded file is empty or over 20 MB")
        secs = upload(chat_id, name, data)
        say(chat_id, f"Uploaded {name} in {secs:.0f}s. Send: Print {name}")
    except serial.SerialException:
        raise                            # main loop reports it and reopens the port
    except Exception as e:
        say(chat_id, f"Upload failed: {redact(e)}")


def handle_text(chat_id, text):
    text = text.strip()
    if "\n" in text or "\r" in text:
        say(chat_id, "One command per message, please.")
        return
    if text.lower().startswith("upload"):
        say(chat_id, "Upload is done by sending a .gx file, not by typing it.")
        return
    print("->", text)
    reply = command(text)
    say(chat_id, reply[1:] if reply else "ESP32 not answering (check USB)")


# ---------- main loop (single-threaded: nothing else runs during an upload) ----------
def get_updates(offset, timeout, **extra):
    r = requests.get(f"{API}/getUpdates", params={"timeout": timeout, "offset": offset, **extra},
                     timeout=timeout + 10)
    j = r.json()
    if not j.get("ok"):
        print("Telegram API error:", j.get("error_code"), j.get("description"))
        time.sleep(3)
        return []
    return j.get("result", [])


def main():
    open_port()

    # Throw away anything sent while we were offline: no stale Print/HomeAll/Cancel replays.
    offset = 0
    while True:
        try:
            old = get_updates(-1, 0)
            if old:
                offset = old[-1]["update_id"] + 1
                get_updates(offset, 0, limit=1)       # confirm so Telegram forgets them
            break
        except requests.RequestException as e:
            print("startup getUpdates failed:", redact(e))
            time.sleep(3)
    print("Listening for Telegram messages...")

    while True:
        try:
            updates = get_updates(offset, 30)
        except (requests.RequestException, ValueError) as e:
            print("getUpdates failed:", redact(e))
            time.sleep(3)
            continue
        if not updates:
            continue
        offset = updates[-1]["update_id"] + 1
        try:
            get_updates(offset, 0, limit=1)           # confirm BEFORE handling: never replay after a crash
        except (requests.RequestException, ValueError):
            pass

        for upd in updates:
            msg = upd.get("message") or {}
            chat_id = str(msg.get("chat", {}).get("id", ""))
            if chat_id not in ALLOWED:
                print(f"ignored message from chat {chat_id!r} (not in TELEGRAM_CHAT_IDS)")
                continue
            try:
                if "document" in msg:
                    handle_document(chat_id, msg["document"])
                elif "text" in msg:
                    handle_text(chat_id, msg["text"])
            except serial.SerialException as e:
                print("serial error:", redact(e))
                say(chat_id, "ESP32 disconnected (check USB). Reconnecting...")
                say(chat_id, "Reconnected. Try again." if reopen_port() else "Could not reopen the serial port.")
            except Exception as e:
                print("error:", redact(e))
                say(chat_id, f"Internal error: {redact(e)}")


if __name__ == "__main__":
    main()
