# Hikaboshi Desktop Companion

Protocol test tool for the ESP32-C3 smart watch over BLE.

## Setup

```bash
cd companion/desktop
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Linux may need capabilities: `sudo setcap cap_net_raw,cap_net_admin+eip $(readlink -f $(which python3))`

## Usage

Wake the watch first (advertising stops after ~5 min idle).

```bash
python hikaboshictl.py scan
python hikaboshictl.py connect
python hikaboshictl.py battery
python hikaboshictl.py steps
python hikaboshictl.py notify "Message" "Hello from desktop"
python hikaboshictl.py time-sync
python hikaboshictl.py weather 24.5 1
python hikaboshictl.py screen off
python hikaboshictl.py wifi on
python hikaboshictl.py ota https://example.com/smart_watch.bin
python hikaboshictl.py watch   # stream battery/steps/control notifies
```

Skip scanning with `--address AA:BB:CC:DD:EE:FF`.

Protocol: see `../PROTOCOL.md`.
