#pragma once
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include <array>
#include <cmath>
#include <cstring>
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
  /* hvac_link: — platform-specific device-link health (a generic climate
   * entity exists whether or not the device behind it answers). Unset, the
   * NaN-room-temp heuristic applies (sl2_hvac_link_infer). */
  void set_hvac_link_lambda(std::function<bool()> fn) { hvac_link_fn_ = std::move(fn); }
  /* hvac_link_sensor: — the same signal from a binary_sensor entity. The
   * schema enforces lambda-or-sensor, never both. */
  void set_hvac_link_sensor(binary_sensor::BinarySensor *s) { hvac_link_sensor_ = s; }
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
  /* link_sensor: links: — per-slot temperature/humidity rows. Unlike the
   * arbitrated pair above these show EVERY bonded Link's reading, non-primary
   * included; arbitration decides which Link feeds the pair and the heat
   * pump, never which readings are visible. A row is a bond SLOT (the
   * compaction caveat on add_dial_row applies). */
  void add_sensor_row(int idx, sensor::Sensor *temp, sensor::Sensor *hum) {
    if (idx < 0 || idx >= SL2_MAX_DIALS) return;
    sensor_rows_[idx].temp = temp;
    sensor_rows_[idx].hum = hum;
    sensor_rows_[idx].declared = true;
    sensor_rows_cfg_ = true;
  }
  /* on_room_temperature: — fired from publish_dial_() so it inherits the
   * frame-driven dedup gate; only for fresh readings while the Serin Link is
   * the selected room source (the guards the YAML recipe used to carry). */
  void add_room_temp_trigger(Trigger<float> *t) { room_temp_triggers_.push_back(t); }
  void set_room_source_select(select::Select *s) { room_source_select_ = s; }
  /* room_temperature_source: sources: — an external reading (any ESPHome
   * sensor) offered as a selectable room source. Registered in YAML order;
   * the position is the dropdown index (after Heat pump) and the catalog
   * position on the dial. The id is a hash of the label (sl2_proto.h). */
  void add_room_source(const std::string &name, sensor::Sensor *s);
  void room_source_select_control(size_t index);
  bool room_catalog_page(uint16_t cursor, struct sl2_room_source_entry *entries,
                         uint8_t cap, uint8_t *count, uint16_t *next,
                         uint32_t *revision);
  bool room_source_get(uint32_t *revision, uint64_t *source_id, uint8_t *status);
  uint8_t room_source_set(uint32_t revision, uint64_t source_id);

  /* A bonded dial reported its own room sensor (called from the trampoline). */
  void room_sensor_feed(const uint8_t src_mac[6],
                        const struct sl2_dial_sensor_pkt *p, bool is_edit);

  /* True while a Serin Link (always a specific, pinned one) is the selected
   * room source. */
  bool room_src_is_link() const { return selected_src_ == SL2_ROOMSRC_LINK; }

  /* For template buttons / lambdas in YAML. */
  void pair_start(uint32_t window_ms = 60000) { sl2_link_pair_start(&link_, window_ms); }
  void pair_cancel() { sl2_link_pair_cancel(&link_); }
  bool pairing() const { return sl2_link_pairing(&link_); }
  const char *pair_result() const { return sl2_link_pair_result(&link_); }
  int dial_count() const { return sl2_link_dial_count(&link_); }
  bool any_dial_live() { return sl2_link_any_live(&link_); }
  void forget_all_dials() { sl2_link_forget_all(&link_); }

  /* Per-dial management. The core's bond table is COMPACTED on forget
   * (sl2_link_forget_dial), so an index identifies a bond SLOT, not a dial:
   * forgetting slot 0 shifts every later dial down one. Callers needing a
   * stable identity must use the MAC. */
  bool forget_dial_slot(int idx) {
    uint8_t mac[6];
    if (!sl2_link_dial_mac(&link_, idx, mac)) return false;
    return sl2_link_forget_dial(&link_, mac);
  }
  bool forget_dial_mac(const uint8_t mac[6]) {
    return sl2_link_forget_dial(&link_, mac);
  }
  int pair_seconds_left() { return sl2_link_pair_seconds_left(&link_); }
  /* Raw per-dial snapshot, so a YAML lambda can reach the fields the
   * diagnostics schema deliberately does not expose (model, caps_seq,
   * cert_state) without the schema having to grow. */
  bool dial_view(int idx, sl2_dial_view_t *out) {
    return sl2_link_dial_view(&link_, idx, out);
  }
  std::string dial_mac_str(int idx);

  /* diagnostics: — component-owned read-only entities over the bond table.
   * Rows are bond SLOTS (see the compaction note above), which is the whole
   * reason the MAC entity exists. */
  /* diagnostics: connected: — any bonded Serin Link alive. Sets
   * diagnostics_cfg_ itself: the 1 Hz walk must run for this entity even
   * when no per-slot rows are declared. */
  void set_connected_sensor(binary_sensor::BinarySensor *s) {
    connected_sensor_ = s;
    diagnostics_cfg_ = true;
  }
  void set_bonded_count_sensor(sensor::Sensor *s) { bonded_count_sensor_ = s; }
  void set_pairing_status_sensor(text_sensor::TextSensor *s) { pairing_status_sensor_ = s; }
  void set_pairing_seconds_sensor(sensor::Sensor *s) { pairing_seconds_sensor_ = s; }
  void add_dial_row(int idx, text_sensor::TextSensor *mac,
                    binary_sensor::BinarySensor *linked, sensor::Sensor *last_seen,
                    text_sensor::TextSensor *firmware) {
    if (idx < 0 || idx >= SL2_MAX_DIALS) return;
    dial_rows_[idx].mac = mac;
    dial_rows_[idx].linked = linked;
    dial_rows_[idx].last_seen = last_seen;
    dial_rows_[idx].firmware = firmware;
    dial_rows_[idx].declared = true;
    diagnostics_cfg_ = true;
  }
  /* links: screen: — its own setter rather than a fifth add_dial_row arg:
   * the entity only exists alongside the screen: switch. */
  void set_dial_screen_sensor(int idx, binary_sensor::BinarySensor *s) {
    if (idx < 0 || idx >= SL2_MAX_DIALS) return;
    dial_rows_[idx].screen = s;
    dial_rows_[idx].declared = true;
    diagnostics_cfg_ = true;
  }

  /* screen: — the presence gate switch. The component only ever READS its
   * published state (hvac_get_state), so the core's per-tick change
   * detection is the whole sync story; nullptr = both wire bits stay clear
   * and Links keep their own idle behavior. */
  void set_screen_switch(switch_::Switch *s) { screen_switch_ = s; }

  void set_night_switch(switch_::Switch *s) { night_switch_ = s; }

  /* Answer a bonded Serin Link's WIFI_REQ with this node's STA credentials
   * (`link_ota_credentials:`), so the Link's own update flow can join the
   * network. Off by default — see hvac_wifi_creds. */
  void set_link_ota_credentials(bool v) { link_ota_credentials_ = v; }

  /* HVAC iface backing (public: called from the C hook trampolines). */
  bool hvac_get_state(sl2_hvac_state_t *out);
  bool hvac_apply(uint16_t mask, const struct sl2_cmd_pkt *cmd);
  bool hvac_get_caps(struct sl2_caps_pkt *out);
  bool hvac_wifi_creds(char ssid[33], char psk[65]);
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
  binary_sensor::BinarySensor *hvac_link_sensor_{nullptr};
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
  bool link_ota_credentials_{false};
  sensor::Sensor *dial_temp_sensor_{nullptr};
  sensor::Sensor *dial_hum_sensor_{nullptr};
  text_sensor::TextSensor *dial_mac_sensor_{nullptr};
  std::vector<Trigger<float> *> room_temp_triggers_;
  uint32_t dial_stale_ms_{90000};
  int16_t dial_temp_cc_{SL2_CC_NA};
  uint16_t dial_hum_cc_{SL2_HUM_CC_NA};
  /* millis() of the last frame carrying a VALID temperature; 0 = never.
   * Freshness follows the TEMPERATURE, not the frame: a dial with sensing
   * hardware but no reading yet must not hold off the stale watchdog. */
  uint32_t dial_temp_ms_{0};
  bool dial_has_sensor_{false};
  uint8_t dial_mac_[6]{};
  uint8_t primary_dial_[6]{};
  bool has_primary_dial_{false};
  select::Select *room_source_select_{nullptr};
  int pub_room_source_idx_{-1};
  /* The ONE stored copy of the room-source choice. selected_src_ and
   * primary_dial_ above are projections of it (room_source_project_), not
   * independent state, so nothing has to keep three fields in step. */
  uint64_t selected_source_id_{SL2_ROOM_SOURCE_INTERNAL_ID};
  ESPPreferenceObject room_source_id_pref_;
  struct ext_source_t {
    std::string name;
    sensor::Sensor *sensor;
    uint64_t id;
    float last{NAN};
    uint32_t last_ms{0};   /* millis() of the last non-NaN value; 0 = never */
  };
  std::vector<ext_source_t> ext_sources_;
  /* Third projection of selected_source_id_ (with has_primary_dial_ and
   * selected_src_): index into ext_sources_, or -1. */
  int selected_ext_{-1};
  int ext_index_(uint64_t id) const;
  void ext_state_(int idx, float v);
  /* on_room_temperature: fan-out. Every feed path goes through here. */
  void fire_room_temperature_(float t);
  /* After a selection change: push the new source's last known value (or 0
   * for Heat pump) so the heat pump does not wait for the next sample. */
  void room_source_changed_();
  uint32_t room_catalog_revision_() const;
  bool room_source_slot_(uint64_t id, int *slot) const;
  bool room_catalog_entry_(int idx, struct sl2_room_source_entry *e) const;
  void room_source_apply_(uint64_t id, bool fire = true);
  void room_source_project_();
  void room_source_reconcile_();
  void refresh_room_source_select_();
  /* Read once at boot for the v3->v4 migration; never written any more. */
  ESPPreferenceObject primary_pref_;
  uint32_t last_primary_ms_{0};
  /* One log line per ignored dial, not per frame: non-primary DIAL_SENSOR
   * frames arrive at up to 3 Hz while that dial's source edit is unconfirmed. */
  uint8_t ignored_logged_[SL2_MAX_DIALS][6]{};
  uint8_t n_ignored_logged_{0};
  int16_t dial_pub_cc_{SL2_CC_NA};   /* last value published to HA */
  uint32_t dial_pub_ms_{0};          /* millis() of that publish; 0 = never */
  bool dial_stale_{false};           /* NAN already published */
  uint32_t last_dial_check_ms_{0};
  /* Per-slot link_sensor rows: the reading, who it belongs to (the slot's
   * occupant can change under a forget-compaction), and the last publish, so
   * each row honours the same frame-driven dedup as the arbitrated pair. */
  struct SensorRow {
    bool declared{false};
    sensor::Sensor *temp{nullptr};
    sensor::Sensor *hum{nullptr};
    uint8_t mac[6]{};                /* whose reading this row shows */
    bool mac_valid{false};
    int16_t temp_cc{SL2_CC_NA};
    uint16_t hum_cc{SL2_HUM_CC_NA};
    uint32_t temp_ms{0};             /* millis() of last valid temp; 0 = never */
    int16_t pub_cc{SL2_CC_NA};
    uint32_t pub_ms{0};
    bool stale_pub{false};           /* NAN already published */
  };
  void feed_sensor_row_(const uint8_t src_mac[6],
                        const struct sl2_dial_sensor_pkt *p);
  void publish_sensor_row_(SensorRow &r, bool stale);
  /* 1 Hz: per-row staleness, and retracting a reading whose slot occupant
   * changed (this is the one publish path that fires on the ABSENCE or
   * re-homing of frames, so it cannot live in feed_sensor_row_). */
  void check_sensor_rows_(uint32_t now);
  SensorRow sensor_rows_[SL2_MAX_DIALS];
  bool sensor_rows_cfg_{false};
  /* Per-slot diagnostics entities plus the last value published for each, so
   * the 1 Hz walk can honour the component's publish-on-change rule without
   * asking each entity what it last held. */
  struct DialRow {
    bool declared{false};
    text_sensor::TextSensor *mac{nullptr};
    binary_sensor::BinarySensor *linked{nullptr};
    sensor::Sensor *last_seen{nullptr};
    text_sensor::TextSensor *firmware{nullptr};
    binary_sensor::BinarySensor *screen{nullptr};
    std::string pub_mac;
    std::string pub_fw;
    bool pub_linked{false};
    bool pub_linked_valid{false};
    bool pub_screen{false};
    bool pub_screen_valid{false};
    uint32_t last_seen_pub_ms{0};
  };
  void publish_diagnostics_(uint32_t now);
  bool diagnostics_cfg_{false};
  DialRow dial_rows_[SL2_MAX_DIALS];
  switch_::Switch *screen_switch_{nullptr};
  switch_::Switch *night_switch_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  bool pub_any_live_{false};
  bool pub_any_live_valid_{false};
  sensor::Sensor *bonded_count_sensor_{nullptr};
  text_sensor::TextSensor *pairing_status_sensor_{nullptr};
  sensor::Sensor *pairing_seconds_sensor_{nullptr};
  int pub_bonded_count_{-1};
  std::string pub_pair_result_;
  int pub_pair_seconds_{-1};
  uint32_t last_diag_ms_{0};
  bool diag_primed_{false};          /* the first pass publishes everything */
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

