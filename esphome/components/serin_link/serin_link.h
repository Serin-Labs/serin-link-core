#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include <functional>
#include <string>
#include <vector>

#include "sl2_link.h"
#include "sl2_rxq.h"

namespace esphome {
namespace serin_link {

/* ESPHome adapter around the platform-free sl2_link core.
 *
 * Bind any `climate` entity (climate_id:) and the component derives the CAPS
 * descriptor from its ClimateTraits, serves STATE from the entity, and routes
 * dial CMDs through a ClimateCall — every ESPHome climate platform becomes a
 * Serin-dial-controllable zone. Without climate_id it runs a canned device
 * (the coexistence spike).
 *
 * Threading: the ESP-NOW recv callback (Wi-Fi task) only pushes raw frames
 * into an SPSC ring; loop() drains it, satisfying the core's same-context
 * contract. */
class SerinLinkComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /* LATE, matching ESPHome's own espnow component: esp_now_init() must run
   * AFTER esp_wifi_start(). At AFTER_WIFI this setup ran mid-wifi-bringup and
   * the radio came up half-wedged — TX and broadcast RX worked, but unicast
   * to our MAC was never ACKed (dial saw sent=172 acked=0 on every channel). */
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_zone_name(const std::string &name) { zone_name_ = name; }
  void set_climate(climate::Climate *c) { climate_ = c; }
  /* cmd_debounce: — trailing quiet window before a dial CMD burst is applied
   * to the entity as one ClimateCall (0 = apply immediately). */
  void set_cmd_debounce(uint32_t ms) { cmd_debounce_ms_ = ms; }
  /* hvac_link: — platform-specific device-link health (a generic climate
   * entity exists whether or not the device behind it answers). Unset, the
   * NaN-room-temp heuristic applies (sl2_hvac_link_infer). */
  void set_hvac_link_lambda(std::function<bool()> fn) { hvac_link_fn_ = std::move(fn); }
  /* vane_v_select:/vane_h_select: — vane axes bound to select entities; the
   * option order defines wire positions, "auto"/"swing" map to those codes. */
  void set_vane_v_select(select::Select *s) { vane_v_sel_ = s; }
  void set_vane_h_select(select::Select *s) { vane_h_sel_ = s; }

