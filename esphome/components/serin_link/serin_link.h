#pragma once
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/select/select.h"
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
