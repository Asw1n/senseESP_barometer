#include <Arduino.h>
#include "sensesp_app_builder.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/sensors/sensor.h"
#include "sensesp/transforms/linear.h"
#include "sensesp/ui/config_item.h"
#include <Adafruit_BMP280.h>
#include <Wire.h>

using namespace sensesp;

Adafruit_BMP280 bmp280;
bool bmp_ok = false;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("barometer: booting...");
  SetupLogging();

  SensESPAppBuilder builder;
    builder.set_hostname("barometer");
    builder.set_wifi_access_point("barometer-ap", "changeme");
    builder.enable_system_info_sensors();
    builder.enable_wifi_watchdog();
    builder.get_app();

  // Use Hat Labs I2C pins (SDA=16, SCL=17)
  Wire.begin(16, 17);

  // Quick I2C scan to aid debugging

  bmp_ok = bmp280.begin(0x76);
  if (!bmp_ok) {
    bmp_ok = bmp280.begin(0x77);
  }
  ESP_LOGI(__FILE__, "BMP280 init: %s", bmp_ok ? "OK" : "FAILED");

  Serial.println("barometer: setup complete.");

  const unsigned int read_interval = 1000; // ms

  if (bmp_ok) {
    auto* pressure_sensor = new RepeatSensor<float>(read_interval, []() {
      return bmp280.readPressure(); // Pascals
    });
    auto pressure_offset = std::make_shared<Linear>(1.0f, 0.0f, "/calibration/pressure");
    ConfigItem(pressure_offset)->set_title("Pressure Calibration");
    pressure_sensor->connect_to(pressure_offset.get())
        ->connect_to(new SKOutputFloat("environment.outside.pressure", "", "Pa"));

    auto* temperature_sensor = new RepeatSensor<float>(read_interval, []() {
      return bmp280.readTemperature() + 273.15f; // Kelvin
    });
    auto temperature_offset = std::make_shared<Linear>(1.0f, 0.0f, "/calibration/temperature");
    ConfigItem(temperature_offset)->set_title("Temperature Calibration");
    temperature_sensor->connect_to(temperature_offset.get())
        ->connect_to(new SKOutputFloat("environment.outside.temperature", "", "K"));
  } else {
    ESP_LOGE(__FILE__, "BMP280 not detected; skipping sensor output wiring");
  }
}

void loop() {
  // Tick the SenseESP/ReactESP event loop so scheduled tasks run.
  event_loop()->tick();
}