  /* Telemetry bindings — each populates an INFO TLV and declares the
   * matching SL2_FEAT_* capability bit (spec §8/§9). All optional; an
   * unbound source simply omits its TLV and the dial hides the row. */
  void set_outside_temp_sensor(sensor::Sensor *s) { outside_temp_sensor_ = s; }
  void set_compressor_hz_sensor(sensor::Sensor *s) { compressor_hz_sensor_ = s; }
  void set_stage_sensor(text_sensor::TextSensor *s) { stage_sensor_ = s; }
  void set_sub_mode_sensor(text_sensor::TextSensor *s) { sub_mode_sensor_ = s; }
  void set_auto_sub_mode_sensor(text_sensor::TextSensor *s) { auto_sub_mode_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { battery_sensor_ = s; }
  void set_battery_low_threshold(uint8_t pct) { batt_low_threshold_ = pct; }
  void set_runtime_sensor(sensor::Sensor *s) { runtime_sensor_ = s; }
  void set_power_sensor(sensor::Sensor *s) { power_sensor_ = s; }
  void set_energy_sensor(sensor::Sensor *s) { energy_sensor_ = s; }

  /* link_sensor: — the dial's OWN room sensor (DIAL_SENSOR, wire spec §10d).
   * Unlike the telemetry bindings above (which point at entities the user
   * already has), these entities are owned by this component: the dial is
   * the data source, so there is nothing pre-existing to bind to. Presence
   * of the YAML block is the opt-in and gates SL2_FEAT_LINK_SENSOR. */
  void set_link_sensor_enabled() { link_sensor_cfg_ = true; }
  void set_dial_temp_sensor(sensor::Sensor *s) { dial_temp_sensor_ = s; }
  void set_dial_hum_sensor(sensor::Sensor *s) { dial_hum_sensor_ = s; }
  void set_dial_mac_sensor(text_sensor::TextSensor *s) { dial_mac_sensor_ = s; }
  void set_dial_stale_after(uint32_t ms) { dial_stale_ms_ = ms; }

  /* A bonded dial reported its own room sensor (called from the trampoline). */
  void room_sensor_feed(const uint8_t src_mac[6],
                        const struct sl2_dial_sensor_pkt *p, bool is_edit);

  /* For actuation automations in YAML: only feed the heat pump when the
   * dial actually selected itself as the room source. */
  bool room_src_is_link() const { return selected_src_ == SL2_ROOMSRC_LINK; }

  /* For template buttons / lambdas in YAML. */
  void pair_start(uint32_t window_ms = 60000) { sl2_link_pair_start(&link_, window_ms); }
  void pair_cancel() { sl2_link_pair_cancel(&link_); }
  bool pairing() const { return sl2_link_pairing(&link_); }
  const char *pair_result() const { return sl2_link_pair_result(&link_); }
  int dial_count() const { return sl2_link_dial_count(&link_); }
  bool any_dial_live() { return sl2_link_any_live(&link_); }
  void forget_all_dials() { sl2_link_forget_all(&link_); }

  /* HVAC iface backing (public: called from the C hook trampolines). */
  bool hvac_get_state(sl2_hvac_state_t *out);
  bool hvac_apply(uint16_t mask, const struct sl2_cmd_pkt *cmd);
  bool hvac_get_caps(struct sl2_caps_pkt *out);
  size_t fill_info_tlvs(uint8_t *buf, size_t cap);
  void copy_zone_name(char *dst, size_t cap) const;

  /* Internals shared with the static ESP-NOW callbacks. */
  sl2_rxq_t rxq_;

 protected:
  /* ordered detent list of the entity's discrete fan modes (excl. auto) */
  void rebuild_fan_detents_();
  /* wire setpoints clamp to the entity's visual range (not every platform
   * clamps in its own control()) */
  float clamp_setpoint_(float c);
  /* Dial CMDs are staged, not applied inline: a burst of detent edits merges
   * into hold_ (latest wins) and one ClimateCall goes out after cmd_debounce_
   * of quiet. The same staged values overlay hvac_get_state until the entity
   * confirms each field — the immediate post-CMD STATE echo would otherwise
   * carry the entity's pre-command state on async platforms (cn105 confirms
   * over serial), and the stale echo snaps the dial back mid-adjustment. */
  void stage_(uint16_t bit);
  void apply_pending_();
  void apply_overlay_(sl2_hvac_state_t *out, bool two_point);
  sl2_link_t link_{};
  sl2_port_t port_{};
  sl2_crypto_t crypto_{};
  sl2_hvac_iface_t hvac_{};
  std::string zone_name_;
  climate::Climate *climate_{nullptr};
  std::function<bool()> hvac_link_fn_{nullptr};
  select::Select *vane_v_sel_{nullptr};
  select::Select *vane_h_sel_{nullptr};
  sensor::Sensor *outside_temp_sensor_{nullptr};
  sensor::Sensor *compressor_hz_sensor_{nullptr};
  text_sensor::TextSensor *stage_sensor_{nullptr};
  text_sensor::TextSensor *sub_mode_sensor_{nullptr};
  text_sensor::TextSensor *auto_sub_mode_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  sensor::Sensor *runtime_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *energy_sensor_{nullptr};
  uint8_t batt_low_threshold_{10};
  /* low-battery hysteresis latch: on at <= threshold, off at >= threshold+5
   * (a cell hovering at the line must not flap the dial's home-face chip) */
  bool batt_low_latch_{false};
  /* Publish the dial reading to HA. stale=true publishes NAN (HA renders it
   * as unknown rather than a frozen number) and latches until a fresh
   * reading arrives. */
  void publish_dial_(bool stale);
  bool link_sensor_cfg_{false};
  sensor::Sensor *dial_temp_sensor_{nullptr};
  sensor::Sensor *dial_hum_sensor_{nullptr};
  text_sensor::TextSensor *dial_mac_sensor_{nullptr};
  uint32_t dial_stale_ms_{90000};
  int16_t dial_temp_dc_{SL2_DC_NA};
  uint8_t dial_hum_pct_{SL2_HUM_NA};
  /* millis() of the last frame carrying a VALID temperature; 0 = never.
   * Freshness follows the TEMPERATURE, not the frame: a dial with sensing
   * hardware but no reading yet must not hold off the stale watchdog. */
  uint32_t dial_temp_ms_{0};
  bool dial_has_sensor_{false};
  uint8_t dial_mac_[6]{};
  int16_t dial_pub_dc_{SL2_DC_NA};   /* last value published to HA */
  uint32_t dial_pub_ms_{0};          /* millis() of that publish; 0 = never */
  bool dial_stale_{false};           /* NAN already published */
  uint32_t last_dial_check_ms_{0};
  /* Health of the SELECTED room source, for the ROOM_SRC TLV. */
  uint8_t room_src_status_() const;
  uint8_t selected_src_{SL2_ROOMSRC_INTERNAL};
  ESPPreferenceObject room_src_pref_;
  ESPPreferenceObject caps_fp_pref_;   /* fingerprint: announce caps changes */
  std::vector<climate::ClimateFanMode> fan_detents_;
  bool fan_has_auto_{false};
  bool use_f_{false};                    /* per-controller display pref (CM_UNITS) */
  ESPPreferenceObject use_f_pref_;
  /* staged CMD fields, wire format, normalized to what the entity will report
   * back once it confirms (clamped temps, canonical fan percents) */
  struct sl2_cmd_pkt hold_ {};
  uint16_t pending_mask_{0};             /* staged, not yet applied to the entity */
  uint16_t overlay_mask_{0};             /* still masking STATE (confirm/timeout clears) */
  uint32_t overlay_since_ms_{0};
  uint32_t cmd_debounce_ms_{300};
  uint32_t last_ps_check_ms_{0};
  bool started_{false};
};

}  // namespace serin_link
}  // namespace esphome
