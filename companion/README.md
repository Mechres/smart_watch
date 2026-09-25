# Hikaboshi Companion Apps

Two tools that speak the watch BLE protocol (`PROTOCOL.md`):

| Path | What |
|------|------|
| `desktop/` | Python CLI test tool (bleak) — scan, battery, notify, OTA, stream |
| `android/` | Native Android app (Kotlin + Compose) — full suite |

## Features (Android)

- Scan / connect / disconnect **Hikaboshi**
- Live **battery** + **steps** + **distance** + **calories** (MTU 256 negotiated for full reads)
- Send **notifications** (`Title|Body`)
- **Time sync**, **weather push**, **screen/wifi** toggles
- **OTA** URL push with status/progress display
- **Find phone** — rings when the watch sends `find_phone`
- **Alarm** — toast + log entry when the watch sends `alarm`
- **Music control** — media keys for `music_*` events from the watch
- Foreground link service so events work while backgrounded
- Event log

## Quick start

### Desktop

```bash
cd desktop
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
python hikaboshictl.py scan
```

### Android

1. Open `android/` in Android Studio (Hedgehog+ / AGP 8.7).
2. Sync Gradle, run on a phone with BLE.
3. Grant Bluetooth (+ notification) permissions when prompted.
4. Wake the watch → Scan → Connect.

Min SDK 31 (Android 12). Protocol: `../PROTOCOL.md`.
