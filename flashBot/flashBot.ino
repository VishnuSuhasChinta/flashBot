// flashBot.ino : ESP32 <-> FlashForge Adventurer 3 bridge
//
// Serial (USB, from the Linux laptop) <-> this ESP32 <-> TCP 10.10.100.254:8899 (printer)
//
// REPLY FORMAT (unchanged): every answer is  "~" + text + "\n" + blank line.
//   The Linux script reads from the "~" line until the blank line.
//
// COMMANDS (typed in Telegram, forwarded as-is by the Linux script)
//   Get Info | Get Status | Get Temperatures | Get Endstop | Get Position
//   HomeAll | Pause | Resume | Cancel | Release
//   Ping                      : ESP32 + printer health, works even if the printer is dead
//   Files                     : list files on the printer (M661; M20 is not documented for the A3)
//   Print <file>              : select a file already on the printer (M23)
//   Relative Move Mode        : enter jog mode (send "10, 0, 0" ... "e e e" to leave)
//   Upload <size> <name>      : start a binary upload (see UPLOAD PROTOCOL below)
//
// TODO (ticket): a way to text JUST the ESP32 and see it reply, without touching the printer.
//   Idea: a small Python "serial console" script (or a /esp prefix in telegram.py) that sends one
//   line over serial and prints/forwards whatever comes back, incl. a "no reply" message.
//
// UPLOAD PROTOCOL (laptop side must follow this, Python is NOT written yet)
//   1. Laptop sends the text line:  Upload <size_in_bytes> <name.gx|name.gcode>
//   2. ESP32 replies "~READY" (or "~ERR ...", in which case stop).
//   3. Laptop then sends frames: 2-byte big-endian length L, then L bytes.
//        L = 1..4352 : raw file bytes, forwarded unchanged to the printer's TCP socket.
//                      ESP32 replies "~ACK n". Send the next frame ONLY after you get
//                      the ACK (stop-and-wait; this protects the ESP32's serial buffer).
//        L = 0       : end of file. ESP32 checks total bytes == <size>, sends M29, and
//                      replies "~UPLOAD DONE ...".
//        L = 0xFFFF  : cancel. ESP32 sends M29 and replies "~ERR upload cancelled".
//   Any reply starting "~ERR" means the upload was aborted.
//   Per the Parallel-7 flashforge-api-docs wiki, M28 takes the file as exactly <size> RAW
//   bytes (no packet header, no CRC, no per-chunk ack from the printer). The total of all
//   frames must equal <size> or the printer deletes the file at M29.

#include <esp_wifi.h>
#include <WiFi.h>

// ---------- CONFIG ----------
const char* ssid = "Adventurer 3";
const char* pass = "123456789";
const char* ip   = "10.10.100.254";
const int   printerPort = 8899;

const long SERIAL_BAUD = 921600;   // MUST match serial.Serial(..., 921600) in telegram.py

// Move mode safety limits
const float MAX_STEP_XY  = 50.0;    // mm per single move, X and Y
const float MAX_STEP_Z   = 50.0;    // mm per single move, Z (down = nozzle crash risk)
const int   DEFAULT_FEED = 3000;    // mm/min if you don't give one
const int   MAX_FEED     = 6000;    // mm/min cap
const int   Z_MAX_FEED   = 600;     // mm/min cap whenever Z is moving
const unsigned long MOVE_IDLE_TIMEOUT_MS = 120000;  // auto-exit move mode after 2 min of silence

// Upload / print
const long MAX_UPLOAD_BYTES = 20000000;
const bool PRINTER_ACKS_EACH_CHUNK = false;  // docs: raw stream, the printer does not ack each chunk
const unsigned long CHUNK_ACK_TIMEOUT_MS = 10000;
const unsigned long UPLOAD_SERIAL_TIMEOUT_MS = 8000;
const bool SEND_M24_AFTER_M23 = false;       // docs: "~M23 0:/user/<file>" selects AND starts the print
const size_t UPLOAD_BUF_SIZE = 4352;         // max bytes per serial frame
const unsigned long KEEPALIVE_MS = 30000;    // docs suggest M27 as a keep-alive while idle

