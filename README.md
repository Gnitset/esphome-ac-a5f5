# esphome-ac-a5f5

ESPHome custom climate component for AC units that communicate with their WiFi module
over a proprietary A5/F5 serial protocol. Developed by passively sniffing and
reverse-engineering the UART bus between the AC mainboard and the original Tuya WiFi MCU.

The built-in ESPHome `tuya:` component does **not** work with these units — they speak
a completely custom binary protocol, not the Tuya 55/AA protocol.

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full reverse-engineering notes.

---

## Hardware

Tested on an AC unit whose WiFi module is a Tuya-branded MCU that identifies itself
with the `A5 41` startup sequence. The AC mainboard communicates at **9600 baud, 8N1**.

Replacement controller: **ESP32-S3 Super Mini** (any ESP32 with two free GPIOs works).

### Wiring

```
AC mainboard TX  ──[10kΩ]──┬──  GPIO8  (ESP32 UART1 RX)
                           │
                         [20kΩ]
                           │
                          GND

AC mainboard RX  ──────────── GPIO9  (ESP32 UART1 TX)

GND              ──────────── GND
```

> **Voltage divider on RX line is important.** The AC board uses 5 V logic; the ESP32
> is 3.3 V tolerant only. Without the divider the MSB of received bytes can be
> corrupted (0xF5 frame-end byte appears as 0x75).

---

## Installation

### 1. Add to your ESPHome YAML

```yaml
external_components:
  - source: github://gnitset/esphome-ac-a5f5@main
    components: [ac_custom]

uart:
  id: ac_uart
  tx_pin: GPIO9
  rx_pin: GPIO8
  baud_rate: 9600
  data_bits: 8
  parity: NONE
  stop_bits: 1

climate:
  - platform: ac_custom
    uart_id: ac_uart
    id: ac_climate
    name: "AC Unit"
```

### 2. Add optional template entities

The component exposes extra state via getter methods. Wire them up as template
entities so they appear in Home Assistant:

```yaml
switch:
  - platform: template
    name: "AC Unit Swing"
    lambda: return id(ac_climate).get_swing();
    turn_on_action:  { lambda: id(ac_climate).set_swing(true); }
    turn_off_action: { lambda: id(ac_climate).set_swing(false); }

  - platform: template
    name: "AC Unit Sleep"
    lambda: return id(ac_climate).get_sleep();
    turn_on_action:  { lambda: id(ac_climate).set_sleep(true); }
    turn_off_action: { lambda: id(ac_climate).set_sleep(false); }

binary_sensor:
  - platform: template
    name: "AC Unit Water Tank Full"
    device_class: problem
    lambda: return id(ac_climate).get_water_full();

  - platform: template
    name: "AC Unit Fault"
    device_class: problem
    lambda: return id(ac_climate).get_fault();

text_sensor:
  - platform: template
    name: "AC Unit Status Bytes"
    entity_category: diagnostic
    update_interval: 12s
    lambda: return id(ac_climate).get_status_hex();
```

---

## Entities

| Entity | Type | Notes |
|--------|------|-------|
| AC Unit | Climate | Power, mode (Cool/Dry/Fan), setpoint 16–31 °C, current temp, fan Low/Med/High |
| Swing | Switch | Louver oscillation |
| Sleep | Switch | Sleep mode — forces fan to Low |
| Water Tank Full | Binary sensor | Always `false` until fault bit is identified — see below |
| Fault | Binary sensor | Always `false` until fault bit is identified — see below |
| Status Bytes (debug) | Text sensor | Raw hex of status bytes d[7]–d[12]; diagnostic category |

### Known AC behavioral constraints

These are enforced by the AC mainboard firmware, not by this component:

- **Dry mode** — fan is locked to Low; fan speed commands are ignored.
- **Fan-only mode** — setpoint cannot be changed; setpoint commands are ignored.
- **Sleep mode** — enabling Sleep forces fan speed to Low immediately.

---

## Identifying the Water Tank and Fault bits

The water-tank-full LED and general fault LED are reported somewhere in status bytes
`d[7]`–`d[12]`, but the exact bits have not yet been captured during a fault condition.

**How to find them:**

1. Flash this firmware and add the `Status Bytes` text sensor to your HA dashboard.
2. Note the normal value: `[7]=00 [8]=00 [9]=00 [10]=00 [11]=48 [12]=00`
3. Trigger the condition (fill the water tank / wait for fault LED to light).
4. The `Status Bytes` sensor will change — note which byte is now non-zero.
   The ESPHome device log will also print a `WARN` line with the same hex dump.
5. Identify which bit changed (e.g. `[7]=02` → d[7] bit 1).
6. Edit `ac_custom.h` and update the stub:

```cpp
bool get_water_full() const { return raw_d_[7] & 0x02; }  // example
```

7. Open a PR or issue with your findings so the component can be updated.

---

## Pinning a specific version

```yaml
external_components:
  - source: github://gnitset/esphome-ac-a5f5@v1.0.0
    components: [ac_custom]
```

Use a tag instead of `@main` for stable production installs.
