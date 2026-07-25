#include "hilo_cuff.h"

#ifdef USE_ESP32

#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

namespace esphome {
namespace hilo_cuff {

static const char *const TAG = "hilo_cuff";

// FAT/Wear-Leveling-Partition für das CSV-Logging (siehe partitions.csv,
// Partitionslabel "hilo_log"). Läuft auf dem internen Flash statt auf einer
// SD-Karte, damit kein Konflikt mit dem SPI-Bus des Displays entsteht.
static const char *const LOG_MOUNT_POINT = "/logfs";
static const char *const LOG_PARTITION_LABEL = "hilo_log";
static wl_handle_t s_log_wl_handle = WL_INVALID_HANDLE;

void HiloCuffComponent::setup() {
  // Nicht dauerhaft verbunden bleiben: laut Protokoll-Doku startet die
  // Manschette bereits beim reinen BLE-Connect das Aufpumpen. Damit das
  // nicht unbemerkt/vorzeitig passiert, bleibt der Client bis zu einem
  // expliziten start_measurement()-Aufruf deaktiviert.
  this->parent()->set_enabled(false);

  // BLE-Sicherheit: Die Manschette lehnt den CCCD-Write für Notifications
  // sonst mit "Insufficient Authentication" (GATT-Status 5) ab - sie
  // verlangt einen verschlüsselten/gebondeten Link. Security-Parameter sind
  // global (nicht pro Node), daher nur einmal setzen. Das eigentliche
  // GAP-Event-Dispatching übernimmt der ble_client-Baustein selbst und ruft
  // dafür unseren (geerbten, virtuellen) gap_event_handler() auf - ein
  // eigenes esp_ble_gap_register_callback() ist NICHT nötig und würde mit
  // der Basisklasse kollidieren.
  static bool security_initialized = false;
  if (!security_initialized) {
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;  // Secure-Connections + Bonding
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;                // Just Works, keine PIN-Eingabe
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
    security_initialized = true;
  }

  if (this->csv_logging_enabled_) {
    this->init_csv_logging_();
  }
}

void HiloCuffComponent::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
      ESP_LOGI(TAG, "Sicherheitsanfrage der Manschette - akzeptiere Pairing");
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "BLE-Pairing/Verschlüsselung erfolgreich");
      } else {
        ESP_LOGW(TAG, "BLE-Pairing fehlgeschlagen, Grund=%d", param->ble_security.auth_cmpl.fail_reason);
      }
      break;
    default:
      break;
  }
}

void HiloCuffComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Hilo/Aktiia Blutdruckmanschette:");
  ESP_LOGCONFIG(TAG, "  Messungs-Timeout: %.0f s", this->measurement_timeout_ / 1000.0f);
  ESP_LOGCONFIG(TAG, "  Trennen nach Messung: %s", YESNO(this->disconnect_after_measurement_));
  ESP_LOGCONFIG(TAG, "  Trennen nach Timeout: %s", YESNO(this->disconnect_on_timeout_));
  ESP_LOGCONFIG(TAG, "  CSV-Logging: %s", YESNO(this->csv_logging_enabled_));
  if (this->csv_logging_enabled_) {
    ESP_LOGCONFIG(TAG, "    Logdatei: %s", this->csv_log_path_.c_str());
    ESP_LOGCONFIG(TAG, "    Download-URL: %s", this->csv_download_url_.c_str());
  }
}

void HiloCuffComponent::schedule_disconnect_() {
  // Kleine Verzögerung, damit die letzte GATT-Antwort/Notification noch
  // sauber durch den BLE-Stack läuft, bevor die Verbindung fällt.
  ESP_LOGI(TAG, "Trenne Verbindung zur Manschette...");
  this->set_timeout(500, [this]() {
    if (this->parent() != nullptr)
      this->parent()->disconnect();
  });
}

