import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart
from . import ac_custom_ns, AcCustom

DEPENDENCIES = ["climate", "uart"]

CONFIG_SCHEMA = (
    climate.climate_schema(AcCustom)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