// ---------- STATE ----------
String cmd = "";

bool wantConnected = false;       // false after "Release" so the idle check leaves it alone
bool homed = false;               // true once HomeAll succeeded since this ESP32 booted
bool moveMode = false;            // we are in the jog loop
bool relativeActive = false;      // printer may be in G91 (relative) right now
unsigned long lastMoveInput = 0;
unsigned long lastLinkCheck = 0;
String linkNote = "";             // e.g. "reconnected", prepended to the next reply

static uint8_t upBuf[UPLOAD_BUF_SIZE];

WiFiClient flashy;

//SESSION CONTROL
String ControlSession = "~M601 S1\r\n";
String ReleaseSession = "~M602\r\n";

//INFORMATION
String GetInfo = "~M115\r\n";
String GetPrintStatus = "~M27\r\n";
String GetTemps = "~M105\r\n";
String GetEndStop = "~M119\r\n";
String GetCurrentPosition = "~M114\r\n";
String ListFiles = "~M661\r\n";   // reply: "ok" first, THEN "D::/path::/path..." (see doFiles)

//JOB CONTROL
String ResumePrint = "~M24\r\n";
String PausePrint = "~M25\r\n";
String CancelPrint = "~M26\r\n";

//MOTION
String HomeAllAxes = "~G28\r\n";
String HomeXY = "~G28 X Y\r\n";

// ---------- forward declarations ----------
void reply(String text);
bool readLine(WiFiClient &c, String &line, unsigned long deadline);
bool printerCommand(WiFiClient &c, const String &gcode, unsigned long timeoutMs, String &body);
bool sendRecievePrint(WiFiClient &CLIENT, const String &message, unsigned long timeoutMs);
bool sendRecievePrint(WiFiClient &CLIENT, const String &message);
bool connectSocket();
bool ensureConnected(bool interactive);

// ---------- output helper ----------
// Every reply is "~" + text + newline + blank line. Never sends an empty reply.
void reply(String text) {
  String out = linkNote + text;
  linkNote = "";
  if (out.length() == 0) out = "OK";
  Serial.print('~');
  Serial.print(out);
  if (!out.endsWith("\n")) Serial.print('\n');
  Serial.print('\n');
}

// ---------- printer I/O ----------
// Reads one line from the printer without ever blocking past the deadline.
bool readLine(WiFiClient &c, String &line, unsigned long deadline) {
  line = "";
  while ((long)(deadline - millis()) > 0) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') return true;
      if (ch != '\r') line += ch;
    }
    if (!c.connected() && !c.available()) return false;
    delay(2);
  }
  return false;
}

// Sends one G-code line, collects the reply until "ok".
// Returns true if the printer said "ok". body = the useful reply lines.
bool printerCommand(WiFiClient &c, const String &gcode, unsigned long timeoutMs, String &body) {
  body = "";
  while (c.available()) c.read();               // throw away stale bytes
  if (c.print(gcode) == 0) return false;        // socket is dead
  unsigned long deadline = millis() + timeoutMs;
  while ((long)(deadline - millis()) > 0) {
    String line;
    if (!readLine(c, line, deadline)) break;
    line.trim();
    if (line.length() == 0) continue;
    if (line == "ok") return true;
    if (line.startsWith("CMD ") && line.endsWith("Received.")) continue;  // printer echo noise
    body += line + "\n";
  }
  return false;
}

// Original entry point, kept so your if-chain still works.
// Reconnects if needed, sends, and ALWAYS replies (never an empty "~").
bool sendRecievePrint(WiFiClient &CLIENT, const String &message, unsigned long timeoutMs) {
  if (!ensureConnected(true)) return false;
  String body;
  bool ok = printerCommand(CLIENT, message, timeoutMs, body);
  if (ok) {
    reply(body.length() ? body : String("OK"));
  } else if (body.length()) {
    reply("ERR no 'ok' from printer. Partial reply:\n" + body);
  } else {
    reply("NO RESPONSE from printer (timeout)");
  }
  return ok;
}
bool sendRecievePrint(WiFiClient &CLIENT, const String &message) {
  return sendRecievePrint(CLIENT, message, 5000);
}

