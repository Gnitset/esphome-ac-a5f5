import esphome.codegen as cg
from esphome.components import uart, climate

ac_custom_ns = cg.esphome_ns.namespace("ac_custom")
AcCustom = ac_custom_ns.class_("AcCustom", cg.Component, uart.UARTDevice, climate.Climate)
