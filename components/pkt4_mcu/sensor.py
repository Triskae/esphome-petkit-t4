import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
	CONF_DISTANCE,
	CONF_ID,
	CONF_WEIGHT,
	DEVICE_CLASS_DISTANCE,
	DEVICE_CLASS_WEIGHT,
	ENTITY_CATEGORY_DIAGNOSTIC,
	STATE_CLASS_MEASUREMENT,
	UNIT_CENTIMETER,
	UNIT_KILOGRAM,
)
from . import PKT4MCUComponent


DEPENDENCIES = ('pkt4_mcu',)

CONF_WEIGHT_SAMPLE = 'weight_sample'

MOTORS = {'drum': 0, 'tray': 1}
MOTOR_SENSORS = {
	# key suffix: (setter, schema)
	'position': ('set_motor_position_sensor', sensor.sensor_schema(
		accuracy_decimals=0,
		icon='mdi:map-marker',
		entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
	)),
	'stop_code': ('set_motor_stop_code_sensor', sensor.sensor_schema(
		accuracy_decimals=0,
		icon='mdi:alert-octagon-outline',
		entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
	)),
	'current': ('set_motor_current_sensor', sensor.sensor_schema(  # raw MCU value, unit unknown
		accuracy_decimals=0,
		icon='mdi:current-dc',
		state_class=STATE_CLASS_MEASUREMENT,
		entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
	)),
}

CONFIG_SCHEMA = cv.Schema({
	cv.GenerateID(): cv.use_id(PKT4MCUComponent),
	cv.Optional(CONF_DISTANCE): sensor.sensor_schema(
		unit_of_measurement=UNIT_CENTIMETER,
		accuracy_decimals=0,
		device_class=DEVICE_CLASS_DISTANCE,
		state_class=STATE_CLASS_MEASUREMENT,
	),
	cv.Optional(CONF_WEIGHT): sensor.sensor_schema(
		unit_of_measurement=UNIT_KILOGRAM,
		accuracy_decimals=2,
		device_class=DEVICE_CLASS_WEIGHT,
		state_class=STATE_CLASS_MEASUREMENT,
	),
	cv.Optional(CONF_WEIGHT_SAMPLE): sensor.sensor_schema(accuracy_decimals=0),
	**{cv.Optional(f'{motor}_{key}'): schema for motor in MOTORS for key, (_, schema) in MOTOR_SENSORS.items()},
})


async def to_code(config):
	var = await cg.get_variable(config[CONF_ID])

	if distance_config := config.get(CONF_DISTANCE):
		sens = await sensor.new_sensor(distance_config)
		cg.add(var.set_distance_sensor(sens))

	if weight_config := config.get(CONF_WEIGHT):
		sens = await sensor.new_sensor(weight_config)
		cg.add(var.set_weight_sensor(sens))

	if sample_config := config.get(CONF_WEIGHT_SAMPLE):
		sens = await sensor.new_sensor(sample_config)
		cg.add(var.set_weight_sample_sensor(sens))

	for motor, index in MOTORS.items():
		for key, (setter, _) in MOTOR_SENSORS.items():
			if motor_config := config.get(f'{motor}_{key}'):
				sens = await sensor.new_sensor(motor_config)
				cg.add(getattr(var, setter)(index, sens))