void HiloCuffComponent::init_csv_logging_() {
  // Log-Partition einhängen (nur einmal, auch falls mehrere Instanzen
  // existieren sollten). "fatfs"/"wear_levelling" müssen dafür in
  // esp32.framework.advanced.include_builtin_idf_components aktiviert und
  // die Partition "hilo_log" in partitions.csv vorhanden sein.
  if (s_log_wl_handle == WL_INVALID_HANDLE) {
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.max_files = 2;
    mount_config.format_if_mount_failed = true;
    mount_config.allocation_unit_size = 0;  // ESP-IDF-Standard verwenden

    esp_err_t mount_err = esp_vfs_fat_spiflash_mount_rw_wl(LOG_MOUNT_POINT, LOG_PARTITION_LABEL, &mount_config,
                                                             &s_log_wl_handle);
    if (mount_err != ESP_OK) {
      ESP_LOGE(TAG,
               "Log-Partition '%s' konnte nicht gemountet werden (%s) - existiert die "
               "Partition 'hilo_log' in partitions.csv und ist der passende ESP-IDF-Build "
               "aktiv?",
               LOG_PARTITION_LABEL, esp_err_to_name(mount_err));
      return;
    }
    ESP_LOGI(TAG, "Log-Partition gemountet unter %s", LOG_MOUNT_POINT);
  }

  // Legt bei Bedarf die CSV-Datei mit Kopfzeile an und registriert den
  // Download-Endpunkt am Webserver.
  FILE *check = fopen(this->csv_log_path_.c_str(), "r");
  if (check != nullptr) {
    fclose(check);
    ESP_LOGI(TAG, "CSV-Logdatei bereits vorhanden: %s", this->csv_log_path_.c_str());
  } else {
    FILE *f = fopen(this->csv_log_path_.c_str(), "w");
    if (f != nullptr) {
      fputs("timestamp,systolic_mmHg,diastolic_mmHg,map_mmHg,heart_rate_bpm,cuff_battery_pct,status\n", f);
      fclose(f);
      ESP_LOGI(TAG, "CSV-Logdatei angelegt: %s", this->csv_log_path_.c_str());
    } else {
      ESP_LOGW(TAG,
               "CSV-Logdatei konnte nicht angelegt werden (%s) - ist die Log-Partition "
               "korrekt gemountet?",
               this->csv_log_path_.c_str());
    }
  }

  if (this->web_server_base_ == nullptr) {
    ESP_LOGW(TAG, "Kein web_server_base verknüpft - CSV-Download-Endpunkt wird nicht registriert");
    return;
  }

  this->register_download_handler_();
}

void HiloCuffComponent::register_download_handler_() {
  // HiloCuffComponent::setup() läuft mit Priorität AFTER_BLUETOOTH und damit
  // typischerweise VOR dem setup() von web_server_base, in dem der
  // eigentliche AsyncWebServer erst angelegt wird. get_server() kann hier
  // also (noch) einen ungültigen/nullptr-artigen Zustand liefern - daher
  // mit kurzer Verzögerung wiederholen, statt uns auf eine exakte
  // Priority-Reihenfolge zu verlassen.
  auto *server = this->web_server_base_->get_server();
  if (server == nullptr) {
    if (this->web_handler_retries_ < 20) {
      this->web_handler_retries_++;
      this->set_timeout(500, [this]() { this->register_download_handler_(); });
    } else {
      ESP_LOGE(TAG, "Webserver nach mehreren Versuchen nicht bereit - CSV-Download-Endpunkt nicht registriert");
    }
    return;
  }

  server->addHandler(new HiloLogDownloadHandler(this->csv_download_url_, this->csv_log_path_));
  ESP_LOGI(TAG, "CSV-Download eingerichtet unter %s", this->csv_download_url_.c_str());
}

void HiloCuffComponent::append_csv_row_(int systolic, int diastolic, int map_val, int hr, int battery_pct,
                                         const std::string &status) {
  if (!this->csv_logging_enabled_)
    return;

  std::string ts = "0";
  if (this->time_source_ != nullptr) {
    auto t = this->time_source_->now();
    if (t.is_valid()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day_of_month, t.hour, t.minute,
                t.second);
      ts = buf;
    }
  }

  FILE *f = fopen(this->csv_log_path_.c_str(), "a");
  if (f == nullptr) {
    ESP_LOGW(TAG, "CSV-Zeile konnte nicht geschrieben werden (%s)", this->csv_log_path_.c_str());
    return;
  }
  fprintf(f, "%s,%d,%d,%d,%d,%d,%s\n", ts.c_str(), systolic, diastolic, map_val, hr, battery_pct, status.c_str());
  fclose(f);
}