class RoomSourceSelect : public select::Select, public Parented<SerinLinkComponent> {
 protected:
  void control(size_t index) override { this->parent_->room_source_select_control(index); }
};

/* screen: — the presence-gate switch. Optimistic: write_state only
 * publishes; the component polls the published state from hvac_get_state
 * every core tick, and the STATE fan-out's change detection carries the flip
 * to every Link inside SL2_STATE_MIN_INTERVAL_MS. Restored (schema default
 * RESTORE_DEFAULT_ON) so a reboot resumes the last gate rather than waking
 * an empty room. */
class ScreenSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

/* night: — the sun-down gate switch. Same optimistic contract as
 * ScreenSwitch above; restored OFF (schema default) so a reboot in a
 * controller HA never touches keeps daytime behavior rather than waking
 * the house to a capped dial. */
class NightSwitch : public switch_::Switch {
 protected:
  void write_state(bool state) override { this->publish_state(state); }
};

/* pair_button: — press opens the pairing window the schema baked in. */
class PairLinkButton : public button::Button, public Parented<SerinLinkComponent> {
 public:
  void set_window(uint32_t ms) { window_ms_ = ms; }

 protected:
  void press_action() override { this->parent_->pair_start(window_ms_); }
  uint32_t window_ms_{60000};
};

