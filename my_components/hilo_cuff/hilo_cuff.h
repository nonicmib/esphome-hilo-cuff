#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/time/real_time_clock.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <esp_vfs_fat.h>
#include <string>
#include <cstdio>

namespace esphome {
namespace hilo_cuff {

// UUIDs und Timing 1:1 aus antirez/bplog (python/bplog.py) übernommen:
// https://github.com/antirez/bplog/blob/main/python/bplog.py
static const char *const SERVICE_UUID_STR = "b1e71568-047b-47c4-88c9-0f90e397acf7";
static const char *const MEASUREMENT_UUID_STR = "a6b40002-003d-4e65-9208-08f4db958863";
static const char *const STATUS_UUID_STR = "a6b40003-003d-4e65-9208-08f4db958863";
static const uint32_t DEFAULT_MEASUREMENT_TIMEOUT_MS = 120000;  // 120 s, wie MEASUREMENT_TIMEOUT im Vorbild

class HiloCuffComponent : public esphome::ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

  void set_systolic_sensor(sensor::Sensor *s) { this->systolic_sensor_ = s; }
  void set_diastolic_sensor(sensor::Sensor *s) { this->diastolic_sensor_ = s; }
  void set_map_sensor(sensor::Sensor *s) { this->map_sensor_ = s; }
  void set_heart_rate_sensor(sensor::Sensor *s) { this->heart_rate_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { this->battery_sensor_ = s; }
  void set_status_text_sensor(text_sensor::TextSensor *s) { this->status_text_sensor_ = s; }
  void set_measurement_timeout(uint32_t ms) { this->measurement_timeout_ = ms; }
  void set_disconnect_after_measurement(bool b) { this->disconnect_after_measurement_ = b; }
  void set_disconnect_on_timeout(bool b) { this->disconnect_on_timeout_ = b; }

  // CSV-Logging + Download über den ESPHome-Webserver
  void set_csv_logging_enabled(bool b) { this->csv_logging_enabled_ = b; }
  void set_csv_log_path(std::string path) { this->csv_log_path_ = std::move(path); }
  void set_csv_download_url(std::string url) { this->csv_download_url_ = std::move(url); }
  void set_web_server_base(web_server_base::WebServerBase *base) { this->web_server_base_ = base; }
  void set_time_source(time::RealTimeClock *time_source) { this->time_source_ = time_source; }

  // Wird vom Start-Button (GPIO39) oder per Home-Assistant-Serviceaufruf
  // (button.press des zugehörigen Button-Entities) aufgerufen.
  void start_measurement();

 protected:
  void reset_();
  void publish_status_(const std::string &status);
  void schedule_disconnect_();
  void init_csv_logging_();
  void register_download_handler_();
  void append_csv_row_(int systolic, int diastolic, int map_val, int hr, int battery_pct, const std::string &status);
  void write_cccd_(esp_gatt_if_t gattc_if);

  uint16_t measurement_handle_{0};
  uint16_t status_handle_{0};
  uint16_t battery_handle_{0};

  bool measurement_requested_{false};
  bool subscribed_{false};
  bool measurement_received_{false};
  uint32_t measurement_start_time_{0};
  uint32_t measurement_timeout_{DEFAULT_MEASUREMENT_TIMEOUT_MS};
  bool disconnect_after_measurement_{true};
  bool disconnect_on_timeout_{true};
  uint8_t last_diastolic_raw_{0};
  uint8_t last_systolic_raw_{0};
  uint8_t last_map_raw_{0};
  uint8_t last_hr_raw_{0};
  uint8_t last_battery_raw_{0};
  uint8_t cccd_write_retries_{0};
  esp_gatt_if_t gattc_if_cached_{0};

  // CSV-Logging + Download
  bool csv_logging_enabled_{true};
  std::string csv_log_path_{"/logfs/hilo_log.csv"};
  std::string csv_download_url_{"/hilo_log.csv"};
  web_server_base::WebServerBase *web_server_base_{nullptr};
  time::RealTimeClock *time_source_{nullptr};
  uint8_t web_handler_retries_{0};

  sensor::Sensor *systolic_sensor_{nullptr};
  sensor::Sensor *diastolic_sensor_{nullptr};
  sensor::Sensor *map_sensor_{nullptr};
  sensor::Sensor *heart_rate_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
};

// Kleiner Button, der nur start_measurement() auf der Hub-Komponente aufruft.
// Wird sowohl vom physischen GPIO39-Taster (binary_sensor -> button.press)
// als auch automatisch als Entity in Home Assistant nutzbar.
class StartMeasurementButton : public button::Button, public Component {
 public:
  void set_parent(HiloCuffComponent *parent) { this->parent_ = parent; }
  void dump_config() override;

 protected:
  void press_action() override;
  HiloCuffComponent *parent_{nullptr};
};

// Registriert einen GET-Endpunkt am ESPHome-Webserver, der die CSV-Logdatei
// zum Download anbietet. Der ESP-IDF-Webserver-Shim von ESPHome kennt kein
// AsyncWebServer::on(...) wie ESPAsyncWebServer - Endpunkte werden dort über
// eine AsyncWebHandler-Subklasse mit addHandler() registriert.
class HiloLogDownloadHandler : public AsyncWebHandler {
 public:
  HiloLogDownloadHandler(std::string url, std::string file_path)
      : url_(std::move(url)), file_path_(std::move(file_path)) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url_buf) == this->url_;
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    FILE *f = fopen(this->file_path_.c_str(), "r");
    if (f == nullptr) {
      request->send(404, "text/plain", "Logdatei nicht gefunden");
      return;
    }

    std::string content;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      content.append(buf, n);
    fclose(f);

    auto *response = request->beginResponse(200, "text/csv", content);
    response->addHeader("Content-Disposition", "attachment; filename=\"hilo_log.csv\"");
    request->send(response);
  }

 protected:
  std::string url_;
  std::string file_path_;
};

}  // namespace hilo_cuff
}  // namespace esphome

#endif  // USE_ESP32
