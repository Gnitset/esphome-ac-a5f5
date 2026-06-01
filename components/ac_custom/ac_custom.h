#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"

/*
 * Custom ESPHome climate component for AC units using the A5/F5 serial protocol.
 *
 * Protocol summary (fully reverse-engineered):
 *
 *   FRAME:  A5  [field1]  [len]  [data × len]  [checksum]  F5
 *   checksum = sum(field1, len, data...) mod 256
 *
 *   AC→MCU status packets (field1=0x00, len=0x0D, sent every ~10 s):
 *     d[0]  power     0x00=OFF  0x01=ON
 *     d[1]  mode      0x01=Cool  0x02=Dry  0x03=Fan
 *     d[2]  sleep     0x00=OFF  0x01=ON
 *     d[3]  sensor    room temp °C, or °F+128 in °F display mode
 *     d[4]  setpoint  target °C (16–31), or °F+128 in °F display mode
 *     d[5]  swing     0x00=OFF  0x01=ON
 *     d[6]  fan       0x01=Low  0x02=Medium  0x03=High
 *     d[7-10]         reserved (always 0x00)
 *     d[11] flags     bit2=0→°C display  bit2=1→°F display
 *     d[12]           reserved (always 0x00)
 *
 *   MCU→AC commands (each sets one field):
 *     0x11  power     0x00=OFF  0xFF=ON  (NOTE: ON is 0xFF, not 0x01)
 *     0x12  mode      0x01=Cool  0x02=Dry  0x03=Fan
 *     0x13  sleep     0x00=OFF  0x01=ON
 *     0x14  setpoint  °C value 16–31, or °F+128 when in °F mode
 *     0x15  swing     0x00=OFF  0x01=ON
 *     0x16  fan       0x01=Low  0x02=Medium  0x03=High
 *     0x19  cf_mode   0x00=°F display  0x01=°C display
 *             (app sends this as part of a full-state resync; this AC unit
 *              may silently ignore it — d[11] never changed in captures)
 *
 *   MCU→AC keep-alive:
 *     0x01  heartbeat  len=1  data=0x00   every ~60 s
 *     0x41  WiFi init  len=1  data=0x00   sent once at startup
 *     0x41  WiFi ready len=1  data=0x03   sent ~5 s after first status packet
 */

namespace esphome {
namespace ac_custom {

static const char *const TAG = "ac_custom";

class AcCustom : public Component, public uart::UARTDevice, public climate::Climate {
 public:
  // ── ESPHome lifecycle ─────────────────────────────────────────────────────

  void setup() override {
    setup_time_     = millis();
    last_heartbeat_ = millis();
  }

  void loop() override {
    read_uart_();
    handle_startup_();
    handle_heartbeat_();
  }

  float get_setup_priority() const override { return setup_priority::DATA; }

  // ── Climate traits ────────────────────────────────────────────────────────

  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    t.set_supports_current_temperature(true);
    t.set_supported_modes({
        climate::CLIMATE_MODE_OFF,
        climate::CLIMATE_MODE_COOL,
        climate::CLIMATE_MODE_DRY,
        climate::CLIMATE_MODE_FAN_ONLY,
    });
    t.set_supported_fan_modes({
        climate::CLIMATE_FAN_LOW,
        climate::CLIMATE_FAN_MEDIUM,
        climate::CLIMATE_FAN_HIGH,
    });
    t.set_visual_min_temperature(16.0f);
    t.set_visual_max_temperature(31.0f);
    t.set_visual_temperature_step(1.0f);
    return t;
  }

  // ── Climate control (called when HA changes state) ────────────────────────

  void control(const climate::ClimateCall &call) override {
    if (call.get_mode().has_value()) {
      auto m = *call.get_mode();
      if (m == climate::CLIMATE_MODE_OFF) {
        send_cmd_(0x11, 0x00);
      } else {
        send_cmd_(0x11, 0xFF);  // AC requires 0xFF for ON, not 0x01
        switch (m) {
          case climate::CLIMATE_MODE_COOL:     send_cmd_(0x12, 0x01); break;
          case climate::CLIMATE_MODE_DRY:      send_cmd_(0x12, 0x02); break;
          case climate::CLIMATE_MODE_FAN_ONLY: send_cmd_(0x12, 0x03); break;
          default: break;
        }
      }
    }
    if (call.get_target_temperature().has_value()) {
      float t = *call.get_target_temperature();
      auto degc = (uint8_t) std::max(16.0f, std::min(31.0f, roundf(t)));
      send_cmd_(0x14, degc);
    }
    if (call.get_fan_mode().has_value()) {
      switch (*call.get_fan_mode()) {
        case climate::CLIMATE_FAN_LOW:    send_cmd_(0x16, 0x01); break;
        case climate::CLIMATE_FAN_MEDIUM: send_cmd_(0x16, 0x02); break;
        case climate::CLIMATE_FAN_HIGH:   send_cmd_(0x16, 0x03); break;
        default: break;
      }
    }
  }