// ---------- connection handling ----------
bool connectSocket() {
  flashy.stop();
  for (int attempt = 0; attempt < 3; attempt++) {
    if (flashy.connect(ip, printerPort, 3000)) {
      flashy.setNoDelay(true);
      String body;
      printerCommand(flashy, ControlSession, 5000, body);   // take control (M601 S1)
      if (body.indexOf("Control Success") >= 0) return true;
      flashy.stop();   // "Control Failed." = old session still open on the printer (can take ~30 s)
    }
    delay(500);
  }
  return false;
}

// Makes sure Wi-Fi and the printer socket are up, reconnecting if needed.
// interactive = true: send an ERR reply on failure. false: stay silent (idle check).
// On a successful reconnect, linkNote is set so the NEXT reply says so.
bool ensureConnected(bool interactive) {
  wantConnected = true;
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk && flashy.connected()) return true;

  moveMode = false;   // any link loss cancels the jog loop

  if (!wifiOk) {
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    unsigned long s = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - s < 15000) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
      if (interactive) reply("ERR Wi-Fi to the printer is down and could not reconnect");
      return false;
    }
  }
  if (!connectSocket()) {
    if (interactive) reply("ERR could not get control of the printer (socket or M601 failed; an old session can take ~30 s to expire, try again)");
    return false;
  }

  String note = "(reconnected to printer)\n";
  if (relativeActive) {                       // printer may still be in G91 from before the drop
    String body;
    if (printerCommand(flashy, "~G90\r\n", 5000, body)) {
      relativeActive = false;
      note += "(move mode was cancelled, back to absolute)\n";
    } else {
      note += "WARNING: could not confirm G90, printer may still be in relative mode\n";
    }
  }
  linkNote += note;
  return true;
}

// Called from loop() when no serial data is waiting.
void idleReconnectCheck() {
  if (!wantConnected) return;
  if (millis() - lastLinkCheck < 5000) return;
  lastLinkCheck = millis();
  if (WiFi.status() == WL_CONNECTED && flashy.connected()) {
    static unsigned long lastKeepAlive = 0;
    if (millis() - lastKeepAlive > KEEPALIVE_MS) {   // cheap M27 so the session doesn't go stale
      lastKeepAlive = millis();
      String body;
      printerCommand(flashy, GetPrintStatus, 2000, body);
    }
    return;
  }
  ensureConnected(false);
}

// ---------- small parsing helpers ----------
bool isNumberToken(const String &t, bool allowSign, bool allowDot) {
  if (t.length() == 0 || t.length() > 10) return false;
  unsigned int i = 0;
  if (allowSign && (t[0] == '-' || t[0] == '+')) i = 1;
  bool digit = false, dot = false;
  for (; i < t.length(); i++) {
    char c = t[i];
    if (c >= '0' && c <= '9') digit = true;
    else if (c == '.' && allowDot && !dot) dot = true;
    else return false;
  }
  return digit;
}

// Splits on spaces/commas/tabs. Returns count (maxTokens+1 if there were too many).
int splitTokens(String s, String *out, int maxTokens) {
  s.replace(',', ' ');
  s.replace('\t', ' ');
  int n = 0;
  unsigned int i = 0;
  while (i < s.length()) {
    while (i < s.length() && s[i] == ' ') i++;
    if (i >= s.length()) break;
    unsigned int j = i;
    while (j < s.length() && s[j] != ' ') j++;
    if (n >= maxTokens) return maxTokens + 1;
    out[n++] = s.substring(i, j);
    i = j;
  }
  return n;
}