void HiloCuffComponent::start_measurement() {
  auto state = this->parent()->state();

  if (state == esp32_ble_tracker::ClientState::ESTABLISHED) {
    if (this->subscribed_) {
      this->measurement_requested_ = true;
      this->measurement_received_ = false;
      this->measurement_start_time_ = millis();
      this->publish_status_("waiting");
      ESP_LOGI(TAG, "Messung angefordert - jetzt Taste an der Manschette drücken / Manschette anlegen");
    } else {
      ESP_LOGW(TAG, "Verbunden, aber noch nicht auf Notifications abonniert - bitte kurz warten");
    }
    return;
  }

  // Läuft bereits ein Verbindungsauf- oder -abbau, NICHT erneut connect()
  // aufrufen - das führte zuvor zu kollidierenden BLE-Events (Disconnect
  // mit reason CONN_CANCEL, gefolgt von OPEN_EVT im falschen Zustand,
  // status=133) und die Messung kam nie zustande. Ein erneuter Tastendruck
  // während des Verbindungsaufbaus wird daher einfach ignoriert.
  if (state == esp32_ble_tracker::ClientState::CONNECTING ||
      state == esp32_ble_tracker::ClientState::DISCONNECTING) {
    ESP_LOGW(TAG, "Verbindungsauf-/-abbau läuft bereits - Anfrage ignoriert (nicht erneut drücken)");
    return;
  }

  this->measurement_requested_ = true;
  this->measurement_received_ = false;
  ESP_LOGI(TAG, "Verbinde mit Manschette - Messung startet automatisch mit dem Connect");
  this->publish_status_("connecting");
  this->parent()->set_enabled(true);
  this->parent()->connect();
  // measurement_start_time_ wird erst gesetzt, sobald Notifications
  // tatsächlich aktiv sind (siehe ESP_GATTC_WRITE_DESCR_EVT), damit das
  // Timeout nicht schon während des Verbindungsaufbaus zu laufen beginnt.
}

void HiloCuffComponent::loop() {
  if (this->measurement_requested_ && this->subscribed_ && !this->measurement_received_) {
    if (millis() - this->measurement_start_time_ > this->measurement_timeout_) {
      ESP_LOGW(TAG, "Timeout: keine Messung innerhalb von %.0f s erhalten", this->measurement_timeout_ / 1000.0f);
      this->publish_status_("timeout");
      this->measurement_requested_ = false;
      if (this->disconnect_on_timeout_) {
        this->schedule_disconnect_();
      }
    }
  }
}

void HiloCuffComponent::reset_() {
  this->subscribed_ = false;
  this->measurement_handle_ = 0;
  this->status_handle_ = 0;
  this->battery_handle_ = 0;
  this->cccd_write_retries_ = 0;
}

void HiloCuffComponent::write_cccd_(esp_gatt_if_t gattc_if) {
  auto *cccd_descriptor = this->parent()->get_config_descriptor(this->measurement_handle_);
  if (cccd_descriptor == nullptr) {
    ESP_LOGE(TAG, "CCCD-Descriptor (0x2902) für die Messwert-Characteristic nicht gefunden");
    this->publish_status_("error: cccd not found");
    return;
  }
  uint16_t cccd_handle = cccd_descriptor->handle;

  uint8_t notify_en[2] = {0x01, 0x00};
  auto status = esp_ble_gattc_write_char_descr(gattc_if, this->parent()->get_conn_id(), cccd_handle,
                                                 sizeof(notify_en), notify_en, ESP_GATT_WRITE_TYPE_RSP,
                                                 ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "esp_ble_gattc_write_char_descr (CCCD) fehlgeschlagen, status=%d", status);
  }
}

void HiloCuffComponent::publish_status_(const std::string &status) {
  if (this->status_text_sensor_ != nullptr)
    this->status_text_sensor_->publish_state(status);
}

void HiloCuffComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                             esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Mit Manschette verbunden");
        this->publish_status_("connected");
        this->cccd_write_retries_ = 0;
        // Verschlüsselung aktiv anstoßen - ohne authentifizierten Link
        // lehnt die Manschette den späteren CCCD-Write ab (Status 5).
        esp_ble_set_encryption(this->parent()->get_remote_bda(), ESP_BLE_SEC_ENCRYPT);
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Manschette getrennt");
      this->reset_();
      this->publish_status_("disconnected");
      // Nicht automatisch neu verbinden - sonst würde jeder Reconnect laut
      // Protokoll erneut das Aufpumpen auslösen, ohne dass eine Messung
      // angefordert wurde.
      this->parent()->set_enabled(false);
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Standard-BLE-Battery-Service (0x180F / 0x2A19), analog BATTERY_UUID in bplog.py
      auto *battery_char = this->parent()->get_characteristic(esp32_ble_tracker::ESPBTUUID::from_uint16(0x180F),
                                                                esp32_ble_tracker::ESPBTUUID::from_uint16(0x2A19));
      if (battery_char != nullptr) {
        this->battery_handle_ = battery_char->handle;
        auto status = esp_ble_gattc_read_char(gattc_if, this->parent()->get_conn_id(), this->battery_handle_,
                                               ESP_GATT_AUTH_REQ_NONE);
        if (status != ESP_OK) {
          ESP_LOGW(TAG, "esp_ble_gattc_read_char (Akku) fehlgeschlagen, status=%d", status);
        }
      } else {
        ESP_LOGW(TAG, "Battery-Characteristic nicht gefunden");
      }

      auto *measurement_char = this->parent()->get_characteristic(
          esp32_ble_tracker::ESPBTUUID::from_raw(SERVICE_UUID_STR),
          esp32_ble_tracker::ESPBTUUID::from_raw(MEASUREMENT_UUID_STR));
      auto *status_char = this->parent()->get_characteristic(
          esp32_ble_tracker::ESPBTUUID::from_raw(SERVICE_UUID_STR),
          esp32_ble_tracker::ESPBTUUID::from_raw(STATUS_UUID_STR));

      if (measurement_char == nullptr || status_char == nullptr) {
        ESP_LOGE(TAG, "Hilo/Aktiia-Service bzw. Characteristics nicht gefunden - falsches Gerät verbunden?");
        this->publish_status_("error: characteristics not found");
        break;
      }

      this->measurement_handle_ = measurement_char->handle;
      this->status_handle_ = status_char->handle;

      auto notify_status =
          esp_ble_gattc_register_for_notify(gattc_if, this->parent()->get_remote_bda(), this->measurement_handle_);
      if (notify_status != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify fehlgeschlagen, status=%d", notify_status);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->measurement_handle_)
        break;

      // esp_ble_gattc_register_for_notify() registriert nur lokal im
      // ESP-IDF-Stack. Damit die Manschette selbst wirklich Notifications
      // sendet, muss zusätzlich der Client Characteristic Configuration
      // Descriptor (CCCD, 0x2902) mit 0x0001 beschrieben werden - das
      // entspricht dem, was bleaks start_notify() in bplog.py automatisch
      // mit erledigt.
      this->gattc_if_cached_ = gattc_if;
      this->cccd_write_retries_ = 0;
      this->write_cccd_(gattc_if);
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        // Status 5 (ESP_GATT_INSUF_AUTHENTICATION) bedeutet: Manschette
        // verlangt einen verschlüsselten/gebondeten Link. esp_ble_set_encryption()
        // wurde beim Connect bereits angestoßen, braucht aber etwas Zeit -
        // deshalb hier mit kurzer Verzögerung erneut versuchen.
        ESP_LOGW(TAG, "CCCD-Write fehlgeschlagen, status=%d", param->write.status);
        if (this->cccd_write_retries_ < 5) {
          this->cccd_write_retries_++;
          ESP_LOGI(TAG, "Versuche CCCD-Write erneut (%d/5) - warte auf Verschlüsselung...",
                   this->cccd_write_retries_);
          this->set_timeout(400, [this]() { this->write_cccd_(this->gattc_if_cached_); });
        } else {
          this->publish_status_("error: notify subscribe failed");
        }
        break;
      }

      this->subscribed_ = true;
      ESP_LOGI(TAG, "Notifications aktiviert - Manschette sollte jetzt automatisch messen");
      this->publish_status_(this->measurement_requested_ ? "waiting" : "ready");
      if (this->measurement_requested_)
        this->measurement_start_time_ = millis();
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status != ESP_GATT_OK)
        break;

      if (param->read.handle == this->battery_handle_ && param->read.value_len >= 1) {
        this->last_battery_raw_ = param->read.value[0];
        if (this->battery_sensor_ != nullptr)
          this->battery_sensor_->publish_state(param->read.value[0]);
      } else if (param->read.handle == this->status_handle_ && param->read.value_len >= 1) {
        // Status 2 = Erfolg. Bei anderen Werten steht der Fehlertyp laut
        // aktueller Protokoll-Doku im zuletzt empfangenen Diastolic-Byte
        // (1 = Range/Validity, 2 = Movement, 4/5 = Measurement error).
        uint8_t status_code = param->read.value[0];
        std::string status_str;
        if (status_code == 2) {
          status_str = "ok";
        } else {
          std::string error_name;
          switch (this->last_diastolic_raw_) {
            case 1:
              error_name = "range/validity error";
              break;
            case 2:
              error_name = "movement error";
              break;
            case 4:
            case 5:
              error_name = "measurement error";
              break;
            default:
              error_name = "unknown error";
              break;
          }
          status_str = "error: code " + std::to_string(status_code) + " (" + error_name + ")";
        }
        this->publish_status_(status_str);
        this->measurement_requested_ = false;

        this->append_csv_row_(this->last_systolic_raw_, this->last_diastolic_raw_, this->last_map_raw_,
                               this->last_hr_raw_, this->last_battery_raw_, status_str);

        // Messung ist abgeschlossen (erfolgreich oder mit Fehlercode) -
        // Verbindung optional trennen, um den Akku der Manschette zu schonen.
        if (this->disconnect_after_measurement_) {
          this->schedule_disconnect_();
        }
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->measurement_handle_)
        break;
      if (param->notify.value_len < 4) {
        ESP_LOGW(TAG, "Messwert-Notification zu kurz (%d Bytes)", param->notify.value_len);
        break;
      }

      // Byte-Layout laut aktueller Protokoll-Doku: [diastolic, systolic, map, hr, unused]
      uint8_t diastolic = param->notify.value[0];
      uint8_t systolic = param->notify.value[1];
      uint8_t map_val = param->notify.value[2];
      uint8_t hr = param->notify.value[3];
      this->last_diastolic_raw_ = diastolic;  // für Fehlercode-Dekodierung beim Status-Read
      this->last_systolic_raw_ = systolic;
      this->last_map_raw_ = map_val;
      this->last_hr_raw_ = hr;

      ESP_LOGI(TAG, "Messung erhalten: %d/%d mmHg, MAP %d mmHg, Puls %d bpm", systolic, diastolic, map_val, hr);

      if (this->systolic_sensor_ != nullptr)
        this->systolic_sensor_->publish_state(systolic);
      if (this->diastolic_sensor_ != nullptr)
        this->diastolic_sensor_->publish_state(diastolic);
      if (this->map_sensor_ != nullptr)
        this->map_sensor_->publish_state(map_val);
      if (this->heart_rate_sensor_ != nullptr)
        this->heart_rate_sensor_->publish_state(hr);

      this->measurement_received_ = true;

      // Status-Characteristic nachlesen (2 = Erfolg), wie im Vorbild.
      auto status = esp_ble_gattc_read_char(gattc_if, this->parent()->get_conn_id(), this->status_handle_,
                                             ESP_GATT_AUTH_REQ_NONE);
      if (status != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gattc_read_char (Status) fehlgeschlagen, status=%d", status);
        this->measurement_requested_ = false;
      }
      break;
    }

    default:
      break;
  }
}

void StartMeasurementButton::dump_config() { ESP_LOGCONFIG(TAG, "Hilo Cuff Start-Button"); }

void StartMeasurementButton::press_action() {
  if (this->parent_ != nullptr)
    this->parent_->start_measurement();
}

}  // namespace hilo_cuff
}  // namespace esphome

#endif  // USE_ESP32