  // ── Public command API (callable from YAML lambdas) ───────────────────────

  void set_swing(bool on)         { send_cmd_(0x15, on ? 0x01 : 0x00); }
  void set_sleep(bool on)         { send_cmd_(0x13, on ? 0x01 : 0x00); }
  void set_celsius_mode(bool cel) { send_cmd_(0x19, cel ? 0x01 : 0x00); }

  bool get_swing()        const { return swing_; }
  bool get_sleep()        const { return sleep_; }
  bool get_celsius_mode() const { return celsius_mode_; }

  // ── Fault / status queries ─────────────────────────────────────────────────
  // These return false until the exact byte/bit is identified.
  // To discover: trigger the condition, watch the "Status Bytes" text sensor
  // in HA (it shows d[7]–d[12] in hex), note which byte/bit changes, then
  // update the expression below and remove the TODO comment.

  // Water-tank-full LED: unit stops cooling until tank is emptied.
  bool get_water_full() const {
    return false;  // TODO: fill tank, note changed byte in status_hex(), set correct bit
  }

  // General fault (Fail LED on unit).
  bool get_fault() const {
    return false;  // TODO: trigger fault, note changed byte in status_hex(), set correct bit
  }

  // Raw hex of d[7]–d[12] for live inspection in HA.
  // All reserved / not-yet-decoded bytes live here.
  // Watch this sensor when triggering fault conditions to identify bits.
  std::string get_status_hex() const {
    char buf[48];
    snprintf(buf, sizeof(buf),
             "[7]=%02X [8]=%02X [9]=%02X [10]=%02X [11]=%02X [12]=%02X",
             raw_d_[7], raw_d_[8], raw_d_[9], raw_d_[10], raw_d_[11], raw_d_[12]);
    return std::string(buf);
  }

 protected:
  // ── TX ────────────────────────────────────────────────────────────────────

