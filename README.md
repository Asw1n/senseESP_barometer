# SenseESP Barometer (BMP280 → Signal K)

Publishes **barometric pressure** and **ambient temperature** from a BMP280 sensor to a Signal K server using the SenseESP framework on an ESP32 (Hat Labs SN ESP).

This README is written to:
- Help you quickly decide if this project matches your goals.
- Be a durable “notes-to-future-self” reference for maintenance and cloning.

## 1) Function & Functional Design

### What it does
- Reads the BMP280 over I2C once per second.
- Publishes measurements to Signal K:
  - Pressure: `environment.outside.pressure` (Pascals)
  - Temperature: `environment.outside.temperature` (Kelvin)

### Data flow
BMP280  Sensor  SenseESP `RepeatSensor<T>`  Calibration (Linear transform)  Signal K output

### Configuration behavior
- The device hostname is set to `barometer`.
- SenseESP is configured to offer an onboard WiFi access point:
  - SSID: `barometer-ap`
  - Password: `changeme`

If you plan to share this firmware or use it outside a lab environment, change the AP password (and consider disabling the AP once provisioned).

### Calibration
Calibration is supported via two adjustable linear transforms:
- Pressure calibration path: `/calibration/pressure`
- Temperature calibration path: `/calibration/temperature`

These appear as editable configuration items in the SenseESP UI.

### Failure modes
- If the BMP280 is not detected at I2C address `0x76` or `0x77`, the firmware logs an error and does not publish sensor values.

## 2) Hardware & Wiring

### Hardware components
- ESP32 board: Hat Labs SN ESP (or compatible ESP32)
- Sensor: BMP280 (I2C)

### I2C pins
This project uses the Hat Labs I2C pins configured in firmware:
- SDA = GPIO16
- SCL = GPIO17

### Wiring (BMP280  ESP32)
Typical BMP280 breakout wiring:

| BMP280 pin | ESP32 pin |
|---|---|
| VIN / 3V3 | 3.3V |
| GND | GND |
| SDA | GPIO16 |
| SCL | GPIO17 |

Notes:
- Many BMP280 breakouts include I2C pull-ups; if yours doesn, add pull-ups (e.g., 4.7k  to 3.3V).
- BMP280 I2C address is commonly `0x76` or `0x77` (this firmware tries both).

### Optional add-ons
- OLED display (SSD1306 over I2C) is not yet wired into this repo. If you want it, we can add it; you need to know your OLED resolution (128x64 vs 128x32) and confirm its I2C address (usually `0x3C`).

## 3) Software Components

### Frameworks & libraries
- Arduino framework on ESP32 (PlatformIO `framework = arduino`)
- SenseESP (Signal K connectivity, configuration UI, scheduling/event loop)
- Adafruit BMP280 + Adafruit Unified Sensor (sensor driver layer)

### Key implementation choices
- Uses `RepeatSensor<float>` to sample on a fixed interval (currently 1000 ms).
- Converts temperature to Kelvin in code to match Signal K conventions.
- Uses SenseESP logging (`SetupLogging()` plus ESP-IDF style logs enabled via build flags).

### Signal K outputs
The published paths are:
- `environment.outside.pressure` (units: Pa)
- `environment.outside.temperature` (units: K)

If you want different paths (e.g., `environment.inside.*` or `environment.outside.ambientTemperature`), change the `SKOutputFloat(...)` paths in `src/main.cpp`.

## 4) Toolchain & How To Build/Run

### Requirements
- Windows 11
- VS Code
- PlatformIO (VS Code extension recommended)
- USB serial connection to the ESP32

### PlatformIO environments
Configured in `platformio.ini`:
- `esp32dev`: USB upload + serial monitor
- `esp32dev-ota`: OTA upload via `espota` to `barometer.local`

### Build / upload / monitor (CLI)
The VS Code PlatformIO extension does **not** automatically put `pio` on your system `PATH`, so `pio` may not work in a regular terminal.

From the repo root, use one of these options:

**Option A (recommended): use the project venv**

```powershell
./.venv/Scripts/Activate.ps1
pio --version
pio run
pio run -t upload
pio device monitor -b 115200
```

**Option B: don’t activate anything (explicit interpreter)**

```powershell
./.venv/Scripts/python.exe -m platformio --version
./.venv/Scripts/python.exe -m platformio run
./.venv/Scripts/python.exe -m platformio run -t upload
./.venv/Scripts/python.exe -m platformio device monitor -b 115200
```

**Option C: PlatformIO terminal inside VS Code**

Run the command palette action `PlatformIO: New Terminal`, then `pio ...` will work in that terminal.

### Common local tweaks
- Serial port: update `upload_port` and `monitor_port` in `platformio.ini` (currently `COM10`).
- Board variant: change `board = esp32dev` if your Hat Labs board maps better to a specific PlatformIO board definition.

### Troubleshooting
- No sensor values: check wiring, confirm 3.3V power, and verify the I2C address is `0x76` or `0x77`.
- Upload failures: verify COM port, close any other serial monitor, and try lowering `upload_speed`.
- OTA upload fails: confirm mDNS resolution of `barometer.local` and that the device is on the same network.