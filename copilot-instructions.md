# AI Agent Instructions (senseESP_barometer)

These instructions apply to any AI agent (GitHub Copilot, ChatGPT, etc.) working in this repository.

## Project intent
- ESP32-based barometer node using **SenseESP** to publish BMP280 measurements to a **Signal K** server.
- Keep the project small, readable, and easy to revive years later.

## Hardware assumptions
- Board: ESP32 (Hat Labs SN ESP or compatible)
- I²C pins are explicitly set in firmware: **SDA=GPIO16**, **SCL=GPIO17**.
- BMP280 I²C address: try `0x76`, then `0x77`.

## Software constraints
- Framework: Arduino on ESP32 via PlatformIO.
- Core libs: SenseESP + Adafruit BMP280 (+ Adafruit Unified Sensor).
- Event loop: must call `event_loop()->tick()` in `loop()`.

## Development workflow
- Prefer PlatformIO tasks already defined in VS Code (Upload/Monitor/etc.).
- Don’t add new dependencies unless clearly justified.
- Keep diffs minimal and focused; avoid drive-by refactors.

## Publishing / secrets
- Do not commit secrets or personal credentials.
- Do not hardcode real WiFi passwords, tokens, or private server URLs.
- If a credential is required, use placeholders (e.g. `changeme`) and document how users should set it.

## Code style & design conventions
- Match existing formatting and patterns in `src/main.cpp`.
- Prefer explicit units:
  - Pressure is published in **Pascals**.
  - Temperature is published in **Kelvin**.
- When changing Signal K paths, update both code and README.

## Documentation expectations
- Keep [README.md](README.md) accurate with the current behavior.
- When adding new hardware features (e.g., OLED), document wiring + configuration defaults.

## Verification
- After changes, do a quick sanity build with PlatformIO (`pio run`) or the VS Code build task.
- Don’t attempt to “fix” unrelated warnings/errors outside the scope of the change.