// Only letters, digits, . _ -  and a printer-friendly extension.
bool safeFilename(String name) {
  if (name.length() < 1 || name.length() > 60) return false;
  if (name[0] == '.') return false;
  for (unsigned int i = 0; i < name.length(); i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  name.toLowerCase();
  return name.endsWith(".gx") || name.endsWith(".gcode") || name.endsWith(".g");
}

// ---------- printer idle check (M119) ----------
// Fails closed: if MachineStatus is not exactly READY (or can't be read) we say "not idle".
bool printerIsIdle(String &raw) {
  String body;
  printerCommand(flashy, GetEndStop, 4000, body);
  raw = body;
  int p = body.indexOf("MachineStatus:");
  if (p < 0) return false;
  int e = body.indexOf('\n', p);
  String status = body.substring(p + 14, e < 0 ? body.length() : e);
  status.trim();
  return status == "READY";
}

// ---------- PING ----------
void doPing() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool sockOk = flashy.connected();
  String t = "PONG (ESP32 is alive)\n";
  t += "wifi=" + String(wifiOk ? "OK" : "DOWN");
  if (wifiOk) t += " rssi=" + String(WiFi.RSSI()) + " dBm";
  t += "\nsocket=" + String(sockOk ? "OK" : "DOWN");
  t += "\nuptime=" + String(millis() / 1000) + " s";
  t += "\nhomed=" + String(homed ? "yes" : "no");
  t += "\nmove_mode=" + String(moveMode ? "ON" : "off");
  String body;
  bool printerOk = false;
  if (wifiOk && sockOk) printerOk = printerCommand(flashy, GetTemps, 2000, body);
  t += "\nprinter=" + String(printerOk ? "RESPONDING" : "NO RESPONSE");
  reply(t);
}

// ---------- RELATIVE MOVE MODE ----------
void enterMoveMode() {
  if (!ensureConnected(true)) return;
  if (!homed) {
    reply("ERR Not homed yet. Send HomeAll first, then Relative Move Mode.");
    return;
  }
  String raw;
  if (!printerIsIdle(raw)) {
    reply("ERR printer is not idle (printing or busy). Refusing to move.\nMachineStatus reply:\n" +
          (raw.length() ? raw : String("(no MachineStatus in reply)")));
    return;
  }
  String body;
  relativeActive = true;                       // assume G91 took effect until G90 is confirmed
  if (!printerCommand(flashy, "~G91\r\n", 5000, body)) {
    printerCommand(flashy, "~G90\r\n", 5000, body);   // best effort undo
    relativeActive = false;
    reply("ERR G91 not confirmed. Move mode NOT entered.");
    return;
  }
  moveMode = true;
  lastMoveInput = millis();
  reply("Move mode ON (relative). Send:  x, y, z  or  x, y, z, feed\n"
        "Limits per move: X/Y " + String(MAX_STEP_XY, 0) + " mm, Z " + String(MAX_STEP_Z, 0) +
        " mm. Default feed " + String(DEFAULT_FEED) + ".\n"
        "Send  e e e  to exit. Auto-exit after " + String(MOVE_IDLE_TIMEOUT_MS / 1000) + " s idle.");
}

void exitMoveMode(bool timedOut) {
  if (timedOut) lastMoveInput = millis();      // if this fails, retry only after another idle period
  if (!ensureConnected(true)) return;
  if (!moveMode) {                             // link dropped: ensureConnected already handled G90
    reply("Move mode is OFF.");
    return;
  }
  String body;
  if (printerCommand(flashy, "~G90\r\n", 5000, body)) {
    relativeActive = false;
    moveMode = false;
    reply(String("Move mode OFF. Back to absolute positioning.") + (timedOut ? " (idle timeout)" : ""));
  } else {
    reply("WARNING: G90 not confirmed. Printer may still be in relative mode.\n"
          "Move mode stays ON. Send  e e e  to retry.");
  }
}

