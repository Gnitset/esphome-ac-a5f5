import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, climate

DEPENDENCIES = ["uart", "climate"]

ac_custom_ns = cg.esphome_ns.namespace("ac_custom")
AcCustom = ac_custom_ns.class_("AcCustom", cg.Component, uart.UARTDevice, climate.Climate)

CONFIG_SCHEMA = (
    climate.climate_schema(AcCustom)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
