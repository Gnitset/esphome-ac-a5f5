# A5/F5 Serial Protocol — Reverse Engineering Notes

This document covers everything learned about the serial protocol used between the
AC mainboard and its Tuya WiFi MCU. All information was obtained by passively
sniffing the UART bus with an ESP32-S3 dual-UART sniffer and analysing captures
with a custom Python decoder.

---

## Background

The AC unit ships with a Tuya-branded WiFi module. When attempting to replace it with
an ESP32 running the ESPHome `tuya:` component, nothing worked at all — no packets
were exchanged. Passive sniffing revealed that the AC mainboard does **not** speak
the standard Tuya 55/AA protocol. It uses a completely custom binary framing with
0xA5 start byte and 0xF5 end byte.

---

## Sniffer setup

A second ESP32-S3 Super Mini was wired as a passive tap with no connection to the TX
lines — RX only on both channels:

```
AC mainboard TX  ──── GPIO8  (UART1 RX, monitors AC→MCU traffic)
WiFi MCU TX      ──── GPIO9  (UART2 RX, monitors MCU→AC traffic)
GND              ──── GND
```

Captures were logged to a host Mac via USB CDC serial using:

```bash
screen /dev/tty.usbmodem101 115200 | tee capture_NAME.log
```

Decoded with a custom Python analyzer (`analyzer/tuya_analyzer.py`) that handles
both directions, validates checksums, and produces a field-diff summary table showing
which bytes change between consecutive status packets.

---

## Physical layer

| Parameter | Value |
|-----------|-------|
| Baud rate | 9600 |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Logic level | 5 V (AC board) / 3.3 V (WiFi MCU) |

> The AC mainboard TX line is 5 V. Without a voltage divider on the ESP32 RX input
> the MSB of received bytes is occasionally corrupted — the 0xF5 frame-end byte
> appears as 0x75. Use a 10 kΩ / 20 kΩ voltage divider.

---

## Frame format

```
A5  [field1]  [len]  [data × len]  [checksum]  F5
```

| Byte | Description |
|------|-------------|
| `A5` | Start of frame |
| `field1` | Packet type / field identifier |
| `len` | Number of data bytes that follow |
| `data[0..len-1]` | Payload |
| `checksum` | `(field1 + len + data[0] + … + data[len-1]) & 0xFF` |
| `F5` | End of frame |

Minimum frame size: 4 bytes (A5, field1, len=0, chk=field1, F5 — not seen in practice).
Status packet size: 18 bytes (A5 + 00 + 0D + 13 data + chk + F5).
Command packet size: 6 bytes (A5 + field1 + 01 + value + chk + F5).

---

## AC → MCU: status packets

The AC mainboard sends an unsolicited status packet approximately every **10 seconds**,
and also immediately after processing a command.

```
field1 = 0x00
len    = 0x0D  (13 bytes)
```

### Data bytes

| Index | Name | Values | Notes |
|-------|------|--------|-------|
| d[0] | Power | `0x00`=OFF, `0x01`=ON | |
| d[1] | Mode | `0x01`=Cool, `0x02`=Dry, `0x03`=Fan | |
| d[2] | Sleep | `0x00`=OFF, `0x01`=ON | |
| d[3] | Room sensor | °C value, or °F+128 in °F mode | Read-only |
| d[4] | Setpoint | °C value 16–31, or °F+128 in °F mode | |
| d[5] | Swing | `0x00`=OFF, `0x01`=ON | |
| d[6] | Fan | `0x01`=Low, `0x02`=Medium, `0x03`=High | |
| d[7] | Reserved | always `0x00` | Possible fault byte — not confirmed |
| d[8] | Reserved | always `0x00` | |
| d[9] | Reserved | always `0x00` | |
| d[10] | Reserved | always `0x00` | |
| d[11] | Flags | bit2: `0`=°C, `1`=°F; other bits always `0x48` | See notes |
| d[12] | Water/fault | `0x00`=OK, `0x03`=tank full; other non-zero=fault | Unit beeps and stops cooling when non-zero |

### d[11] flags byte

In all captures d[11] = `0x48` in °C mode and (predicted) `0x4C` in °F mode.

```
0x48 = 0100 1000
         ^    ^
         |    bit3 — always 1 (purpose unknown)
         bit6 — always 1 (purpose unknown)
bit2 = 0 → °C display
bit2 = 1 → °F display
```