void handleMoveInput(String in) {
  lastMoveInput = millis();

  String low = in;
  low.toLowerCase();
  low.replace(',', ' ');
  while (low.indexOf("  ") >= 0) low.replace("  ", " ");
  low.trim();
  if (low == "e e e" || low == "exit") { exitMoveMode(false); return; }

  String tok[5];
  int n = splitTokens(in, tok, 4);
  if (n < 3 || n > 4 ||
      !isNumberToken(tok[0], true, true) || !isNumberToken(tok[1], true, true) ||
      !isNumberToken(tok[2], true, true) || (n == 4 && !isNumberToken(tok[3], false, false))) {
    reply("Bad input. Send:  x, y, z  or  x, y, z, feed   (numbers only, e e e to exit)");
    return;
  }

  float x = tok[0].toFloat(), y = tok[1].toFloat(), z = tok[2].toFloat();
  int feed = (n == 4) ? tok[3].toInt() : DEFAULT_FEED;

  if (fabs(x) > MAX_STEP_XY) { reply("Rejected: X step " + String(x, 1) + " exceeds +/-" + String(MAX_STEP_XY, 0) + " mm. Nothing sent."); return; }
  if (fabs(y) > MAX_STEP_XY) { reply("Rejected: Y step " + String(y, 1) + " exceeds +/-" + String(MAX_STEP_XY, 0) + " mm. Nothing sent."); return; }
  if (fabs(z) > MAX_STEP_Z)  { reply("Rejected: Z step " + String(z, 1) + " exceeds +/-" + String(MAX_STEP_Z, 0) + " mm. Nothing sent."); return; }
  if (feed <= 0) { reply("Rejected: feed must be above 0."); return; }
  if (feed > MAX_FEED) feed = MAX_FEED;
  if (z != 0 && feed > Z_MAX_FEED) feed = Z_MAX_FEED;
  if (x == 0 && y == 0 && z == 0) { reply("Nothing to move (0, 0, 0)."); return; }

  // The link may have dropped and been restored; relative mode would then be gone.
  if (!ensureConnected(true)) return;
  if (!moveMode) {
    reply("Connection was restored and move mode was cancelled. Nothing sent.");
    return;
  }

  String g = String("~G1 X") + String(x, 2) + " Y" + String(y, 2) + " Z" + String(z, 2) + " F" + String(feed) + "\r\n";
  String body;
  if (printerCommand(flashy, g, 15000, body)) {
    reply("Moved X" + String(x, 2) + " Y" + String(y, 2) + " Z" + String(z, 2) + " F" + String(feed));
  } else {
    reply("NO CONFIRMATION from printer. The move may or may not have happened.");
  }
}

// ---------- FILES / PRINT ----------
void doPrint(String name) {
  name.trim();
  // Files listed by M661 look like "/data/Name.gx"; files we upload live in "0:/user/".
  String path;
  if (name.startsWith("/data/")) {
    String f = name.substring(6);
    if (safeFilename(f)) path = "/data/" + f;
  } else if (safeFilename(name)) {
    path = "0:/user/" + name;
  }
  if (path.length() == 0) {
    reply("ERR bad filename. Use letters, digits . _ - only, ending in .gx / .gcode / .g (optionally prefixed /data/)");
    return;
  }
  if (!ensureConnected(true)) return;
  String raw;
  if (!printerIsIdle(raw)) {
    reply("ERR printer is not idle. Refusing to start a print.\nMachineStatus reply:\n" +
          (raw.length() ? raw : String("(no MachineStatus in reply)")));
    return;
  }
  String g = "~M23 " + path + "\r\n";
  if (!SEND_M24_AFTER_M23) {
    sendRecievePrint(flashy, g, 10000);
    return;
  }
  String body;
  if (!printerCommand(flashy, g, 10000, body)) {
    reply("ERR M23 not confirmed.\n" + body);
    return;
  }
  sendRecievePrint(flashy, ResumePrint, 5000);   // M24
}