/* ── automation actions ───────────────────────────────────────────────────
 * Parented rather than free-standing, so a config with two serin_link
 * components can address each one. These replace the raw lambdas the example
 * configs used to need. */

template<typename... Ts>
class PairStartAction : public Action<Ts...>, public Parented<SerinLinkComponent> {
 public:
  TEMPLATABLE_VALUE(uint32_t, window)
  void play(Ts... x) override { this->parent_->pair_start(this->window_.value(x...)); }
};

template<typename... Ts>
class PairCancelAction : public Action<Ts...>, public Parented<SerinLinkComponent> {
 public:
  void play(Ts...) override { this->parent_->pair_cancel(); }
};

/* Exactly one of slot/mac is set — enforced by the config schema, so the
 * runtime simply prefers the MAC when one is present. slot: is templatable
 * (it is just an index); mac: is resolved at compile time by cv.mac_address,
 * which is what keeps runtime MAC string parsing out of the component. */
template<typename... Ts>
class ForgetDialAction : public Action<Ts...>, public Parented<SerinLinkComponent> {
 public:
  TEMPLATABLE_VALUE(int, slot)
  /* std::array, not uint8_t[6]: the codegen emits a braced initializer, which
   * a C array parameter cannot take (it decays to a pointer). */
  void set_mac(const std::array<uint8_t, 6> &mac) {
    mac_ = mac;
    has_mac_ = true;
  }
  void play(Ts... x) override {
    const bool ok = has_mac_ ? this->parent_->forget_dial_mac(mac_.data())
                             : this->parent_->forget_dial_slot(this->slot_.value(x...));
    if (!ok) ESP_LOGW("serin_link", "forget_link: no such Serin Link");
  }

 protected:
  std::array<uint8_t, 6> mac_{};
  bool has_mac_{false};
};

template<typename... Ts>
class ForgetAllDialsAction : public Action<Ts...>, public Parented<SerinLinkComponent> {
 public:
  void play(Ts...) override { this->parent_->forget_all_dials(); }
};

}  // namespace serin_link
}  // namespace esphome