  void send_cmd_(uint8_t field1, uint8_t value) {
    uint8_t buf[6];
    buf[0] = 0xA5;
    buf[1] = field1;
    buf[2] = 0x01;
    buf[3] = value;
    buf[4] = (uint8_t)((field1 + 0x01u + value) & 0xFFu);
    buf[5] = 0xF5;
    write_array(buf, 6);
    ESP_LOGD(TAG, "TX: %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
  }

  // ── Startup sequence ──────────────────────────────────────────────────────

  void handle_startup_() {
    if (last_status_rx_ == 0) return;  // wait for first status packet
    uint32_t now = millis();

    if (!wifi_init_sent_) {
      send_cmd_(0x41, 0x00);
      wifi_init_sent_ = true;
      ESP_LOGI(TAG, "Sent WiFi-init (0x00)");
    }
    if (!wifi_ready_sent_ && (now - last_status_rx_) >= 5000) {
      send_cmd_(0x41, 0x03);
      wifi_ready_sent_ = true;
      ESP_LOGI(TAG, "Sent WiFi-ready (0x03)");
    }
  }

  // ── Heartbeat ─────────────────────────────────────────────────────────────

  void handle_heartbeat_() {
    if ((millis() - last_heartbeat_) >= 60000UL) {
      send_cmd_(0x01, 0x00);
      last_heartbeat_ = millis();
      ESP_LOGD(TAG, "Heartbeat sent");
    }
  }

  // ── RX: accumulate bytes, scan for complete packets ───────────────────────

  void read_uart_() {
    while (available()) {
      if (rx_len_ < sizeof(rx_buf_))
        rx_buf_[rx_len_++] = (uint8_t) read();
      else
        rx_len_ = 0;  // overflow — re-sync
    }
    // Try to extract packets from the buffer
    while (rx_len_ >= 18) {
      int found = -1;
      for (int i = 0; i <= (int) rx_len_ - 18; i++) {
        if (rx_buf_[i]    == 0xA5 &&
            rx_buf_[i+1]  == 0x00 &&
            rx_buf_[i+2]  == 0x0D &&
            rx_buf_[i+17] == 0xF5) {
          uint8_t chk = 0;
          for (int j = i + 1; j < i + 17; j++) chk += rx_buf_[j];
          if (chk == rx_buf_[i + 16]) {
            process_status_(rx_buf_ + i + 3);
            found = i + 18;
            break;
          }
        }
      }
      if (found < 0) {
        // No valid header found — drop one byte and try again
        memmove(rx_buf_, rx_buf_ + 1, rx_len_ - 1);
        rx_len_--;
        return;
      }
      memmove(rx_buf_, rx_buf_ + found, rx_len_ - found);
      rx_len_ -= (uint8_t) found;
    }
  }

  // ── Status packet decoder ─────────────────────────────────────────────────

  static float decode_temp_(uint8_t v) {
    if (v & 0x80) {
      // °F display mode: stored as (°F + 128), convert to °C for HA
      return ((float)(v - 0x80) - 32.0f) * 5.0f / 9.0f;
    }
    return (float) v;
  }

  void process_status_(const uint8_t *d) {
    last_status_rx_ = millis();
    memcpy(raw_d_, d, 13);

    bool    power    = (d[0] != 0x00);
    uint8_t mode     = d[1];
    bool    slp      = (d[2] != 0x00);
    float   sensor   = decode_temp_(d[3]);
    float   setpoint = decode_temp_(d[4]);
    bool    swng     = (d[5] != 0x00);
    uint8_t fan      = d[6];
    bool    cel      = !(d[11] & 0x04);  // bit2=0 → °C, bit2=1 → °F

    ESP_LOGD(TAG,
             "Status: pwr=%d mode=%d sleep=%d sensor=%.1f sp=%.1f swing=%d fan=%d cf=%s",
             power, mode, slp, sensor, setpoint, swng, fan, cel ? "C" : "F");

    // Log when any reserved byte is non-zero — indicates an undecoded fault or flag.
    // The text sensor in HA also shows these; this makes them visible in device logs.
    bool any_unknown = (d[7] || d[8] || d[9] || d[10] || (d[11] & ~0x04u) || d[12]);
    if (any_unknown) {
      ESP_LOGW(TAG,
               "Unknown status bytes: [7]=%02X [8]=%02X [9]=%02X [10]=%02X [11]=%02X [12]=%02X",
               d[7], d[8], d[9], d[10], d[11], d[12]);
    }

    // Update stored states for lambda access
    swing_        = swng;
    sleep_        = slp;
    celsius_mode_ = cel;

    // Update climate entity
    current_temperature = sensor;
    target_temperature  = setpoint;

    if (!power) {
      this->mode = climate::CLIMATE_MODE_OFF;
    } else {
      switch (mode) {
        case 0x01: this->mode = climate::CLIMATE_MODE_COOL;     break;
        case 0x02: this->mode = climate::CLIMATE_MODE_DRY;      break;
        case 0x03: this->mode = climate::CLIMATE_MODE_FAN_ONLY; break;
        default:   this->mode = climate::CLIMATE_MODE_COOL;     break;
      }
    }

    switch (fan) {
      case 0x01: fan_mode = climate::CLIMATE_FAN_LOW;    break;
      case 0x02: fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
      case 0x03: fan_mode = climate::CLIMATE_FAN_HIGH;   break;
      default:   fan_mode = climate::CLIMATE_FAN_LOW;    break;
    }

    publish_state();
  }

  // ── Members ───────────────────────────────────────────────────────────────

  uint8_t  rx_buf_[64]{};
  uint8_t  rx_len_{0};

  uint32_t setup_time_{0};
  uint32_t last_heartbeat_{0};
  uint32_t last_status_rx_{0};

  bool wifi_init_sent_{false};
  bool wifi_ready_sent_{false};

  bool    swing_{false};
  bool    sleep_{false};
  bool    celsius_mode_{true};

  uint8_t raw_d_[13]{};
};

}  // namespace ac_custom
}  // namespace esphome