// M661: the printer sends "ok" FIRST, then the list "D::/path::/path..." as a separate line.
void doFiles() {
  if (!ensureConnected(true)) return;
  String body;
  if (!printerCommand(flashy, ListFiles, 5000, body)) { reply("NO RESPONSE from printer (timeout)"); return; }
  // The list may already be in `body` (if it came before "ok") or still arriving, and it may
  // have no trailing newline, so collect raw bytes until the printer goes quiet.
  String raw = body;
  unsigned long last = millis(), deadline = millis() + 4000;
  while ((long)(deadline - millis()) > 0 && millis() - last < 1000) {
    while (flashy.available()) {
      char c = flashy.read();
      if (c != '\r') raw += c;
      last = millis();
    }
    delay(5);
  }
  while (raw.indexOf("\n\n") >= 0) raw.replace("\n\n", "\n");   // a blank line would end the reply early
  int p = raw.indexOf("D:");
  if (p >= 0) {
    String list = raw.substring(p);
    list.replace("::", "\n");
    list = list.substring(1);                                     // drops the leading "D"
    list.trim();
    if (list.length() > 3500) list = list.substring(0, 3500) + "\n...(truncated, Telegram limit)";
    reply("Files (send:  Print <path exactly as listed>):\n" + list);
    return;
  }
  raw.trim();
  reply("No 'D::' file list found. Raw reply from printer:\n" + (raw.length() ? raw : String("(nothing after ok)")));
}

// ---------- UPLOAD ----------
void abortUpload(const String &why) {
  String body;
  if (flashy.connected()) printerCommand(flashy, "~M29\r\n", 5000, body);
  delay(300);
  while (Serial.available()) Serial.read();      // drop whatever the laptop was still sending
  reply("ERR " + why);
}

void uploadLoop(const String &name, long size) {
  Serial.setTimeout(UPLOAD_SERIAL_TIMEOUT_MS);
  uint32_t chunks = 0;
  long sent = 0;                                 // bytes forwarded so far; must equal size at the end
  while (true) {
    uint8_t hdr[2];
    if (Serial.readBytes(hdr, 2) != 2) { abortUpload("upload aborted: no data from laptop"); break; }
    uint16_t len = ((uint16_t)hdr[0] << 8) | hdr[1];

    if (len == 0) {                              // end of file
      if (sent != size) { abortUpload("size mismatch: sent " + String(sent) + " of " + String(size) + " bytes"); break; }
      String body;
      bool ok = printerCommand(flashy, "~M29\r\n", 15000, body);
      // docs: success is `ack: "ok"`, failure is "File Is Not Aviable" (sic)
      if ((ok || body.indexOf("ok") >= 0) && body.indexOf("Not Aviable") < 0) {
        reply("UPLOAD DONE " + name + " (" + String(chunks) + " chunks). Send:  Print " + name);
      } else {
        reply("ERR M29 not confirmed, file may be incomplete.\n" + body);
      }
      break;
    }
    if (len == 0xFFFF) { abortUpload("upload cancelled"); break; }
    if (len > UPLOAD_BUF_SIZE) { abortUpload("frame too large (" + String(len) + ")"); break; }

    if (Serial.readBytes(upBuf, len) != len) { abortUpload("upload aborted: short frame from laptop"); break; }
    if (!flashy.connected()) { abortUpload("upload aborted: printer socket closed"); break; }
    if (sent + len > size) { abortUpload("more data than the declared size"); break; }

    while (flashy.available()) flashy.read();
    if (flashy.write(upBuf, len) != len) { abortUpload("upload aborted: write to printer failed"); break; }

    if (PRINTER_ACKS_EACH_CHUNK) {
      unsigned long deadline = millis() + CHUNK_ACK_TIMEOUT_MS;
      bool gotOk = false;
      while (!gotOk && (long)(deadline - millis()) > 0) {
        String line;
        if (!readLine(flashy, line, deadline)) break;
        line.trim();
        if (line == "ok") gotOk = true;
      }
      if (!gotOk) { abortUpload("printer did not acknowledge chunk " + String(chunks + 1)); break; }
    }
    sent += len;
    chunks++;
    reply("ACK " + String(chunks));
  }
  Serial.setTimeout(1000);
}

