#include <Arduino.h>
#include "sensesp_app_builder.h"
#include "sensesp_app.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/sensors/sensor.h"
#include "sensesp/transforms/linear.h"
#include "sensesp/ui/config_item.h"
#include <Adafruit_BMP280.h>
#include <Wire.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
// Use SenseESP transforms for averaging and plumbing
#include "sensesp/transforms/moving_average.h"
#include "sensesp/transforms/transform.h"
// Standard SenseESP transform for piecewise interpolation
#include "sensesp/transforms/curveinterpolator.h"
// Config UI controls for numeric parameters
#include "sensesp/ui/ui_controls.h"
// Serializable is provided via Saveable base; explicit include not required

// No compile-time defaults; calibration uses raw → fraction mapping directly

// Avoid namespace collisions; qualify all SenseESP types explicitly.

Adafruit_BMP280 bmp280;
bool bmp_ok = false;


// Global averaged/latest raw for future calibration
static uint16_t g_latest_raw = 0;
static float g_avg_raw = 0.0f;

// Keep ConfigItem instances alive to ensure UI registration persists
static std::vector<std::shared_ptr<sensesp::ConfigItemBase>> g_config_items;
// Keep transforms alive beyond setup scope
static std::vector<std::shared_ptr<sensesp::TransformBase>> g_transforms;
// Global handle to tank level curve
static std::shared_ptr<sensesp::CurveInterpolator> g_curve;
// Keep sensors alive (e.g., AnalogInput)
static std::vector<std::shared_ptr<sensesp::FloatSensor>> g_sensors;
// Configurable tank capacity in litres
static float g_capacity_liters = 100.0f;
static String g_capacity_path = "/tanks/freshwater/capacityLitres";
// Configurable tank indicator index (used in Signal K path)
static String g_tank_id = "0";
static String g_tank_id_path = "/tanks/freshwater/tankId";
// Hold config objects to read current values reliably
static std::shared_ptr<sensesp::NumberConfig> g_capacity_cfg;
static std::shared_ptr<sensesp::StringConfig> g_tank_id_cfg;

// 5 s SMA on 5 Hz sampling
// Tap transform to store latest averaged raw and collect calibration samples
class LatestAvgTap : public sensesp::FloatTransform {
 public:
  LatestAvgTap(const String& path = "") : sensesp::FloatTransform(path) {}
  void set(const float& input) override {
    float clamped = input;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > 4095.0f) clamped = 4095.0f;
    g_avg_raw = clamped;
    g_latest_raw = (uint16_t)clamped;
    // Pass-through downstream
    this->emit(clamped);
  }
};

// Transform: level (0..1) to volume (m3) using configured capacity in litres
class LevelToVolume : public sensesp::FloatTransform {
 public:
  LevelToVolume(const String& path = "") : sensesp::FloatTransform(path) {}
  void set(const float& level) override {
    float cap_l = g_capacity_cfg ? g_capacity_cfg->get_value() : g_capacity_liters;
    float volume_m3 = level * (cap_l / 1000.0f);
    this->emit(volume_m3);
  }
};

// Single calibration controller with Status (read-only) and Action (apply)
class CalibrationController : public sensesp::TransformBase {
 public:
  enum class Status { Inactive, Running };

  explicit CalibrationController(const String& path)
      : sensesp::TransformBase(path), status_(Status::Inactive), running_(false) {
    // Sample averaged raw at 1 Hz while Running; keep lightweight
    sensesp::event_loop()->onRepeat(1000, [this]() {
      if (running_) {
        samples_.push_back(g_avg_raw);
      }
    });
  }

  void start() {
    ESP_LOGI(__FILE__, "Calibration: Start");
    samples_.clear();
    running_ = true;
    status_ = Status::Running;
  }

