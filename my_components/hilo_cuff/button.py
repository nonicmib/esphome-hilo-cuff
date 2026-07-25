import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button

from . import CONF_HILO_CUFF_ID, HiloCuffComponent, hilo_cuff_ns

DEPENDENCIES = ["hilo_cuff"]

StartMeasurementButton = hilo_cuff_ns.class_(
    "StartMeasurementButton", button.Button, cg.Component
)

CONFIG_SCHEMA = button.button_schema(
    StartMeasurementButton,
    icon="mdi:heart-pulse",
).extend(
    {
        cv.GenerateID(CONF_HILO_CUFF_ID): cv.use_id(HiloCuffComponent),
    }
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_HILO_CUFF_ID])
    cg.add(var.set_parent(hub))
