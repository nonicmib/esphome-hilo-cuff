import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, time, web_server_base
from esphome.const import CONF_ID, CONF_TIME_ID

CODEOWNERS = ["@your-github-handle"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["sensor", "text_sensor", "button"]
MULTI_CONF = True

hilo_cuff_ns = cg.esphome_ns.namespace("hilo_cuff")
HiloCuffComponent = hilo_cuff_ns.class_(
    "HiloCuffComponent", cg.Component, ble_client.BLEClientNode
)

CONF_HILO_CUFF_ID = "hilo_cuff_id"
CONF_MEASUREMENT_TIMEOUT = "measurement_timeout"
CONF_DISCONNECT_AFTER_MEASUREMENT = "disconnect_after_measurement"
CONF_DISCONNECT_ON_TIMEOUT = "disconnect_on_timeout"
CONF_CSV_LOGGING = "csv_logging"
CONF_CSV_LOG_PATH = "csv_log_path"
CONF_CSV_DOWNLOAD_URL = "csv_download_url"
CONF_WEB_SERVER_BASE_ID = "web_server_base_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HiloCuffComponent),
            cv.Optional(
                CONF_MEASUREMENT_TIMEOUT, default="120s"
            ): cv.positive_time_period_milliseconds,
            # Nach erfolgreicher (bzw. mit Fehlercode abgeschlossener) Messung
            # automatisch von der Manschette trennen, um deren Akku zu schonen.
            cv.Optional(CONF_DISCONNECT_AFTER_MEASUREMENT, default=True): cv.boolean,
            # Optional: auch nach einem Timeout (keine Messung erhalten) trennen.
            cv.Optional(CONF_DISCONNECT_ON_TIMEOUT, default=True): cv.boolean,
            # CSV-Logging jeder Messung auf die SD-Karte + Download über den
            # ESPHome-Webserver.
            cv.Optional(CONF_CSV_LOGGING, default=True): cv.boolean,
            cv.Optional(CONF_CSV_LOG_PATH, default="/logfs/hilo_log.csv"): cv.string,
            cv.Optional(CONF_CSV_DOWNLOAD_URL, default="/hilo_log.csv"): cv.string,
            cv.Optional(CONF_WEB_SERVER_BASE_ID): cv.use_id(web_server_base.WebServerBase),
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(ble_client.BLE_CLIENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_measurement_timeout(config[CONF_MEASUREMENT_TIMEOUT]))
    cg.add(var.set_disconnect_after_measurement(config[CONF_DISCONNECT_AFTER_MEASUREMENT]))
    cg.add(var.set_disconnect_on_timeout(config[CONF_DISCONNECT_ON_TIMEOUT]))
    cg.add(var.set_csv_logging_enabled(config[CONF_CSV_LOGGING]))
    cg.add(var.set_csv_log_path(config[CONF_CSV_LOG_PATH]))
    cg.add(var.set_csv_download_url(config[CONF_CSV_DOWNLOAD_URL]))

    if CONF_WEB_SERVER_BASE_ID in config:
        server = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
        cg.add(var.set_web_server_base(server))

    if CONF_TIME_ID in config:
        time_source = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_source))
