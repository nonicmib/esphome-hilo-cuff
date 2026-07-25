import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_HILO_CUFF_ID, HiloCuffComponent

DEPENDENCIES = ["hilo_cuff"]

CONF_MEASUREMENT_STATUS = "measurement_status"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HILO_CUFF_ID): cv.use_id(HiloCuffComponent),
        cv.Optional(CONF_MEASUREMENT_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:information-outline",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_HILO_CUFF_ID])
    if CONF_MEASUREMENT_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_MEASUREMENT_STATUS])
        cg.add(hub.set_status_text_sensor(sens))
