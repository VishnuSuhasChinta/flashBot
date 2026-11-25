# flashBot

Control a **Flashforge Adventurer 3** from Telegram, through an ESP32 that talks to the printer over Wi-Fi.

Part of [ProteanCNC](https://github.com/VishnuSuhasChinta/ProteanCNC): this is the control layer for AI-supervised printing.

```
Telegram  <->  telegram.py (laptop)  <->  USB serial  <->  ESP32 (flashBot.ino)  <->  Wi-Fi TCP :8899  <->  Adventurer 3
```

## What it can do
- Start, pause, resume and cancel prints; list files on the printer
- Upload a sliced `.gx` / `.gcode` file by sending it to the bot (up to 20 MB, with progress)
- Read temperatures, print status, end stops and position; home all axes
- Jog the axes in a relative move mode with step, feed-rate and Z-speed limits, which exits on its own after 2 minutes idle
- Health check (`Ping`) that works even when the printer is off
- Only answers chat IDs on an allowlist

## Files
| File | What it is |
|---|---|
| `telegram.py` | Telegram bot ↔ serial relay, including the framed `.gx` upload |
| `flashBot/flashBot.ino` | ESP32 firmware: serial ↔ printer bridge. The full command list and upload protocol are in its header comment |
| `connect.py` | First proof of concept: talks to the printer's TCP port directly from Python |

## Setup
1. Put the printer in Wi-Fi AP mode. `flashBot.ino` uses the printer's factory AP name, password and IP (`10.10.100.254:8899`); change them at the top of the file if yours differ.
2. Flash `flashBot/flashBot.ino` to an ESP32 (Arduino IDE or `arduino-cli`) and plug it into the laptop over USB.
3. Install the Python side and set your bot details as environment variables. **Never put the token in the code.**
   ```bash
   pip install -r requirements.txt
   export TELEGRAM_BOT_TOKEN=...          # from @BotFather
   export TELEGRAM_CHAT_IDS=123456789     # comma-separated chats allowed to control the printer
   export SERIAL_PORT=/dev/ttyUSB0        # default COM2
   python telegram.py
   ```

## Printer protocol
Commands follow the community [Flashforge API docs (Parallel-7)](https://github-wiki-see.page/m/Parallel-7/flashforge-api-docs/wiki/Adventurer-3-Series).
