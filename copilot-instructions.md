# AI Agent Instructions (senseESP_barometer)

These instructions apply to any AI agent (GitHub Copilot, ChatGPT, etc.) working in this repository.

## Project intent
- ESP32-based node using **SenseESP** to publish BMP280 measurements and an analog **fresh water tank level** to a **Signal K** server.
- Tank level is calibrated via a single card using **CurveInterpolator**; publishes current level (ratio), current volume (m3), and capacity (m3).
- Keep the project small, readable, and easy to revive years later.

## Hardware assumptions
- Board: ESP32 (Hat Labs SN ESP or compatible)
- I²C pins are explicitly set in firmware: **SDA=GPIO16**, **SCL=GPIO17**.
- BMP280 I²C address: try `0x76`, then `0x77`.
 - Analog tank sensor: **ADC1 VP (GPIO36)** with **ADC_11db** attenuation and **12-bit** resolution (0..4095 counts).

## Software constraints
- Framework: Arduino on ESP32 via PlatformIO.
- Core libs: SenseESP (v3.x), ArduinoJson, Adafruit BMP280 (+ Adafruit Unified Sensor).
- Event loop: must call `event_loop()->tick()` in `loop()`.
- Transforms used: `RepeatSensor<float>`, `MovingAverage`, `CurveInterpolator`, custom `LevelToVolume`.
- OTA is optional (`barometer.local`); prefer USB uploads for reliability.

## Development workflow
- Prefer PlatformIO tasks in VS Code, or invoke via the project venv:
  - Build: `./.venv/Scripts/python.exe -m platformio run`
  - Upload (USB): `./.venv/Scripts/python.exe -m platformio run -t upload`
  - Monitor: `./.venv/Scripts/python.exe -m platformio device monitor -b 115200`
- Don’t add new dependencies unless clearly justified.
- Keep diffs minimal and focused; avoid drive-by refactors.
- Use small, surgical edits via workspace tools; avoid filename or path changes unless necessary.

## Publishing / secrets
- Do not commit secrets or personal credentials.
- Do not hardcode real WiFi passwords, tokens, or private server URLs.
- If a credential is required, use placeholders (e.g. `changeme`) and document how users should set it.
 - AP defaults exist: SSID `barometer-ap`, password `changeme` (lab-only; change for non-lab use).

## Code style & design conventions
- Match existing formatting and patterns in `src/main.cpp`.
- Prefer explicit units:
  - Pressure: **Pascals**.
  - Temperature: **Kelvin**.
  - Tank volume/capacity: **m3** (capacity configured in litres, converted at publish).
- Tank level clamped to [0,1].
- On startup, if the tank curve has <2 samples, initialize with `(0.0→0.0)` and `(4095.0→1.0)`.
- Calibration Finish adds full-range guards `(0.0→0.0)` and `(4095.0→1.0)` in addition to observed endpoints and normalized samples.
- `tankId` is a string used in Signal K paths; path changes take effect on restart.
- When changing Signal K paths or config schema, update both code and README.

## Documentation expectations
- Keep [README.md](README.md) accurate with the current behavior (barometer + tank level, calibration, config paths).
- When adding new hardware features (e.g., OLED), document wiring + configuration defaults.
- Note sampling defaults in docs when relevant (currently 5 Hz sampling with 25-sample SMA for sensors).

## Verification
- After changes, do a quick sanity build using the venv-based PlatformIO invocation or VS Code task.
- Don’t attempt to “fix” unrelated warnings/errors outside the scope of the change.

## Runtime configuration & controls
- Calibration UI: single-letter actions — `S` (Start), `F` (Finish), `A` (Abort), `C` (Clear/reset to defaults).
- Capacity config: `/tanks/freshwater/capacityLitres` (litres); updates affect volume/capacity outputs immediately.
- Tank ID config: `/tanks/freshwater/tankId` (string); used in SK paths.
- Curve config: `/calibration/tank/curve`; samples persisted.
- Sample frequency & SMA window: currently fixed in code; if needed, re-create `RepeatSensor` and `MovingAverage` with new values and reconnect pipeline.

## Signal K path conventions
- Barometer:
  - `environment.outside.pressure` (Pa)
  - `environment.outside.temperature` (K)
- Tank:
  - `tanks.freshWater.<tankId>.currentLevel` (ratio)
  - `tanks.freshWater.<tankId>.currentVolume` (m3)
  - `tanks.freshWater.<tankId>.capacity` (m3)
