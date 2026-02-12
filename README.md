# SenseESP Barometer + Tank Level (ESP32 → Signal K)

Publishes BMP280-based pressure/temperature and an analog freshwater tank level to Signal K using SenseESP on ESP32. Includes manual calibration with a single UI card, moving averages, and configurable capacity and tank identifier.

## Features
- BMP280: pressure (`environment.outside.pressure`, Pa) and temperature (`environment.outside.temperature`, K).
- Analog tank sensor (XDB401 on ADC1/GPIO36) with `CurveInterpolator` calibration.
- Tank outputs: `tanks.freshWater.<tankId>.currentLevel` (ratio), `.currentVolume` (m3), `.capacity` (m3).
- Single calibration controller card: continuous sampling during calibration; persists curve.
- Configurable capacity in litres and `tankId` string; changes reflect immediately in volume/capacity outputs.
- Hostname `barometer`; optional OTA via `barometer.local`; AP SSID `barometer-ap`.

## Signal K Paths
- Barometer:
  - `environment.outside.pressure` (Pa)
  - `environment.outside.temperature` (K)
- Tank (fresh water):
  - `tanks.freshWater.<tankId>.currentLevel` (ratio)
  - `tanks.freshWater.<tankId>.currentVolume` (m3)
  - `tanks.freshWater.<tankId>.capacity` (m3)

## Calibration Flow (Tank)
The calibration UI appears under “Fresh Water Tank Calibration” with a single-letter action control:
- `S`: Start (begin collecting averaged raw samples at 1 Hz)
- `F`: Finish (compute endpoints, normalize samples, update and save curve)
- `A`: Abort (stop sampling; no curve changes)
- `C`: Clear (erase curve and save; resets to default endpoints)

Defaults: On first boot or if fewer than two samples exist, the curve is initialized with endpoints `(raw=0.0 → level=0.0)` and `(raw=4095.0 → level=1.0)`.

Finish behavior: To ensure the full sensor range is covered and avoid out-of-range outputs, the calibration finish step always includes the full-range guards `(0.0 → 0.0)` and `(4095.0 → 1.0)` in addition to the observed empty/full endpoints and normalized samples.

## Configuration
- Capacity (litres): path `/tanks/freshwater/capacityLitres`. Used to compute volume and capacity (m3) outputs.
- Tank identifier: path `/tanks/freshwater/tankId`. Used in Signal K path construction. Changes require a restart to rebuild paths.
- Curve: path `/calibration/tank/curve`. Samples are persisted automatically.

## Hardware & Wiring
- ESP32 Dev Module (or compatible).
- BMP280 on I2C: SDA = GPIO16, SCL = GPIO17; address `0x76` or `0x77`.
- Analog tank sensor on ADC1 VP (GPIO36). ESP32 ADC configured for 12-bit (0..4095) with `ADC_11db` attenuation.

## Build / Upload / Monitor
PlatformIO environments in `platformio.ini`:
- `esp32dev`: USB upload (`upload_port = COM10`, `upload_speed = 921600`) and monitor.
- `esp32dev-ota`: OTA via `espota` to `barometer.local` (optional; USB is more reliable).

CLI examples (using the project venv on Windows):

```powershell
./.venv/Scripts/Activate.ps1
pio run
pio run -t upload
pio device monitor -b 115200
```

## Notes & Troubleshooting
- If BMP280 is not detected, the firmware logs and skips its outputs.
- OTA can be flaky; prefer USB. Ensure device and host share the same network and `barometer.local` resolves.
- Capacity is stored in litres; volume output is m3. Updating capacity in the UI changes outputs immediately.
- Changing `tankId` requires a restart to rebuild Signal K paths.