  void finish() {
    ESP_LOGI(__FILE__, "Calibration: Finish (%u samples)", (unsigned)samples_.size());
    running_ = false;
    if (samples_.empty()) {
      ESP_LOGW(__FILE__, "Calibration: No samples collected; skipping curve update");
      status_ = Status::Inactive;
      return;
    }
    // Build curve using normalized fractions and explicitly include empty/full endpoints.
    const size_t N = 500;
    if (g_curve) {
      g_curve->clear_samples();
      const size_t total = samples_.size();
      if (total < 2) {
        // With only one sample, we cannot form a slope; still store endpoints as the same raw.
        float r = samples_[0];
        // Also ensure full-range endpoints are present
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(0.0f, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(r, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(r, 1.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(4095.0f, 1.0f));
        bool saved = g_curve->save();
        ESP_LOGW(__FILE__, "Calibration: Only one sample; stored degenerate endpoints (save %s)", saved ? "OK" : "FAILED");
        status_ = Status::Inactive;
        return;
      }

      // Compute observed empty/full raw values
      float min_raw = samples_[0];
      float max_raw = samples_[0];
      for (float v : samples_) {
        if (v < min_raw) min_raw = v;
        if (v > max_raw) max_raw = v;
      }

      if (max_raw <= min_raw) {
        // Fallback: store degenerate endpoints with full-range guards
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(0.0f, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(min_raw, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(max_raw, 1.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(4095.0f, 1.0f));
      } else {
        // Explicit endpoints
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(0.0f, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(min_raw, 0.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(max_raw, 1.0f));
        g_curve->add_sample(sensesp::CurveInterpolator::Sample(4095.0f, 1.0f));

        // Add up to N normalized samples across the collected set
        const size_t M = (total < N) ? total : N;
        for (size_t i = 0; i < M; i++) {
          float frac_idx = (M > 1) ? (float)i / (float)(M - 1) : 0.0f;
          size_t idx = (size_t) roundf(frac_idx * (float)(total - 1));
          float raw = samples_[idx];
          float frac = (raw - min_raw) / (max_raw - min_raw);
          if (frac < 0.0f) frac = 0.0f;
          if (frac > 1.0f) frac = 1.0f;
          g_curve->add_sample(sensesp::CurveInterpolator::Sample(raw, frac));
        }
      }

      ESP_LOGI(__FILE__, "Calibration: Curve endpoints raw_min=%.1f→0.0, raw_max=%.1f→1.0", min_raw, max_raw);
      bool saved = g_curve->save();
      ESP_LOGI(__FILE__, "Calibration: Curve save %s", saved ? "OK" : "FAILED");
    } else {
      ESP_LOGE(__FILE__, "Calibration: CurveInterpolator not available");
    }
    status_ = Status::Inactive;
  }

  void abort() {
    ESP_LOGI(__FILE__, "Calibration: Abort");
    running_ = false;
    samples_.clear();
    status_ = Status::Inactive;
  }

  void clear_curve() {
    ESP_LOGI(__FILE__, "Calibration: Clear curve");
    running_ = false;
    samples_.clear();
    if (g_curve) {
      g_curve->clear_samples();
      // After clearing, add default endpoints like startup initializer
      g_curve->add_sample(sensesp::CurveInterpolator::Sample(0.0f, 0.0f));
      g_curve->add_sample(sensesp::CurveInterpolator::Sample(4095.0f, 1.0f));
      bool saved = g_curve->save();
      ESP_LOGI(__FILE__, "Calibration: Curve reset to defaults (save %s)", saved ? "OK" : "FAILED");
    }
    status_ = Status::Inactive;
  }

  // Serializable interface for ConfigItem
  bool to_json(JsonObject& root) override {
    root["status"] = status_string();
    // Default action is 'N' (None)
    root["action"] = "N";
    return true;
  }

  bool from_json(const JsonObject& config) override {
    String act = config["action"] | String("N");
    act.toUpperCase();
    if (act.length() == 1) {
      char c = act[0];
      switch (c) {
        case 'S':
          start();
          break;
        case 'F':
          finish();
          break;
        case 'A':
          abort();
          break;
        case 'C':
          clear_curve();
          break;
        case 'N':
        default:
          ESP_LOGI(__FILE__, "Calibration: No action");
          break;
      }
    } else {
      // Backward compatibility with full-word actions
      if (act == "START") {
        start();
      } else if (act == "FINISH") {
        finish();
      } else if (act == "ABORT") {
        abort();
      } else if (act == "CLEAR") {
        clear_curve();
      } else {
        ESP_LOGI(__FILE__, "Calibration: No action");
      }
    }
    return true;
  }

  String get_config_schema() {
    StaticJsonDocument<768> doc;
    JsonObject root = doc.to<JsonObject>();
    root["type"] = "object";
    JsonObject props = root.createNestedObject("properties");

    JsonObject status = props.createNestedObject("status");
    status["type"] = "string";
    status["readOnly"] = true;
    status["title"] = "Status";

    JsonObject action = props.createNestedObject("action");
    action["type"] = "string";
    action["title"] = "Action";
    action["description"] =
      "Actions: N=None, S=Start, F=Finish, A=Abort, C=Clear";
    JsonArray en = action.createNestedArray("enum");
    en.add("N");
    en.add("S");
    en.add("F");
    en.add("A");
    en.add("C");

    String out;
    serializeJson(doc, out);
    return out;
  }

 private:
  String status_string() const {
    switch (status_) {
      case Status::Inactive:
        return "Inactive";
      case Status::Running:
        return "Running";
    }
    return "Inactive";
  }

  Status status_;
  bool running_;
  std::vector<float> samples_;
};

// Free function to provide schema for ConfigItem
namespace sensesp {
  inline const String ConfigSchema(CalibrationController& obj) {
    return obj.get_config_schema();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  sensesp::SetupLogging();

  sensesp::SensESPAppBuilder builder;
    builder.set_hostname("barometer");
    builder.set_wifi_access_point("barometer-ap", "changeme");
    builder.enable_system_info_sensors();
    builder.enable_wifi_watchdog();
    auto app = builder.get_app();

  // Enable Arduino OTA so PlatformIO 'espota' can upload via barometer.local
  ArduinoOTA.setHostname("barometer");
  ArduinoOTA.onStart([]() { ESP_LOGI(__FILE__, "OTA: Start"); });
  ArduinoOTA.onEnd([]() { ESP_LOGI(__FILE__, "OTA: End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Throttle progress logs
    static unsigned int last_pct = 0;
    unsigned int pct = (total ? (progress * 100 / total) : 0);
    if (pct - last_pct >= 10) { // every 10%
      ESP_LOGI(__FILE__, "OTA: %u%%", pct);
      last_pct = pct;
    }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ESP_LOGE(__FILE__, "OTA Error[%u]", static_cast<unsigned int>(error));
  });
  ArduinoOTA.begin();

  // Use Hat Labs I2C pins (SDA=16, SCL=17)
  Wire.begin(16, 17);

  // Quick I2C scan to aid debugging

  bmp_ok = bmp280.begin(0x76);
  if (!bmp_ok) {
    bmp_ok = bmp280.begin(0x77);
  }

  const unsigned int read_interval = 200; // 5 Hz for BMP280

  if (bmp_ok) {
    auto* pressure_sensor = new sensesp::RepeatSensor<float>(read_interval, []() {
      return bmp280.readPressure(); // Pascals
    });
    auto pressure_offset = std::make_shared<sensesp::Linear>(1.0f, 0.0f, "/calibration/bmp280_pressure");
    auto ci_pressure = sensesp::ConfigItem(pressure_offset);
    ci_pressure->set_title("BMP280 Pressure Calibration");
    g_config_items.push_back(ci_pressure);
    auto pressure_avg = std::make_shared<sensesp::MovingAverage>(25);
    g_transforms.push_back(pressure_offset);
    g_transforms.push_back(pressure_avg);
    pressure_sensor->connect_to(pressure_offset)
      ->connect_to(pressure_avg)
      ->connect_to(new sensesp::SKOutputFloat("environment.outside.pressure", "", "Pa"));

    auto* temperature_sensor = new sensesp::RepeatSensor<float>(read_interval, []() {
      return bmp280.readTemperature() + 273.15f; // Kelvin
    });
    auto temperature_offset = std::make_shared<sensesp::Linear>(1.0f, 0.0f, "/calibration/bmp280_temperature");
    auto ci_temperature = sensesp::ConfigItem(temperature_offset);
    ci_temperature->set_title("BMP280 Temperature Calibration");
    g_config_items.push_back(ci_temperature);
    auto temperature_avg = std::make_shared<sensesp::MovingAverage>(25);
    g_transforms.push_back(temperature_offset);
    g_transforms.push_back(temperature_avg);
    temperature_sensor->connect_to(temperature_offset)
      ->connect_to(temperature_avg)
      ->connect_to(new sensesp::SKOutputFloat("environment.outside.temperature", "", "K"));
  } else {
    ESP_LOGE(__FILE__, "BMP280 not detected; skipping sensor output wiring");
  }

  // XDB401 analog sensor on ADC1 VP (GPIO36) using standard classes
  const int xdb401_adc_pin = 36; // VP (ADC1_CH0)
  pinMode(xdb401_adc_pin, INPUT);
  analogSetPinAttenuation(xdb401_adc_pin, ADC_11db);
  analogReadResolution(12); // ensure 0..4095 counts
  const unsigned int xdb401_interval = 200; // 5 Hz
  // Read raw ADC counts directly using RepeatSensor
  auto xdb_input = std::make_shared<sensesp::RepeatSensor<float>>(xdb401_interval, [=]() {
    int counts = analogRead(xdb401_adc_pin);
    return (float) counts;
  });
  auto xdb_avg = std::make_shared<sensesp::MovingAverage>(25);
  auto xdb_tap = std::make_shared<LatestAvgTap>("/calibration/tank/avg_tap");
  g_curve = std::make_shared<sensesp::CurveInterpolator>(nullptr, 
                              "/calibration/tank/curve");
  g_curve->set_input_title("raw")->set_output_title("level");
  // Ensure the curve has at least two calibration pairs on first boot
  {
    size_t n = g_curve->get_samples().size();
    ESP_LOGI(__FILE__, "Startup: Curve has %u sample(s)", (unsigned)n);
    if (n < 2) {
      g_curve->clear_samples();
      // Default to full ADC range: empty=0.0 -> 0.0, full=4095.0 -> 1.0
      g_curve->add_sample(sensesp::CurveInterpolator::Sample(0.0f, 0.0f));
      g_curve->add_sample(sensesp::CurveInterpolator::Sample(4095.0f, 1.0f));
      bool saved = g_curve->save();
      ESP_LOGI(__FILE__, "Startup: Initialized curve with 2 default samples (save %s)", saved ? "OK" : "FAILED");
    }
  }
  g_sensors.push_back(std::static_pointer_cast<sensesp::FloatSensor>(xdb_input));
  g_transforms.push_back(xdb_avg);
  g_transforms.push_back(xdb_tap);
  g_transforms.push_back(g_curve);
  xdb_input->connect_to(xdb_avg)->connect_to(xdb_tap)->connect_to(g_curve);

  // Publish uncalibrated readings via serial logs only (no Signal K outputs)

    // Removed legacy pressure/height outputs; calibrated level published separately

    // Averaging handled via SenseESP MovingAverage; raw_sampler no longer needed

    // Config UI controls under /calibration/tank
    // Remove legacy calibration UI cards in Step 1 (single card will be added in Step 2)

    // Read tank identifier (default "0") from storage before building Signal K paths
    g_tank_id_cfg = std::make_shared<sensesp::StringConfig>(g_tank_id, g_tank_id_path);
    // Update runtime tank id from stored value
    g_tank_id = g_tank_id_cfg->get_value();
    // Note: changes to tank id take effect after restart
    String tank_prefix = String("tanks.freshWater.") + g_tank_id + String(".");

    // Publish calibrated level and computed volume
    g_curve->connect_to(new sensesp::SKOutputFloat(tank_prefix + "currentLevel", 
                 "/tanks/freshwater/level", "ratio"));
    // Publish configured capacity (m3) — sensor defined after config load below
    std::shared_ptr<sensesp::RepeatSensor<float>> capacity_m3_sensor;
    g_curve->connect_to(new LevelToVolume())
      ->connect_to(new sensesp::SKOutputFloat(tank_prefix + "currentVolume", 
                 "/tanks/freshwater/volume", "m3"));

  // Expose the curve and capacity in the configuration UI
  auto ci_curve = sensesp::ConfigItem(g_curve);
  ci_curve->set_title("Fresh Water Tank Curve");
  g_config_items.push_back(ci_curve);

  // Capacity configuration (litres)
  g_capacity_cfg = std::make_shared<sensesp::NumberConfig>(g_capacity_liters, g_capacity_path);
  auto ci_cap = sensesp::ConfigItem(g_capacity_cfg);
  ci_cap->set_title("Fresh Water Tank Capacity (L)");
  g_config_items.push_back(ci_cap);
  

  // Create capacity sensor after config load so initial value reflects storage
  capacity_m3_sensor = std::make_shared<sensesp::RepeatSensor<float>>(5000, []() {
    float cap_l = g_capacity_cfg ? g_capacity_cfg->get_value() : g_capacity_liters;
    return cap_l / 1000.0f;
  });
  g_sensors.push_back(std::static_pointer_cast<sensesp::FloatSensor>(capacity_m3_sensor));
  capacity_m3_sensor->connect_to(new sensesp::SKOutputFloat(String("tanks.freshWater.") + g_tank_id + String(".") + "capacity",
                                                            "/tanks/freshwater/capacity", "m3"));

  // (Diagnostics removed for clean runtime logs)

  // Tank identifier configuration (string)
  auto ci_tank_id = sensesp::ConfigItem(g_tank_id_cfg);
  ci_tank_id->set_title("Fresh Water Tank Identifier");
  ci_tank_id->set_description("String tank id used in Signal K paths (default \"0\"). Changes require restart.");
  g_config_items.push_back(ci_tank_id);

  // Single calibration card (Status + Action)
  auto cal_controller = std::make_shared<CalibrationController>("/calibration/tank/controller");
  auto ci_cal = sensesp::ConfigItem(cal_controller);
  ci_cal->set_title("Fresh Water Tank Calibration");
  ci_cal->set_description(
      "Use one-letter actions: N=None, S=Start, F=Finish, A=Abort, C=Clear.");
  g_config_items.push_back(ci_cal);

  // Start the app (this builds the UI and tabs)
  app->start();
}

void loop() {
  // Tick the SenseESP/ReactESP event loop so scheduled tasks run.
  sensesp::event_loop()->tick();
  // Handle OTA events
  ArduinoOTA.handle();
}

// No custom ConfigSchema overrides needed in Step 1