The component masks bit2 when checking for unexpected values, so the always-set
bits in d[11] do not trigger the "unknown bytes" warning.

### Temperature encoding

When the unit is in °C display mode, d[3] and d[4] are plain °C integers.

When in °F display mode, both bytes are stored as **(°F value + 128)**. The component
converts these to °C for Home Assistant:

```
°C = ((byte − 128) − 32) × 5 / 9
```

### Example status packet (Cool mode, 25 °C setpoint, room 24 °C, fan Low)

```
A5 00 0D 01 01 00 18 19 00 01 00 00 00 00 48 00 8D F5
         d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 dA dB dC  chk
```

- d[0]=01 → power ON
- d[1]=01 → Cool
- d[2]=00 → sleep OFF
- d[3]=18 → room 24 °C
- d[4]=19 → setpoint 25 °C
- d[5]=00 → swing OFF
- d[6]=01 → fan Low
- d[11]=48 → °C mode, standard flags

---

## MCU → AC: command packets

Each command sets exactly one field. The AC acknowledges by sending an updated status
packet within ~100 ms.

```
field1 = 0x10 + (1-based byte index)   [for most fields]
len    = 0x01
data   = new value
```

### Command table

| field1 | Field | Values | Confirmed |
|--------|-------|--------|-----------|
| `0x11` | Power | `0x00`=OFF, `0xFF`=ON | ✓ captured |
| `0x12` | Mode | `0x01`=Cool, `0x02`=Dry, `0x03`=Fan | ✓ captured |
| `0x13` | Sleep | `0x00`=OFF, `0x01`=ON | ✓ captured |
| `0x14` | Setpoint | `0x10`–`0x1F` (°C 16–31) | ✓ captured |
| `0x15` | Swing | `0x00`=OFF, `0x01`=ON | ✓ captured |
| `0x16` | Fan | `0x01`=Low, `0x02`=Med, `0x03`=High | ✓ captured |
| `0x19` | C/F mode | `0x00`=°F, `0x01`=°C (assumed) | captured, AC may ignore |

> **Power ON is 0xFF, not 0x01.** This is the most surprising finding.
> The AC will not turn on if sent `0x01`. Every capture of a power-on command
> shows `0xFF` as the data byte. Power OFF is `0x00` as expected.

### Example: set setpoint to 22 °C

```
A5 14 01 16 2B F5
   ^^          field1 = 0x14 (setpoint command)
      ^^       len = 1
         ^^    data = 0x16 = 22
            ^^ checksum = (0x14 + 0x01 + 0x16) & 0xFF = 0x2B
```

---

## MCU → AC: keep-alive packets

### Heartbeat

Sent every ~60 seconds to keep the AC from timing out.

```
A5 01 01 00 02 F5
```

field1=`0x01`, data=`0x00`, checksum=`0x02`.

### WiFi module startup sequence

Sent once after power-on, triggered by the first received status packet:

**Step 1** — immediately after first status packet:
```
A5 41 01 00 42 F5   (WiFi module initialising)
```

**Step 2** — ~5 seconds later:
```
A5 41 01 03 45 F5   (WiFi module connected / ready)
```

The AC mainboard appears to change its displayed WiFi indicator based on these.
If the ESP32 does not send step 2, the AC may show a WiFi error LED.

A single unknown byte `0xE0` has been observed at the very start of the UART bus
immediately after AC power-on, before any framed packets. This is likely a sync/wake
byte emitted by the AC mainboard and can be ignored.

---

## C/F display toggle (field1 = 0x19)

When toggling the temperature unit in the Tuya app, the MCU sends a 5-command
resync sequence rather than a single toggle:

```
A5 11 01 FF 11 F5   power ON
A5 14 01 <°C>  F5   setpoint in °C
A5 12 01 <mode> F5  current mode
A5 19 01 00 1A F5   C/F toggle (0x00 = °F mode)
A5 14 01 <°F+128> F5  setpoint re-sent in °F+128 format
```

**Observed behaviour:** in all captures the AC status byte d[11] never changed from
`0x48` (°C mode) in response to this sequence. The app retried the sequence 3–4 times
before giving up. This suggests either:

- This particular AC mainboard does not support UART-controlled C/F toggle.
- The sequence requires an additional precondition not yet identified.

