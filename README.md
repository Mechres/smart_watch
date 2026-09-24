# ESP32-C3 Smart Watch

A feature-rich smartwatch firmware built for the **ESP32-C3 SuperMini** development board using the **ESP-IDF** framework. This project integrates an OLED display, accelerometer, and various sensors to provide a comprehensive wearable experience with multiple watchfaces, fitness tracking, weather updates, and smartphone connectivity.

## Features

### 🕒 Watchfaces
Choose from a variety of stylish and functional watchfaces:
- **Digital**: Large, easy-to-read time with date, steps, and environment info.
- **Analog Style**: Classic analog clock face with digital date and step counter.
- **Minimal**: Clean interface focusing on time and date.
- **Compact**: Information-dense layout with time, date, battery, weather, steps, and notifications.
- **Terminal**: Retro command-line interface style.
- **Matrix**: "Digital rain" animation effect.
- **Cats**: Animated cat pixel art.

### 🏃 Fitness & Sensors
- **Pedometer**: Accurate step counting using the ADXL345 accelerometer.
- **Environment**: Real-time Temperature and Humidity readings (via SHT3x/AHT10).
- **Motion Detection**: Wake-on-lift functionality to save battery.

### 📡 Connectivity
- **Bluetooth Low Energy (BLE)**:
  - **Service UUID**: `1d8a503d-e931-369f-164b-6f10596a178d`
  - **Characteristics**:
    - **Notification** (`2480757d-4f07-9fa5-0f48-e4125a9bdab8`):
      - Properties: Read, Write, Notify.
      - Write format: `Title|Body` or `Title\nBody` (e.g., `Message|Hello World`).
      - **Read wire format** (explicit, not a raw struct):
        ```
        byte 0: flags (bit0=has_data, bit1=has_unread)
        byte 1: title_len (0..31)
        byte 2: body_len  (0..127)
        bytes 3..: title bytes, then body bytes
        ```
    - **Control** (`b31cb75e-410c-29ba-0b45-9da7834df66e`):
      - Properties: Read, Write, Notify.
      - Commands:
        - `wifi_on` / `wifi_off`: Toggle WiFi.
        - `screen_on` / `screen_off`: Toggle the display.
        - `time=<timestamp>`: Set system time (Unix epoch seconds).
        - `weather=<temp>,<code>`: Set weather (e.g., `weather=24.5,1`).
        - `ota=<https-url>`: Download firmware from an HTTPS URL and reboot into it.
          Progress/status is notified back as `ota_status=...` / `ota_progress=NN`.
    - **Battery** (`12345678-90ab-cdef-1234-567890abcdef`):
      - Properties: Read, Notify. Value: battery percentage (0-100), uint8.
    - **Steps** (`fedcba98-7654-3210-fedc-ba9876543210`):
      - Properties: Read, Notify. Value: step count, uint32 native endian.
  - Advertising: fast interval while the screen is active; stops after ~5 minutes
    without a connection; restarts when the user wakes the watch.
- **WiFi**:
  - Connects to configured WiFi networks (lazily initialized on first use).
  - *Note: WiFi is kept off by default to conserve power.*

### 🔄 OTA Updates
- Partition table: `nvs + otadata + phy + ota_0 + ota_1` (two 1984 KB app slots).
- Trigger via BLE control command: `ota=https://example.com/smart_watch.bin`
- Uses `esp_https_ota` with the built-in certificate bundle (HTTPS required).
- On success the watch reboots into the new image automatically.

### ⚙️ System
- **Settings Menu**:
  - Adjustable **Screen Brightness**.
  - Configurable **Screen Timeout**.
  - Adjustable **Motion Sensitivity**.
  - Reboot and Power Off options.
- **Tools**:
  - **Stopwatch**: Simple start/stop/reset timer.
  - **Flashlight**: Turns the screen white for emergency lighting.
  - **System Info**: View Uptime, Heap Memory, IP Address, and MAC Address.
- **Battery Monitoring**: Voltage and percentage display.

## Hardware Requirements

| Component | Description | Connection / Pin |
|-----------|-------------|------------------|
| **MCU** | ESP32-C3 SuperMini | - |
| **Display** | SH1106 OLED (128x64) | I2C (SDA: GPIO8, SCL: GPIO9) |
| **Accel** | ADXL345 | I2C (SDA: GPIO8, SCL: GPIO9) |
| **Sensor** | AHT10 / SHT3x (Temp/Hum) | I2C (SDA: GPIO8, SCL: GPIO9) |
| **Buttons** | Push Buttons | GPIO Input (Active High/Low depending on config) |
| **Battery** | LiPo Battery | Voltage Divider on GPIO4 |

**Pinout Configuration:**
- **I2C SDA**: GPIO 8
- **I2C SCL**: GPIO 9
- **Battery ADC**: GPIO 4 (ADC1 Channel 4)
- **Tap Interrupt**: GPIO 1 (Configured for ADXL345)
- **Buttons**: UP (GPIO 7), DOWN (GPIO 6), OK (GPIO 5)

## Installation

### Prerequisites
- **ESP-IDF v5.5** (or compatible version).
- Python 3.11+.

### Build & Flash

1.  **Clone the repository:**
    ```bash
    git clone <repository-url>
    cd smart_watch
    ```

2.  **Set up the environment:**
    (If not already done via your IDE)
    ```bash
    . $HOME/esp/esp-idf/export.sh
    ```

3.  **Configure the project:**
    ```bash
    idf.py menuconfig
    ```
    *Configure WiFi credentials under "Smart Watch Configuration" and other settings if necessary.*

4.  **Build the project:**
    ```bash
    idf.py build
    ```

5.  **Flash to device:**
    ```bash
    idf.py -p <PORT> flash monitor
    ```
    *Replace `<PORT>` with your device's serial port (e.g., `COM3` on Windows or `/dev/ttyUSB0` on Linux).*

## Usage

- **Navigation**:
  - **UP / DOWN**: Scroll through menus or change values.
  - **OK**: Select item / Enter menu / Wake screen.
  - **Back**: Return to previous menu (usually the last item in the list).
- **Shortcuts**:
  - Press **OK** on the watchface to enter the Main Menu.
  - **Wake Screen**: Lift wrist (Motion) or press any button.

### 📱 Companion Apps

See [`companion/`](companion/) — protocol in [`companion/PROTOCOL.md`](companion/PROTOCOL.md).

| App | Path | Stack |
|-----|------|-------|
| Desktop test tool | `companion/desktop/` | Python + bleak CLI |
| Android (full suite) | `companion/android/` | Kotlin + Jetpack Compose |

Android features: scan/connect, battery + steps, send notifications, time sync, weather push, screen/wifi controls, OTA with progress, find-phone ring, music media keys, event log.

## Project Structure

```
smart_watch/
├── main/                 # Firmware sources
├── companion/
│   ├── PROTOCOL.md       # BLE wire protocol
│   ├── desktop/          # Python bleak CLI
│   └── android/          # Native Android app
├── CMakeLists.txt
├── partitions.csv
├── PLAN.md               # Optimization plan (not for commit)
└── README.md
```

## License

This project is open source. Feel free to modify and distribute.
