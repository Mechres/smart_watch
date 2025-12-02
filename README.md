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
      - Usage: Send text to display on the watch.
      - Format: `Title|Body` or `Title\nBody` (e.g., `Message|Hello World`).
    - **Control** (`b31cb75e-410c-29ba-0b45-9da7834df66e`):
      - Properties: Read, Write, Notify.
      - Usage: Send commands to control watch functions.
      - Commands:
        - `wifi_on`: Turn WiFi on.
        - `wifi_off`: Turn WiFi off.
        - `screen_on`: Wake the screen.
        - `screen_off`: Turn the screen off.
        - `time=<timestamp>`: Set system time (Unix epoch seconds).
- **WiFi**:
  - Connects to configured WiFi networks.
  - **Weather**: Fetches current weather data (Temperature & Condition) from Open-Meteo API.
  - *Note: WiFi is kept off by default to conserve power and only enabled for updates.*

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
| **Battery** | LiPo Battery | Voltage Divider on GPIO2 |

**Pinout Configuration:**
- **I2C SDA**: GPIO 8
- **I2C SCL**: GPIO 9
- **Battery ADC**: GPIO 2
- **Tap Interrupt**: GPIO 1 (Configured for ADXL345)

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
    *Configure WiFi credentials and other settings if necessary.*

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

## Project Structure

```
smart_watch/
├── main/
│   ├── main.c           # Entry point, initialization, and main loop
│   ├── menu.c           # Menu system logic and rendering
│   ├── watchfaces.c     # Implementation of various watchfaces
│   ├── display.c        # SH1106 OLED driver
│   ├── sensors.c        # Sensor drivers (ADXL345, AHT10)
│   ├── ble_manager.c    # BLE GAP/GATT handling
│   ├── wifi_manager.c   # WiFi connection management
│   ├── weather.c        # HTTP client for weather API
│   ├── battery.c        # ADC reading for battery level
│   └── ...
├── CMakeLists.txt       # Project build configuration
└── README.md            # This file
```

## License

This project is open source. Feel free to modify and distribute.