void doUpload(String args) {
  args.trim();
  int sp = args.indexOf(' ');
  if (sp < 0) { reply("ERR usage: Upload <size> <name>"); return; }
  String sizeTok = args.substring(0, sp);
  String name = args.substring(sp + 1);
  name.trim();
  if (!isNumberToken(sizeTok, false, false)) { reply("ERR size must be a whole number of bytes"); return; }
  long size = sizeTok.toInt();
  if (size <= 0 || size > MAX_UPLOAD_BYTES) { reply("ERR size out of range (max " + String(MAX_UPLOAD_BYTES) + " bytes)"); return; }
  if (!safeFilename(name)) { reply("ERR bad filename. Use letters, digits . _ - only, ending in .gx / .gcode / .g"); return; }

  if (!ensureConnected(true)) return;
  String raw;
  if (!printerIsIdle(raw)) {
    reply("ERR printer is not idle. Refusing to upload.\nMachineStatus reply:\n" +
          (raw.length() ? raw : String("(no MachineStatus in reply)")));
    return;
  }
  String body;
  String g = String("~M28 ") + String(size) + " 0:/user/" + name + "\r\n";
  if (!printerCommand(flashy, g, 8000, body)) {
    reply("ERR printer did not accept the upload (M28).\n" + body);
    return;
  }
  reply("READY");
  uploadLoop(name, size);
}

// ---------- SETUP / LOOP ----------
void setup() {
  Serial.setRxBufferSize(8192);     // must be called before begin(); upload frames are ~4 KB
  Serial.begin(SERIAL_BAUD);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print('~');
    Serial.print("Connecting");
    Serial.print('\n');
  }

  Serial.print("Adventurer 3 connected.\n");

  while (!connectSocket()) {
    Serial.print("~Waiting for printer session (socket failed or Control Failed, old session may still be open)\n\n");
    delay(1000);
  }
  wantConnected = true;
  reply("Printer connected, session started.");
}

void loop() {
  // Safety: leave jog mode if nobody has typed for a while.
  if (moveMode && (millis() - lastMoveInput > MOVE_IDLE_TIMEOUT_MS)) {
    exitMoveMode(true);
  }

  if (Serial.available() == 0) idleReconnectCheck();

  //get the command from telegram
  cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  // While in move mode, EVERYTHING typed is a move (or "e e e").
  if (moveMode) {
    handleMoveInput(cmd);
    return;
  }

  if (cmd == "Get Info") {
    sendRecievePrint(flashy, GetInfo);
  } else if (cmd == "Get Status") {
    sendRecievePrint(flashy, GetPrintStatus);
  } else if (cmd == "Get Temperatures") {
    sendRecievePrint(flashy, GetTemps);
  } else if (cmd == "Get Endstop") {
    sendRecievePrint(flashy, GetEndStop);
  } else if (cmd == "Get Position") {
    sendRecievePrint(flashy, GetCurrentPosition);
  } else if (cmd == "HomeAll") {
    if (sendRecievePrint(flashy, HomeAllAxes, 30000)) homed = true;   // homing is slow
  } else if (cmd == "Resume") {
    sendRecievePrint(flashy, ResumePrint);
  } else if (cmd == "Pause") {
    sendRecievePrint(flashy, PausePrint);
  } else if (cmd == "Cancel") {
    sendRecievePrint(flashy, CancelPrint);
  } else if (cmd == "Release") {
    if (flashy.connected()) {
      sendRecievePrint(flashy, ReleaseSession);
      flashy.stop();
    } else {
      reply("Already released.");
    }
    wantConnected = false;           // the next real command reconnects automatically
  } else if (cmd.equalsIgnoreCase("Ping")) {
    doPing();
  } else if (cmd.equalsIgnoreCase("Files")) {
    doFiles();
  } else if (cmd.startsWith("Print ")) {
    doPrint(cmd.substring(6));
  } else if (cmd.startsWith("Upload ")) {
    doUpload(cmd.substring(7));
  } else if (cmd.equalsIgnoreCase("Relative Move Mode")) {
    enterMoveMode();
  } else {
    reply("Please enter a known command");
  }
}
