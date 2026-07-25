import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_BATTERY_LEVEL,
    DEVICE_CLASS_BATTERY,
    ICON_HEART_PULSE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

from . import CONF_HILO_CUFF_ID, HiloCuffComponent

DEPENDENCIES = ["hilo_cuff"]

CONF_SYSTOLIC = "systolic"
CONF_DIASTOLIC = "diastolic"
CONF_MAP = "map"
CONF_HEART_RATE = "heart_rate"

UNIT_MMHG = "mmHg"
UNIT_BPM = "bpm"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HILO_CUFF_ID): cv.use_id(HiloCuffComponent),
        cv.Optional(CONF_SYSTOLIC): sensor.sensor_schema(
            unit_of_measurement=UNIT_MMHG,
            icon=ICON_HEART_PULSE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_DIASTOLIC): sensor.sensor_schema(
            unit_of_measurement=UNIT_MMHG,
            icon=ICON_HEART_PULSE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MAP): sensor.sensor_schema(
            unit_of_measurement=UNIT_MMHG,
            icon=ICON_HEART_PULSE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HEART_RATE): sensor.sensor_schema(
            unit_of_measurement=UNIT_BPM,
            icon="mdi:pulse",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon="mdi:battery",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_HILO_CUFF_ID])

    if CONF_SYSTOLIC in config:
        sens = await sensor.new_sensor(config[CONF_SYSTOLIC])
        cg.add(hub.set_systolic_sensor(sens))
    if CONF_DIASTOLIC in config:
        sens = await sensor.new_sensor(config[CONF_DIASTOLIC])
        cg.add(hub.set_diastolic_sensor(sens))
    if CONF_MAP in config:
        sens = await sensor.new_sensor(config[CONF_MAP])
        cg.add(hub.set_map_sensor(sens))
    if CONF_HEART_RATE in config:
        sens = await sensor.new_sensor(config[CONF_HEART_RATE])
        cg.add(hub.set_heart_rate_sensor(sens))
    if CONF_BATTERY_LEVEL in config:
        sens = await sensor.new_sensor(config[CONF_BATTERY_LEVEL])
        cg.add(hub.set_battery_sensor(sens))