The component sends `A5 19 01 [00|01] F5` when `set_celsius_mode()` is called, but
the AC may silently ignore it. The °C/°F state reported by `get_celsius_mode()` is
read from d[11] bit2 in incoming status packets.

---

## Timer

**The timer is cloud-only.** When a timer is set in the Tuya app, the UART bus shows
absolutely no traffic — not a single byte. The timer is implemented as a Tuya cloud
schedule that fires a power command after the specified delay. It has no UART
representation and cannot be replicated by the ESP32 component.

Bytes d[7]–d[10] and d[12] remain `0x00` regardless of timer state.

---

## AC behavioral constraints

These are enforced by the AC mainboard firmware and cannot be overridden by the MCU:

| Condition | Constraint |
|-----------|------------|
| Mode = Dry | Fan speed locked to Low; fan commands ignored |
| Mode = Fan-only | Setpoint cannot be changed; setpoint commands ignored |
| Sleep enabled | Fan speed immediately forced to Low |

---

## Unknown / unconfirmed bytes

Bytes d[7]–d[10] always read `0x00` in all captures. Their purpose is unknown.

**d[12]** is confirmed: `0x00` = normal, `0x03` = water tank full (unit beeps and stops
cooling). Any other non-zero value is treated as a general fault.

The `get_status_hex()` method and `Status Bytes` text sensor in HA expose all of
these bytes for future fault capture.

---

## Capture methodology

### Tools used

- **Passive sniffer**: ESP32-S3 Super Mini, dual HardwareSerial, GPIO8 + GPIO9,
  outputs timestamped hex lines to USB CDC serial.
  (`sniffer/esp32_sniffer/esp32_sniffer.ino`)
- **Capture**: `screen /dev/tty.usbmodem101 115200 | tee capture_NAME.log`
- **Decoder**: `analyzer/tuya_analyzer.py` — decodes both directions, validates
  checksums, labels all known fields, prints a field-diff summary table at the end.

### Methodology

1. **Passive baseline** — capture button presses from the physical remote to map
   the read path: which status bytes change for each control.
2. **Write path** — capture commands sent by the Tuya app to the AC to identify
   the command field1 values and data encoding.
3. **Round-trip verification** — after predicting all command bytes, capture each
   individually from the app (power, mode, setpoint, fan, swing, sleep, C/F, timer)
   and verify predictions against actual captures.
4. **Field diff** — the analyzer script compares consecutive status packets and
   highlights which bytes changed, isolating the effect of each command.

### Key discoveries

- The protocol is not Tuya 55/AA. The ESPHome `tuya:` component is completely
  incompatible with these units.
- Power ON uses `0xFF`, not `0x01`. This was found during write-path capture.
- The `field1 = 0x10 + (1-based byte index)` pattern holds for all confirmed
  command bytes (0x11–0x16, 0x19 fits a gap in this pattern).
- The °F encoding (byte = °F + 128) was identified by toggling the C/F button
  and observing d[3] and d[4] jump above 0x80.
- Timer is cloud-only — confirmed by an empty capture log with no UART activity
  while setting and cancelling timers in the Tuya app.

---

## Confirmed field table

| Field | Status byte | Command field1 | Status read | Command write |
|-------|-------------|----------------|-------------|---------------|
| Power | d[0] | 0x11 | ✓ confirmed | ✓ OFF=0x00, ON=0xFF |
| Mode | d[1] | 0x12 | ✓ confirmed | ✓ Cool/Dry/Fan |
| Sleep | d[2] | 0x13 | ✓ confirmed | ✓ 0x00/0x01 |
| Room sensor | d[3] | — read-only | ✓ confirmed | — |
| Setpoint | d[4] | 0x14 | ✓ confirmed | ✓ °C 16–31 |
| Swing | d[5] | 0x15 | ✓ confirmed | ✓ 0x00/0x01 |
| Fan | d[6] | 0x16 | ✓ confirmed | ✓ 0x01/0x02/0x03 |
| Timer | d[7–12]? | none | never seen | cloud-only, no UART |
| C/F display | d[11] bit2 | 0x19 | ✓ confirmed | captured; AC may ignore |
| Water tank full | d[12]==0x03 | — read-only | ✓ confirmed | 0x03 observed when full LED lit |
| Fault | d[12]!=0x00 | — read-only | assumed | any non-zero d[12], including tank full |
