#include "serin_link.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/version.h"

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <nvs.h>
#include <esp_random.h>
#include <esp_system.h>

#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "sl2_info.h"

namespace esphome {
namespace serin_link {

static const char *const TAG = "serin_link";
static SerinLinkComponent *g_self = nullptr;

/* bytes -> "AA:BB:CC:DD:EE:FF". publish_dial_() had the only copy; the
 * diagnostics rows and the dump_config bond table need the same format. */
static void sl2_fmt_mac(const uint8_t mac[6], char out[18]) {
  std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── ESP-NOW port ─────────────────────────────────────────────────────── */

static void on_espnow_recv(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len) {
  if (g_self == nullptr) return;
  sl2_rxq_push(&g_self->rxq_, info->src_addr, info->des_addr, data, len);
}

static bool p_send(void *, const uint8_t mac[6], const void *buf, size_t len) {
  return esp_now_send(mac, static_cast<const uint8_t *>(buf), len) == ESP_OK;
}

static bool p_peer_add(void *, const uint8_t mac[6], const uint8_t lmk[16],
                       bool encrypt) {
  esp_now_peer_info_t pi{};
  std::memcpy(pi.peer_addr, mac, 6);
  pi.ifidx = WIFI_IF_STA;
  pi.channel = 0;  /* follow the STA channel */
  pi.encrypt = encrypt;
  if (encrypt && lmk != nullptr) std::memcpy(pi.lmk, lmk, 16);
  esp_err_t err = esp_now_add_peer(&pi);
  if (err == ESP_ERR_ESPNOW_EXIST) err = esp_now_mod_peer(&pi);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "peer_add %02X:%02X:%02X:%02X:%02X:%02X enc=%d failed: %s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], encrypt,
             esp_err_to_name(err));
  return err == ESP_OK;
}

static void p_peer_del(void *, const uint8_t mac[6]) { esp_now_del_peer(mac); }

static bool p_own_mac(void *, uint8_t out[6]) {
  return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
}

static uint8_t p_channel(void *) {
  uint8_t ch = 0;
  wifi_second_chan_t sc;
  return esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
}

static uint32_t p_now_ms(void *) {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static const char *const NVS_NS = "serinlink";

static bool p_kv_get(void *, const char *key, void *buf, size_t *len) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  esp_err_t err = nvs_get_blob(h, key, buf, len);
  nvs_close(h);
  return err == ESP_OK;
}

static bool p_kv_set(void *, const char *key, const void *buf, size_t len) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}

static void p_log(void *, int level, const char *msg) {
  switch (level) {
    case 0: ESP_LOGE(TAG, "%s", msg); break;
    case 1: ESP_LOGW(TAG, "%s", msg); break;
    case 2: ESP_LOGI(TAG, "%s", msg); break;
    default: ESP_LOGD(TAG, "%s", msg); break;
  }
}

/* ── Monocypher crypto (Ed25519 + X25519 only; HKDF is pinned in sl2_sha256.h) ── */

/* Vendored rather than pulled from espressif/libsodium: only ONE libsodium can
 * exist in an ESPHome image, and it is never ours. `api: encryption:` pulls
 * esphome/noise-c, ESPHome converts that PlatformIO lib tree into an IDF
 * component keyed `libsodium`, and that entry is written after -- so it
 * overwrites -- anything add_idf_component() declares under the same key.
 * Declaring a different key instead collides on name-without-namespace and
 * fails component discovery outright ("Can't decide which one to pick").
 * ESPHome's port is a curated subset for noise-c: it compiles the ed25519_ref10
 * primitives but no crypto_sign_* and no SHA-512, so binding to whatever wins
 * the key link-errors on the three signing calls below.
 *
 * Monocypher (4.0.3, dual BSD-2/CC-0, see LICENCE.monocypher.md) is
 * self-contained and namespaced away from libsodium, so it coexists with
 * noise-c's copy no matter which ESPHome version resolves it. Wire formats are
 * unchanged: RFC 8032 Ed25519 and RFC 7748 X25519, and Monocypher's 64-byte
 * secret key is seed||public_key exactly as libsodium's was, so identity keys
 * already provisioned in NVS stay valid. */
static int c_rand(void *, uint8_t *buf, size_t len) {
  esp_fill_random(buf, len);
  return 0;
}

static int c_xkp(void *, uint8_t priv[32], uint8_t pub[32]) {
  esp_fill_random(priv, 32);
  crypto_x25519_public_key(pub, priv);
  return 0;
}

static int c_xsh(void *, const uint8_t priv[32], const uint8_t peer[32],
                 uint8_t out[32]) {
  crypto_x25519(out, priv, peer);
  /* libsodium's crypto_scalarmult() rejected small-order peer points by
   * returning -1; Monocypher returns void and documents the same check as the
   * caller's job. An all-zero shared secret is the tell. */
  uint8_t acc = 0;
  for (size_t i = 0; i < 32; i++)
    acc = (uint8_t) (acc | out[i]);
  return acc == 0 ? -1 : 0;
}

static int c_ekp(void *, uint8_t priv[64], uint8_t pub[32]) {
  uint8_t seed[32];
  esp_fill_random(seed, sizeof(seed));
  crypto_ed25519_key_pair(priv, pub, seed); /* wipes seed */
  return 0;
}

static int c_sign(void *, const uint8_t priv[64], const uint8_t *msg,
                  size_t msg_len, uint8_t sig[64]) {
  crypto_ed25519_sign(sig, priv, msg, msg_len);
  return 0;
}

static int c_verify(void *, const uint8_t pub[32], const uint8_t *msg,
                    size_t msg_len, const uint8_t sig[64]) {
  return crypto_ed25519_check(sig, pub, msg, msg_len); /* 0 = valid */
}

/* ── ClimateTraits/state <-> sl2 semantic model ───────────────────────── */

static uint8_t mode_to_sl2(climate::ClimateMode m) {
  switch (m) {
    case climate::CLIMATE_MODE_OFF:       return SL2_MODE_OFF;
    case climate::CLIMATE_MODE_HEAT:      return SL2_MODE_HEAT;
    case climate::CLIMATE_MODE_COOL:      return SL2_MODE_COOL;
    case climate::CLIMATE_MODE_HEAT_COOL: return SL2_MODE_HEAT_COOL;
    case climate::CLIMATE_MODE_AUTO:      return SL2_MODE_AUTO;
    case climate::CLIMATE_MODE_DRY:       return SL2_MODE_DRY;
    case climate::CLIMATE_MODE_FAN_ONLY:  return SL2_MODE_FAN_ONLY;
    default:                              return SL2_MODE_OFF;
  }
}

static climate::ClimateMode mode_from_sl2(uint8_t m) {
  switch (m) {
    case SL2_MODE_HEAT:      return climate::CLIMATE_MODE_HEAT;
    case SL2_MODE_COOL:      return climate::CLIMATE_MODE_COOL;
    case SL2_MODE_HEAT_COOL: return climate::CLIMATE_MODE_HEAT_COOL;
    case SL2_MODE_AUTO:      return climate::CLIMATE_MODE_AUTO;
    case SL2_MODE_DRY:       return climate::CLIMATE_MODE_DRY;
    case SL2_MODE_FAN_ONLY:  return climate::CLIMATE_MODE_FAN_ONLY;
    default:                 return climate::CLIMATE_MODE_OFF;
  }
}

static uint8_t action_to_sl2(climate::ClimateAction a) {
  switch (a) {
    case climate::CLIMATE_ACTION_COOLING: return SL2_ACT_COOLING;
    case climate::CLIMATE_ACTION_HEATING: return SL2_ACT_HEATING;
    case climate::CLIMATE_ACTION_DRYING:  return SL2_ACT_DRYING;
    case climate::CLIMATE_ACTION_FAN:     return SL2_ACT_FAN;
    case climate::CLIMATE_ACTION_IDLE:
    case climate::CLIMATE_ACTION_OFF:     return SL2_ACT_IDLE;
    default:                              return SL2_ACT_UNKNOWN;
  }
}

static uint8_t preset_to_sl2(climate::ClimatePreset p) {
  switch (p) {
    case climate::CLIMATE_PRESET_ECO:      return SL2_PRESET_ECO;
    case climate::CLIMATE_PRESET_AWAY:     return SL2_PRESET_AWAY;
    case climate::CLIMATE_PRESET_BOOST:    return SL2_PRESET_BOOST;
    case climate::CLIMATE_PRESET_COMFORT:  return SL2_PRESET_COMFORT;
    case climate::CLIMATE_PRESET_HOME:     return SL2_PRESET_HOME;
    case climate::CLIMATE_PRESET_SLEEP:    return SL2_PRESET_SLEEP;
    case climate::CLIMATE_PRESET_ACTIVITY: return SL2_PRESET_ACTIVITY;
    default:                               return SL2_PRESET_NONE;
  }
}

static bool preset_from_sl2(uint8_t p, climate::ClimatePreset *out) {
  switch (p) {
    case SL2_PRESET_NONE:     *out = climate::CLIMATE_PRESET_NONE; return true;
    case SL2_PRESET_ECO:      *out = climate::CLIMATE_PRESET_ECO; return true;
    case SL2_PRESET_AWAY:     *out = climate::CLIMATE_PRESET_AWAY; return true;
    case SL2_PRESET_BOOST:    *out = climate::CLIMATE_PRESET_BOOST; return true;
    case SL2_PRESET_COMFORT:  *out = climate::CLIMATE_PRESET_COMFORT; return true;
    case SL2_PRESET_HOME:     *out = climate::CLIMATE_PRESET_HOME; return true;
    case SL2_PRESET_SLEEP:    *out = climate::CLIMATE_PRESET_SLEEP; return true;
    case SL2_PRESET_ACTIVITY: *out = climate::CLIMATE_PRESET_ACTIVITY; return true;
    default: return false;
  }
}

static int16_t c_to_dc_or(float c, int16_t fallback) {
  if (std::isnan(c)) return fallback;
  return sl2_c_to_dc(c);
}

/* Canonical detent order for discrete fan modes (spec: dial sends canonical
 * detent percents; the ordering here defines what "higher" means). */
static const climate::ClimateFanMode FAN_ORDER[] = {
    climate::CLIMATE_FAN_QUIET,  climate::CLIMATE_FAN_LOW,
    climate::CLIMATE_FAN_MIDDLE, climate::CLIMATE_FAN_MEDIUM,
    climate::CLIMATE_FAN_FOCUS,  climate::CLIMATE_FAN_DIFFUSE,
    climate::CLIMATE_FAN_HIGH,
};

void SerinLinkComponent::rebuild_fan_detents_() {
  fan_detents_.clear();
  fan_has_auto_ = false;
  if (climate_ == nullptr) return;
  auto traits = climate_->get_traits();
  for (auto m : FAN_ORDER)
    if (traits.supports_fan_mode(m)) fan_detents_.push_back(m);
  fan_has_auto_ = traits.supports_fan_mode(climate::CLIMATE_FAN_AUTO) ||
                  traits.supports_fan_mode(climate::CLIMATE_FAN_ON);
}

/* ── vane axes bound to select entities ───────────────────────────────────
 * The select's option list defines the wire positions IN ORDER (1..n);
 * options named "auto"/"swing" (case-insensitive) map to the wire AUTO(0) /
 * SWING(255) codes instead of occupying a position. cn105's vertical vane
 * ("AUTO ↑↑ ↑ — ↓ ↓↓ SWING") thus declares 5 positions + auto + swing, and
 * its horizontal one ("←← ← | → →→ ←→ SWING ...") puts the split pattern
 * (←→) at position 6 — exactly the native controller's declaration. */

static bool opt_is(const std::string &o, const char *name) {
  if (o.size() != strlen(name)) return false;
  for (size_t i = 0; i < o.size(); i++)
    if (tolower((unsigned char)o[i]) != name[i]) return false;
  return true;
}

static uint8_t vane_caps_byte(select::Select *s) {
  int npos = 0; bool has_auto = false, has_swing = false;
  for (const auto &o : s->traits.get_options()) {
    if (opt_is(o, "auto")) has_auto = true;
    else if (opt_is(o, "swing")) has_swing = true;
    else npos++;
  }
  if (npos > 15) npos = 15;                     /* VANECAP nibble */
  return SL2_VANECAP(npos, has_auto, has_swing);
}

static uint8_t vane_state_code(select::Select *s) {
  const std::string cur = s->current_option().str();
  int pos = 0;
  for (const auto &o : s->traits.get_options()) {
    if (opt_is(o, "auto"))  { if (o == cur) return SL2_VANE_AUTO;  continue; }
    if (opt_is(o, "swing")) { if (o == cur) return SL2_VANE_SWING; continue; }
    pos++;
    if (o == cur) return (uint8_t)pos;
  }
  return SL2_VANE_AUTO;                          /* unknown/unset option */
}

static void vane_apply(select::Select *s, uint8_t code) {
  int pos = 0;
  for (const auto &o : s->traits.get_options()) {
    bool is_auto = opt_is(o, "auto"), is_swing = opt_is(o, "swing");
    if (!is_auto && !is_swing) pos++;
    if ((code == SL2_VANE_AUTO && is_auto) ||
        (code == SL2_VANE_SWING && is_swing) ||
        (code >= 1 && code <= 15 && !is_auto && !is_swing && pos == code)) {
      auto call = s->make_call();
      call.set_option(o);
      call.perform();
      return;
    }
  }
}

/* ── HVAC iface: bound climate entity (or the canned spike device) ────── */

bool SerinLinkComponent::hvac_get_state(sl2_hvac_state_t *out) {
  std::memset(out, 0, sizeof *out);
  out->wifi = network::is_connected();
  out->wifi_provisioned = true;   /* creds live in the YAML by definition */
  out->use_f = use_f_;
  out->set_low_dc = SL2_DC_NA;
  out->set_high_dc = SL2_DC_NA;
  out->room_hum_pct = SL2_HUM_NA;
  out->hum_set_pct = SL2_HUM_NA;

  if (battery_sensor_ != nullptr && battery_sensor_->has_state() &&
      !std::isnan(battery_sensor_->state)) {
    float b = battery_sensor_->state;
    if (b <= batt_low_threshold_) batt_low_latch_ = true;
    else if (b >= batt_low_threshold_ + 5) batt_low_latch_ = false;
  } else {
    batt_low_latch_ = false;
  }
  out->sensor_batt_low = batt_low_latch_;

  if (climate_ == nullptr) {                 /* spike: canned device */
    out->hvac_link = true;
    out->mode = SL2_MODE_HEAT;
    out->action = SL2_ACT_HEATING;
    out->fan = 40;
    out->vane_v = 3;
    uint32_t t = p_now_ms(nullptr) / 30000U;
    out->room_dc = static_cast<int16_t>(208 + (t % 5));
    out->set_dc = 220;
    return true;
  }

  auto traits = climate_->get_traits();
  /* lambda or sensor, never both (schema-enforced) — either one makes the
   * signal explicit and turns the NaN-room-temp heuristic off. */
  const bool has_link_src = static_cast<bool>(hvac_link_fn_) || hvac_link_sensor_ != nullptr;
  const bool link_val = hvac_link_fn_          ? hvac_link_fn_()
                        : hvac_link_sensor_    ? hvac_link_sensor_->state
                                               : false;
  out->hvac_link = sl2_hvac_link_infer(
      has_link_src, link_val,
      traits.has_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE),
      climate_->current_temperature);
  out->mode = mode_to_sl2(climate_->mode);
  out->action = traits.has_feature_flags(climate::CLIMATE_SUPPORTS_ACTION)
                    ? action_to_sl2(climate_->action) : SL2_ACT_UNKNOWN;
  out->room_dc = c_to_dc_or(climate_->current_temperature, 0);
  if (traits.has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE)) {
    out->set_low_dc = c_to_dc_or(climate_->target_temperature_low, SL2_DC_NA);
    out->set_high_dc = c_to_dc_or(climate_->target_temperature_high, SL2_DC_NA);
    /* single-setpoint field mirrors the low bound so pre-band UIs show something */
    out->set_dc = out->set_low_dc != SL2_DC_NA ? out->set_low_dc : 0;
  } else {
    out->set_dc = c_to_dc_or(climate_->target_temperature, 0);
  }
  if (!fan_detents_.empty() && climate_->fan_mode.has_value()) {
    auto fm = climate_->fan_mode.value();
    if (fm == climate::CLIMATE_FAN_AUTO || fm == climate::CLIMATE_FAN_ON) {
      out->fan = SL2_FAN_AUTO;
    } else {
      for (size_t i = 0; i < fan_detents_.size(); i++)
        if (fan_detents_[i] == fm) {
          out->fan = static_cast<uint8_t>(
              ((i + 1) * 100 + fan_detents_.size() / 2) / fan_detents_.size());
          break;
        }
    }
  }
  if (vane_v_sel_) out->vane_v = vane_state_code(vane_v_sel_);
  if (vane_h_sel_) out->vane_h = vane_state_code(vane_h_sel_);
  if (climate_->preset.has_value())
    out->preset = preset_to_sl2(climate_->preset.value());
  if (traits.has_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_HUMIDITY) &&
      !std::isnan(climate_->current_humidity))
    out->room_hum_pct = static_cast<uint8_t>(climate_->current_humidity);
  if (traits.has_feature_flags(climate::CLIMATE_SUPPORTS_TARGET_HUMIDITY) &&
      !std::isnan(climate_->target_humidity))
    out->hum_set_pct = static_cast<uint8_t>(climate_->target_humidity);
  apply_overlay_(out, traits.has_feature_flags(
                          climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE));
  return true;
}

float SerinLinkComponent::clamp_setpoint_(float c) {
  auto traits = climate_->get_traits();
  const float lo = traits.get_visual_min_temperature();
  const float hi = traits.get_visual_max_temperature();
  if (c < lo) return lo;
  if (c > hi) return hi;
  return c;
}

bool SerinLinkComponent::hvac_apply(uint16_t mask, const struct sl2_cmd_pkt *cmd) {
  if (mask & SL2_CM_UNITS) {
    use_f_ = cmd->use_f != 0;
    use_f_pref_.save(&use_f_);
  }
  if (climate_ == nullptr) {
    ESP_LOGI(TAG, "spike CMD: mask=0x%04x mode=%u fan=%u set=%d", mask,
             cmd->mode, cmd->fan, static_cast<int>(cmd->set_dc));
    return true;
  }
  // Band CMDs carry low/high (set_dc is NA), so log those instead of a bare
  // "set=0.0C" that reads as an empty command.
  if (mask & SL2_CM_TEMP_BAND) {
    ESP_LOGI(TAG, "CMD from Serin Link: mask=0x%04x mode=%u fan=%u band=%.1f..%.1fC", mask,
             cmd->mode, cmd->fan, sl2_dc_to_c(cmd->set_low_dc), sl2_dc_to_c(cmd->set_high_dc));
  } else {
    ESP_LOGI(TAG, "CMD from Serin Link: mask=0x%04x mode=%u fan=%u set=%.1fC", mask,
             cmd->mode, cmd->fan, sl2_dc_to_c(cmd->set_dc));
  }
  /* Stage, don't apply: fields merge into hold_ (normalized to what the
   * entity will report back once it confirms, so the overlay's per-field
   * confirmation compare can match exactly) and one ClimateCall goes out
   * after cmd_debounce_ms_ of quiet. Returning true still echoes STATE
   * immediately — apply_overlay_ makes that echo read back the staged
   * values instead of the entity's pre-command state. */
  bool any = (mask & SL2_CM_UNITS) != 0;
  if (mask & SL2_CM_MODE) {
    hold_.mode = mode_to_sl2(mode_from_sl2(cmd->mode));
    stage_(SL2_CM_MODE); any = true;
  }
  if (mask & SL2_CM_TEMP) {
    hold_.set_dc = sl2_c_to_dc(clamp_setpoint_(sl2_dc_to_c(cmd->set_dc)));
    stage_(SL2_CM_TEMP); any = true;
  }
  if (mask & SL2_CM_TEMP_BAND) {
    hold_.set_low_dc = cmd->set_low_dc != SL2_DC_NA
        ? sl2_c_to_dc(clamp_setpoint_(sl2_dc_to_c(cmd->set_low_dc))) : SL2_DC_NA;
    hold_.set_high_dc = cmd->set_high_dc != SL2_DC_NA
        ? sl2_c_to_dc(clamp_setpoint_(sl2_dc_to_c(cmd->set_high_dc))) : SL2_DC_NA;
    stage_(SL2_CM_TEMP_BAND); any = true;
  }
  if ((mask & SL2_CM_FAN) && !fan_detents_.empty()) {
    if (cmd->fan == SL2_FAN_AUTO) {
      if (fan_has_auto_) { hold_.fan = SL2_FAN_AUTO; stage_(SL2_CM_FAN); any = true; }
    } else {
      const size_t n = fan_detents_.size();
      size_t idx = (static_cast<size_t>(cmd->fan) * n + 50) / 100;
      if (idx < 1) idx = 1;
      if (idx > n) idx = n;
      hold_.fan = static_cast<uint8_t>((idx * 100 + n / 2) / n);  /* canonical pct */
      stage_(SL2_CM_FAN); any = true;
    }
  }
  if (mask & SL2_CM_PRESET) {
    climate::ClimatePreset p;
    if (preset_from_sl2(cmd->preset, &p)) {
      hold_.preset = cmd->preset;
      stage_(SL2_CM_PRESET); any = true;
    }
  }
  /* wire range 0-100; anything else (incl. the 0xFF n/a sentinel) is ignored,
   * matching the unknown-mode/preset policy */
  if ((mask & SL2_CM_HUM) && cmd->hum_set_pct <= 100) {
    hold_.hum_set_pct = cmd->hum_set_pct;
    stage_(SL2_CM_HUM); any = true;
  }
  if ((mask & SL2_CM_VANEV) && vane_v_sel_) {
    hold_.vane_v = cmd->vane_v;
    stage_(SL2_CM_VANEV); any = true;
  }
  if ((mask & SL2_CM_VANEH) && vane_h_sel_) {
    hold_.vane_h = cmd->vane_h;
    stage_(SL2_CM_VANEH); any = true;
  }
  if (pending_mask_ != 0) {
    if (cmd_debounce_ms_ == 0) {
      apply_pending_();
    } else {
      /* named timeout: each CMD in a burst re-arms it (trailing debounce) */
      this->set_timeout("sl2_cmd", cmd_debounce_ms_, [this]() { this->apply_pending_(); });
    }
  }
  return any;
}

void SerinLinkComponent::stage_(uint16_t bit) {
  pending_mask_ |= bit;
  overlay_mask_ |= bit;
  overlay_since_ms_ = millis();
}

void SerinLinkComponent::apply_pending_() {
  const uint16_t mask = pending_mask_;
  pending_mask_ = 0;
  if (climate_ == nullptr || mask == 0) return;
  auto call = climate_->make_call();
  bool any = false;
  if (mask & SL2_CM_MODE) { call.set_mode(mode_from_sl2(hold_.mode)); any = true; }
  if (mask & SL2_CM_TEMP) {
    call.set_target_temperature(sl2_dc_to_c(hold_.set_dc));
    any = true;
  }
  if (mask & SL2_CM_TEMP_BAND) {
    if (hold_.set_low_dc != SL2_DC_NA)
      call.set_target_temperature_low(sl2_dc_to_c(hold_.set_low_dc));
    if (hold_.set_high_dc != SL2_DC_NA)
      call.set_target_temperature_high(sl2_dc_to_c(hold_.set_high_dc));
    any = true;
  }
  if ((mask & SL2_CM_FAN) && !fan_detents_.empty()) {
    if (hold_.fan == SL2_FAN_AUTO) {
      call.set_fan_mode(climate::CLIMATE_FAN_AUTO);
    } else {
      size_t idx = (static_cast<size_t>(hold_.fan) * fan_detents_.size() + 50) / 100;
      if (idx < 1) idx = 1;
      if (idx > fan_detents_.size()) idx = fan_detents_.size();
      call.set_fan_mode(fan_detents_[idx - 1]);
    }
    any = true;
  }
  if (mask & SL2_CM_PRESET) {
    climate::ClimatePreset p;
    if (preset_from_sl2(hold_.preset, &p)) { call.set_preset(p); any = true; }
  }
  if (mask & SL2_CM_HUM) {
    call.set_target_humidity(static_cast<float>(hold_.hum_set_pct));
    any = true;
  }
  if (any) call.perform();
  /* Vanes live outside the ClimateCall: they route to the bound selects. */
  if ((mask & SL2_CM_VANEV) && vane_v_sel_) vane_apply(vane_v_sel_, hold_.vane_v);
  if ((mask & SL2_CM_VANEH) && vane_h_sel_) vane_apply(vane_h_sel_, hold_.vane_h);
}

/* Staged values mask the entity's published state until the entity reports
 * them back (per-field) or the safety window expires — the cn105-homekit
 * "wanted settings" pattern. Timeout means the entity rejected or dropped
 * the command; the next STATE then carries the truth and the dial reverts,
 * which is the correct user-visible outcome. */
void SerinLinkComponent::apply_overlay_(sl2_hvac_state_t *out, bool two_point) {
  if (overlay_mask_ == 0) return;
  static const uint32_t ECHO_HOLD_MS = 10000;
  if (millis() - overlay_since_ms_ > ECHO_HOLD_MS) {
    overlay_mask_ = 0;
    return;
  }
  if (overlay_mask_ & SL2_CM_MODE) {
    if (out->mode == hold_.mode) overlay_mask_ &= ~SL2_CM_MODE;
    else out->mode = hold_.mode;
  }
  if (overlay_mask_ & SL2_CM_TEMP) {
    if (out->set_dc == hold_.set_dc) overlay_mask_ &= ~SL2_CM_TEMP;
    else out->set_dc = hold_.set_dc;
  }
  if (overlay_mask_ & SL2_CM_TEMP_BAND) {
    bool confirmed = true;
    if (hold_.set_low_dc != SL2_DC_NA && out->set_low_dc != hold_.set_low_dc) {
      out->set_low_dc = hold_.set_low_dc;
      confirmed = false;
    }
    if (hold_.set_high_dc != SL2_DC_NA && out->set_high_dc != hold_.set_high_dc) {
      out->set_high_dc = hold_.set_high_dc;
      confirmed = false;
    }
    if (confirmed) overlay_mask_ &= ~SL2_CM_TEMP_BAND;
    /* keep the single-setpoint mirror consistent with the overlaid band */
    if (two_point) out->set_dc = out->set_low_dc != SL2_DC_NA ? out->set_low_dc : 0;
  }
  if (overlay_mask_ & SL2_CM_FAN) {
    if (out->fan == hold_.fan) overlay_mask_ &= ~SL2_CM_FAN;
    else out->fan = hold_.fan;
  }
  if (overlay_mask_ & SL2_CM_VANEV) {
    if (out->vane_v == hold_.vane_v) overlay_mask_ &= ~SL2_CM_VANEV;
    else out->vane_v = hold_.vane_v;
  }
  if (overlay_mask_ & SL2_CM_VANEH) {
    if (out->vane_h == hold_.vane_h) overlay_mask_ &= ~SL2_CM_VANEH;
    else out->vane_h = hold_.vane_h;
  }
  if (overlay_mask_ & SL2_CM_PRESET) {
    if (out->preset == hold_.preset) overlay_mask_ &= ~SL2_CM_PRESET;
    else out->preset = hold_.preset;
  }
  if (overlay_mask_ & SL2_CM_HUM) {
    if (out->hum_set_pct == hold_.hum_set_pct) overlay_mask_ &= ~SL2_CM_HUM;
    else out->hum_set_pct = hold_.hum_set_pct;
  }
}

bool SerinLinkComponent::hvac_get_caps(struct sl2_caps_pkt *out) {
  /* Telemetry capability bits: WIFI_INFO/FW_INFO are always served (SYS has
   * no bit — it's an always-on TLV); the rest follow the YAML bindings.
   * Feature bit = capability; TLV presence = current validity (spec §9). */
  out->features = SL2_FEAT_WIFI_INFO | SL2_FEAT_FW_INFO;
  if (outside_temp_sensor_ != nullptr) out->features |= SL2_FEAT_OUTSIDE_T;
  if (compressor_hz_sensor_ != nullptr || stage_sensor_ != nullptr ||
      sub_mode_sensor_ != nullptr || auto_sub_mode_sensor_ != nullptr)
    out->features |= SL2_FEAT_COMPRESSOR;
  if (battery_sensor_ != nullptr) out->features |= SL2_FEAT_SENSOR_BATT;
  if (link_sensor_cfg_) out->features |= SL2_FEAT_LINK_SENSOR;
  if (runtime_sensor_ != nullptr) out->features |= SL2_FEAT_RUNTIME;
  if (power_sensor_ != nullptr || energy_sensor_ != nullptr)
    out->features |= SL2_FEAT_ENERGY;
  if (climate_ == nullptr) {                 /* spike: canned CAPS */
    out->modes = (1u << SL2_MODE_OFF) | (1u << SL2_MODE_HEAT) |
                 (1u << SL2_MODE_COOL) | (1u << SL2_MODE_DRY) |
                 (1u << SL2_MODE_FAN_ONLY) | (1u << SL2_MODE_AUTO);
    out->fan_steps = 5;
    out->fan_flags = SL2_FAN_HAS_AUTO;
    out->vane_v = SL2_VANECAP(5, true, true);
    out->set_min_dc = 160;
    out->set_max_dc = 305;
    out->set_step_dc = 5;
    out->ftab_id = 1;
    copy_zone_name(out->name, sizeof out->name);
    return true;
  }

  auto traits = climate_->get_traits();
  rebuild_fan_detents_();
  static const climate::ClimateMode ALL_MODES[] = {
      climate::CLIMATE_MODE_HEAT_COOL, climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_DRY,       climate::CLIMATE_MODE_AUTO,
  };
  static const climate::ClimatePreset ALL_PRESETS[] = {
      climate::CLIMATE_PRESET_HOME,  climate::CLIMATE_PRESET_AWAY,
      climate::CLIMATE_PRESET_BOOST, climate::CLIMATE_PRESET_COMFORT,
      climate::CLIMATE_PRESET_ECO,   climate::CLIMATE_PRESET_SLEEP,
      climate::CLIMATE_PRESET_ACTIVITY,
  };
  out->modes = 1u << SL2_MODE_OFF;
  for (auto m : ALL_MODES)
    if (traits.supports_mode(m)) out->modes |= 1u << mode_to_sl2(m);
  for (auto p : ALL_PRESETS)
    if (traits.supports_preset(p)) out->presets |= 1u << preset_to_sl2(p);
  out->fan_steps = static_cast<uint8_t>(fan_detents_.size());
  out->fan_flags = fan_has_auto_ ? SL2_FAN_HAS_AUTO : 0;
  out->vane_v = vane_v_sel_ ? vane_caps_byte(vane_v_sel_) : 0;
  out->vane_h = vane_h_sel_ ? vane_caps_byte(vane_h_sel_) : 0;
  out->set_min_dc = sl2_c_to_dc(traits.get_visual_min_temperature());
  out->set_max_dc = sl2_c_to_dc(traits.get_visual_max_temperature());
  float step = traits.get_visual_target_temperature_step();
  int step_dc = static_cast<int>(std::lround(step * 10.0f));
  out->set_step_dc = static_cast<uint8_t>(step_dc < 5 ? 5 : step_dc);
  out->ftab_id = 0;                          /* linear °F display */
  if (traits.has_feature_flags(climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE))
    out->band_min_gap_dc = 5;
  if (traits.has_feature_flags(climate::CLIMATE_SUPPORTS_TARGET_HUMIDITY)) {
    out->caps_flags |= SL2_CF_HUM_CTRL;
    out->hum_step_pct = 1;
  }
  copy_zone_name(out->name, sizeof out->name);
  return true;
}

/* NaN/negative -> 0; ceiling-clamped round for sensor floats. */
static uint8_t f_to_u8(float f, uint8_t max) {
  if (std::isnan(f) || f <= 0.0f) return 0;
  if (f >= max) return max;
  return static_cast<uint8_t>(std::lround(f));
}

static uint32_t f_to_u32(float f, uint32_t max) {
  if (std::isnan(f) || f <= 0.0f) return 0;
  if (f >= static_cast<float>(max)) return max;
  return static_cast<uint32_t>(std::llround(static_cast<double>(f)));
}

size_t SerinLinkComponent::fill_info_tlvs(uint8_t *buf, size_t cap) {
  size_t off = 0;
  /* Same TLV order as the reference adopter (mitsubishi-cn105-homekit);
   * HOMEKIT (0x02) has no ESPHome analogue and is never emitted. */
  if (network::is_connected() && wifi::global_wifi_component != nullptr) {
    char ip_buf[network::IP_ADDRESS_BUFFER_SIZE] = {};
    for (const auto &addr : wifi::global_wifi_component->wifi_sta_ip_addresses())
      if (addr.is_set()) { addr.str_to(ip_buf); break; }
    char ssid_buf[wifi::SSID_BUFFER_SIZE];
    sl2_info_put_wifi(buf, cap, &off,
                      static_cast<int8_t>(wifi::global_wifi_component->wifi_rssi()),
                      p_channel(nullptr),
                      wifi::global_wifi_component->wifi_ssid_to(ssid_buf),
                      ip_buf);
  }
  if (outside_temp_sensor_ != nullptr && outside_temp_sensor_->has_state() &&
      !std::isnan(outside_temp_sensor_->state))
    sl2_info_put_outside_t(buf, cap, &off,
                           sl2_c_to_dc(outside_temp_sensor_->state));
  if (compressor_hz_sensor_ != nullptr || stage_sensor_ != nullptr ||
      sub_mode_sensor_ != nullptr || auto_sub_mode_sensor_ != nullptr) {
    uint8_t hz = 0;
    if (compressor_hz_sensor_ != nullptr && compressor_hz_sensor_->has_state())
      hz = f_to_u8(compressor_hz_sensor_->state, 255);
    uint8_t stage = 0, sub = 0, autosub = 0;
    if (stage_sensor_ != nullptr && stage_sensor_->has_state())
      stage = sl2_info_stage_code(stage_sensor_->state.c_str());
    if (sub_mode_sensor_ != nullptr && sub_mode_sensor_->has_state())
      sub = sl2_info_sub_mode_code(sub_mode_sensor_->state.c_str());
    if (auto_sub_mode_sensor_ != nullptr && auto_sub_mode_sensor_->has_state())
      autosub = sl2_info_auto_sub_code(auto_sub_mode_sensor_->state.c_str());
    sl2_info_put_compressor(buf, cap, &off, hz, stage, sub, autosub);
  }
  if (battery_sensor_ != nullptr && battery_sensor_->has_state() &&
      !std::isnan(battery_sensor_->state))
    sl2_info_put_batt(buf, cap, &off, f_to_u8(battery_sensor_->state, 100));
  char build_buf[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_buf);
  sl2_info_put_fw(buf, cap, &off, ESPHOME_VERSION, build_buf);
  if (runtime_sensor_ != nullptr && runtime_sensor_->has_state() &&
      !std::isnan(runtime_sensor_->state))
    sl2_info_put_runtime(buf, cap, &off,
                         f_to_u32(runtime_sensor_->state, UINT32_MAX - 1));
  sl2_info_put_sys(buf, cap, &off,
                   static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL),
                   static_cast<uint8_t>(esp_reset_reason()));
  if (power_sensor_ != nullptr || energy_sensor_ != nullptr) {
    uint16_t w = SL2_INFO_W_NA;
    if (power_sensor_ != nullptr && power_sensor_->has_state() &&
        !std::isnan(power_sensor_->state))
      w = static_cast<uint16_t>(f_to_u32(power_sensor_->state, 0xFFFE));
    uint32_t wh = SL2_INFO_WH_NA;
    if (energy_sensor_ != nullptr && energy_sensor_->has_state() &&
        !std::isnan(energy_sensor_->state))
      wh = f_to_u32(energy_sensor_->state * 1000.0f, 0xFFFFFFFE);  /* kWh -> Wh */
    sl2_info_put_energy(buf, cap, &off, w, wh);
  }
  /* Feature bit = capability, TLV presence = current validity (spec §9): a
   * node that did not opt in emits neither. */
  if (link_sensor_cfg_)
    sl2_info_put_room_src(buf, cap, &off, selected_src_, room_src_status_());
  return off;
}

/* The three source values a dial may legally request over the wire — shared
 * between the edit path (room_sensor_feed) and the NVS load (setup()) so an
 * out-of-range byte can't reach the wire from either origin. BLE is included
 * even though this component has no BLE room source to honor: accepting it
 * into selected_src_ resolves to UNAVAILABLE rather than being rejected (see
 * the design doc §3 and the comment in room_sensor_feed). */
static bool is_room_src_valid(uint8_t v) {
  return v == SL2_ROOMSRC_INTERNAL || v == SL2_ROOMSRC_BLE || v == SL2_ROOMSRC_LINK;
}

uint8_t SerinLinkComponent::room_src_status_() const {
  switch (selected_src_) {
    case SL2_ROOMSRC_LINK:
      /* No sensing hardware is a permanent no, not a pending timeout: report
       * it immediately rather than making the user wait out stale_after for
       * an answer that can never change. This is what SL2_DSF_HAS_SENSOR is
       * for, and the same use the reference controller puts it to. */
      if (!dial_has_sensor_ || dial_temp_ms_ == 0) return SL2_ROOMST_UNAVAILABLE;
      return (millis() - dial_temp_ms_ >= dial_stale_ms_) ? SL2_ROOMST_STALE
                                                          : SL2_ROOMST_OK;
    case SL2_ROOMSRC_BLE:
      /* Selectable on the dial (SL2_FEAT_SENSOR_BATT) but this component has
       * no BLE room source to feed from: selected, never had a reading. */
      return SL2_ROOMST_UNAVAILABLE;
    default:
      /* Internal — the heat pump's own thermistor has no failure mode. */
      return SL2_ROOMST_OK;
  }
}

void SerinLinkComponent::room_sensor_feed(const uint8_t src_mac[6],
                                          const struct sl2_dial_sensor_pkt *p,
                                          bool is_edit) {
  /* The trampoline is installed unconditionally (see setup()) — without
   * SL2_FEAT_LINK_SENSOR in CAPS a conforming dial never sends DIAL_SENSOR at
   * all, so this is unreachable today. It's here so a disabled feature can't
   * be made to write NVS by a nonconforming dial. */
  if (!link_sensor_cfg_) return;

  /* The core did the length check (a short frame is reading-only, decided by
   * LENGTH never by value — the zero-filled want_src of a truncated frame
   * means Internal and would silently switch the source). Trust is_edit; the
   * VALUE still needs validating, it crossed the air.
   *
   * Any of the three known sources is accepted, including BLE, which this
   * component can never honor — the dial offers it whenever the controller
   * sets SL2_FEAT_SENSOR_BATT, i.e. merely by binding battery_sensor:.
   * Rejecting it (what the reference controller does) would be worse than
   * useless here: the dial re-sends until INFO echoes the source it asked
   * for, with no timeout, so a rejected edit repeats at ~3 Hz forever.
   * Accepting it and reporting UNAVAILABLE is exactly what that status code
   * means, ends the retry, and tells the user the truth on the dial's face.
   * See the design doc §3. */
  if (is_edit && is_room_src_valid(p->want_src) && selected_src_ != p->want_src) {
    selected_src_ = p->want_src;
    room_src_pref_.save(&selected_src_);
    ESP_LOGI(TAG, "room source -> %u (set from Serin Link)",
             static_cast<unsigned>(selected_src_));
  }

  /* link_sensor: links: — per-slot rows see EVERY Link's frame, so this runs
   * BEFORE the primary arbitration below: arbitration decides which Link
   * feeds the arbitrated pair and the heat pump, never which readings may be
   * seen. */
  if (sensor_rows_cfg_ && src_mac != nullptr) feed_sensor_row_(src_mac, p);

  /* primary_link: (YAML) — a non-primary Link's READING is ignored; its EDIT is not.
   * Deliberately placed AFTER the is_edit branch above: refusing the edit
   * would spin that dial at ~3 Hz forever (§10d has no give-up rule), which
   * is the same reason BLE is accepted-then-reported-UNAVAILABLE rather than
   * rejected. Everything downstream is then correct by construction, because
   * dial_temp_ms_ only ever advances from the primary — so room_src_status_()
   * reports STALE/UNAVAILABLE when the primary dies even while another dial
   * is chattering happily. */
  if (has_primary_dial_ && src_mac != nullptr &&
      std::memcmp(src_mac, primary_dial_, 6) != 0) {
    bool logged = false;
    for (uint8_t i = 0; i < n_ignored_logged_; i++)
      if (std::memcmp(ignored_logged_[i], src_mac, 6) == 0) { logged = true; break; }
    if (!logged && n_ignored_logged_ < SL2_MAX_DIALS) {
      std::memcpy(ignored_logged_[n_ignored_logged_++], src_mac, 6);
      char got[18], want[18];
      sl2_fmt_mac(src_mac, got);
      sl2_fmt_mac(primary_dial_, want);
      ESP_LOGI(TAG, "ignoring room sensor from %s: primary_link is %s", got, want);
    }
    return;
  }

  /* The tracked dial may clear its own has-sensor flag without sending a
   * reading — a permanent "no", which resolves to UNAVAILABLE rather than
   * waiting out stale_after. Only the dial already on display may do this:
   * another dial's edit-only frame must not speak for this one. */
  if (src_mac != nullptr && memcmp(dial_mac_, src_mac, 6) == 0)
    dial_has_sensor_ = (p->flags & SL2_DSF_HAS_SENSOR) != 0;
  const bool got_temp = p->temp_dc != SL2_DC_NA;
  if (!got_temp) return;

  /* dial_mac_ and (below) dial_hum_pct_ describe "the dial whose reading is
   * on display" — adopt them only from a frame that actually carries one,
   * so an edit-only frame (no reading: a sensorless dial, or a source edit
   * before its ack) can't silently re-label the entity or leak another
   * dial's humidity in. dial_has_sensor_ is handled above (the tracked
   * dial may update it from an edit-only frame too) but still needs the
   * unconditional assignment below to cover a newly-adopted dial, since the
   * MAC-scoped check above ran before dial_mac_ picks up this frame's
   * source. Humidity additionally needs a same-source check of its own:
   * temp_dc and hum_pct have independent NA sentinels, so one dial's
   * frame may carry a value the other's doesn't — if the reporting dial
   * just changed, drop the outgoing dial's humidity before this frame's
   * fields apply, so it can never be republished under the new dial's MAC. */
  if (src_mac != nullptr && memcmp(dial_mac_, src_mac, 6) != 0) {
    dial_hum_pct_ = SL2_HUM_NA;
    memcpy(dial_mac_, src_mac, 6);
  }
  dial_has_sensor_ = (p->flags & SL2_DSF_HAS_SENSOR) != 0;
  if (p->hum_pct != SL2_HUM_NA) dial_hum_pct_ = p->hum_pct;
  dial_temp_dc_ = p->temp_dc;
  dial_temp_ms_ = millis();

  /* Publishing is frame-driven, never timed (the stale path in loop() is the
   * one exception, because it fires on the ABSENCE of frames). Dedup matters:
   * in steady state the dial sends every 20 s or on a 0.5 C change, but while
   * a source edit is unconfirmed it re-sends every link-task pass — ~3 Hz —
   * until INFO echoes the source it asked for. Publishing per frame would
   * spray HA with duplicates for the whole confirmation window. */
  const bool changed = dial_temp_dc_ != dial_pub_dc_;
  const bool due = dial_pub_ms_ == 0 || millis() - dial_pub_ms_ >= 30000;
  if (changed || due || dial_stale_) publish_dial_(false);
}

void SerinLinkComponent::publish_dial_(bool stale) {
  dial_stale_ = stale;
  if (stale) {
    if (dial_temp_sensor_ != nullptr) dial_temp_sensor_->publish_state(NAN);
    if (dial_hum_sensor_ != nullptr) dial_hum_sensor_->publish_state(NAN);
    return;
  }
  dial_pub_dc_ = dial_temp_dc_;
  dial_pub_ms_ = millis();
  if (dial_temp_sensor_ != nullptr)
    dial_temp_sensor_->publish_state(dial_temp_dc_ / 10.0f);
  /* Humidity rides the temperature's publish decision — one gate, both
   * entities — so the two never drift apart in HA's history. */
  if (dial_hum_sensor_ != nullptr && dial_hum_pct_ != SL2_HUM_NA)
    dial_hum_sensor_->publish_state(dial_hum_pct_);
  if (dial_mac_sensor_ != nullptr) {
    char mac[18];
    sl2_fmt_mac(dial_mac_, mac);
    if (dial_mac_sensor_->state != mac) dial_mac_sensor_->publish_state(mac);
  }
  /* on_room_temperature: — same gate as the HA publish (frame-driven,
   * deduped), plus the selected-source guard: cycling the Serin Link's room
   * source back to Internal must stop the feed, with no YAML condition. */
  if (room_src_is_link()) {
    const float t = dial_temp_dc_ / 10.0f;
    for (auto *trig : room_temp_triggers_) trig->trigger(t);
  }
}

void SerinLinkComponent::feed_sensor_row_(const uint8_t src_mac[6],
                                          const struct sl2_dial_sensor_pkt *p) {
  /* The row is the sender's bond SLOT — resolved per frame, because a
   * forget-compaction can move a Link to another slot between frames. */
  int slot = -1;
  uint8_t mac[6];
  for (int i = 0; i < SL2_MAX_DIALS; i++) {
    if (sl2_link_dial_mac(&link_, i, mac) && std::memcmp(mac, src_mac, 6) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0 || !sensor_rows_[slot].declared) return;
  SensorRow &r = sensor_rows_[slot];
  if (!r.mac_valid || std::memcmp(r.mac, src_mac, 6) != 0) {
    /* New occupant (fresh bond, or a compaction the 1 Hz walk hasn't caught
     * yet): drop the previous occupant's humidity before this frame's fields
     * apply (independent NA sentinels — same leak the arbitrated pair
     * guards), and defeat the dedup gate so the new Link's first reading
     * always publishes. */
    std::memcpy(r.mac, src_mac, 6);
    r.mac_valid = true;
    r.hum_pct = SL2_HUM_NA;
    r.pub_ms = 0;
    r.pub_dc = SL2_DC_NA;
  }
  if (p->temp_dc == SL2_DC_NA) return;  /* edit-only frame: nothing to show */
  if (p->hum_pct != SL2_HUM_NA) r.hum_pct = p->hum_pct;
  r.temp_dc = p->temp_dc;
  r.temp_ms = millis();
  /* Same frame-driven dedup as the arbitrated pair: on change, on the 30 s
   * heartbeat, or to recover from a published NAN. */
  const bool changed = r.temp_dc != r.pub_dc;
  const bool due = r.pub_ms == 0 || millis() - r.pub_ms >= 30000;
  if (changed || due || r.stale_pub) publish_sensor_row_(r, false);
}

void SerinLinkComponent::publish_sensor_row_(SensorRow &r, bool stale) {
  r.stale_pub = stale;
  if (stale) {
    if (r.temp != nullptr) r.temp->publish_state(NAN);
    if (r.hum != nullptr) r.hum->publish_state(NAN);
    return;
  }
  r.pub_dc = r.temp_dc;
  r.pub_ms = millis();
  if (r.temp != nullptr) r.temp->publish_state(r.temp_dc / 10.0f);
  /* Humidity rides the temperature's publish decision — one gate, both
   * entities — so the two never drift apart in HA's history. */
  if (r.hum != nullptr && r.hum_pct != SL2_HUM_NA) r.hum->publish_state(r.hum_pct);
}

void SerinLinkComponent::check_sensor_rows_(uint32_t now) {
  for (int i = 0; i < SL2_MAX_DIALS; i++) {
    SensorRow &r = sensor_rows_[i];
    if (!r.declared) continue;
    uint8_t mac[6];
    const bool occupied = sl2_link_dial_mac(&link_, i, mac);
    if (r.mac_valid && (!occupied || std::memcmp(mac, r.mac, 6) != 0)) {
      /* The slot's occupant changed (a forget compacted the table) or left:
       * the reading on display belongs to the OLD occupant. Retract it; the
       * new occupant's next frame repopulates through feed_sensor_row_. */
      const bool had_reading = r.temp_ms != 0;
      r.mac_valid = false;
      r.temp_dc = SL2_DC_NA;
      r.hum_pct = SL2_HUM_NA;
      r.temp_ms = 0;
      if (had_reading && !r.stale_pub) publish_sensor_row_(r, true);
      continue;
    }
    if (!r.stale_pub && r.temp_ms != 0 && now - r.temp_ms >= dial_stale_ms_) {
      ESP_LOGW(TAG, "Serin Link %d room sensor stale (%" PRIu32
               " ms) — publishing unknown", i + 1, now - r.temp_ms);
      publish_sensor_row_(r, true);
    }
  }
}

/* trampolines: sl2 C hooks -> the component */
static bool t_get_state(void *ctx, sl2_hvac_state_t *out) {
  return static_cast<SerinLinkComponent *>(ctx)->hvac_get_state(out);
}
static bool t_apply(void *ctx, uint16_t mask, const struct sl2_cmd_pkt *cmd) {
  return static_cast<SerinLinkComponent *>(ctx)->hvac_apply(mask, cmd);
}
static bool t_get_caps(void *ctx, struct sl2_caps_pkt *out) {
  return static_cast<SerinLinkComponent *>(ctx)->hvac_get_caps(out);
}
static size_t t_tlvs(void *ctx, uint8_t *buf, size_t cap) {
  return static_cast<SerinLinkComponent *>(ctx)->fill_info_tlvs(buf, cap);
}
static void t_room_sensor(void *ctx, const uint8_t src_mac[6],
                          const struct sl2_dial_sensor_pkt *p, bool is_edit) {
  static_cast<SerinLinkComponent *>(ctx)->room_sensor_feed(src_mac, p, is_edit);
}

/* ── component ────────────────────────────────────────────────────────── */

/* Publish the dropdown only when it actually changes. select::publish_state
 * has no dedup of its own (it fires the state callback and notifies the API
 * unconditionally), and this is called at 1 Hz. */
void SerinLinkComponent::publish_primary_(size_t index) {
  if (primary_select_ == nullptr) return;
  if (pub_primary_idx_ == static_cast<int>(index)) return;
  pub_primary_idx_ = static_cast<int>(index);
  primary_select_->publish_state(index);
}

/* Which bond slot currently holds the pinned MAC. False when nothing is
 * pinned, or when the pinned Serin Link is no longer in the bond table. */
bool SerinLinkComponent::primary_slot_(int *out_idx) const {
  if (!has_primary_dial_) return false;
  int n = sl2_link_dial_count(const_cast<sl2_link_t *>(&link_));
  for (int i = 0; i < n; i++) {
    uint8_t mac[6];
    if (!sl2_link_dial_mac(const_cast<sl2_link_t *>(&link_), i, mac)) continue;
    if (std::memcmp(mac, primary_dial_, 6) == 0) {
      if (out_idx != nullptr) *out_idx = i;
      return true;
    }
  }
  return false;
}

void SerinLinkComponent::primary_select_control(size_t index) {
  if (index == 0) {                       /* Auto — clear the pin */
    has_primary_dial_ = false;
    std::memset(primary_dial_, 0, sizeof primary_dial_);
    n_ignored_logged_ = 0;                /* let the log speak again if re-pinned */
    uint8_t blob[7] = {0};
    primary_pref_.save(&blob);
    ESP_LOGI(TAG, "primary Serin Link: auto (last reporting wins)");
    publish_primary_(0);
    return;
  }

  const int slot = static_cast<int>(index) - 1;
  uint8_t mac[6];
  if (!sl2_link_dial_mac(&link_, slot, mac)) {
    /* An empty slot cannot be satisfied. Re-publish what is actually in force
     * rather than leaving HA showing a selection the controller never took. */
    ESP_LOGW(TAG, "primary Serin Link: slot %d is empty — selection ignored", slot + 1);
    refresh_primary_select_();
    return;
  }
  std::memcpy(primary_dial_, mac, 6);
  has_primary_dial_ = true;
  n_ignored_logged_ = 0;
  /* Persist the MAC, never the slot: forgetting a Serin Link COMPACTS the bond
   * table, so a stored slot would quietly re-point the pin at a different
   * room. */
  uint8_t blob[7];
  blob[0] = 1;
  std::memcpy(blob + 1, mac, 6);
  primary_pref_.save(&blob);
  char s[18];
  sl2_fmt_mac(mac, s);
  ESP_LOGI(TAG, "primary Serin Link: %s (slot %d)", s, slot + 1);
  refresh_primary_select_();
}

void SerinLinkComponent::refresh_primary_select_() {
  if (primary_select_ == nullptr) return;
  int slot = 0;
  if (!has_primary_dial_) {
    publish_primary_(0);
    return;
  }
  if (primary_slot_(&slot)) {
    publish_primary_(static_cast<size_t>(slot + 1));
    return;
  }
  /* Pinned Serin Link is gone from the bond table (forgotten, not merely
   * offline): drop the pin rather than strand the room source at unavailable
   * with no way back except a reflash. */
  char s[18];
  sl2_fmt_mac(primary_dial_, s);
  ESP_LOGW(TAG, "primary Serin Link %s is no longer bonded — reverting to auto", s);
  has_primary_dial_ = false;
  std::memset(primary_dial_, 0, sizeof primary_dial_);
  uint8_t blob[7] = {0};
  primary_pref_.save(&blob);
  publish_primary_(0);
}

std::string SerinLinkComponent::dial_mac_str(int idx) {
  uint8_t mac[6];
  if (!sl2_link_dial_mac(&link_, idx, mac)) return "";
  char buf[18];
  sl2_fmt_mac(mac, buf);
  return std::string(buf);
}

void SerinLinkComponent::copy_zone_name(char *dst, size_t cap) const {
  const char *n = zone_name_.c_str();
  if (zone_name_.empty()) {
    if (climate_ != nullptr && !climate_->get_name().empty())
      n = climate_->get_name().c_str();
    else
      n = App.get_friendly_name().c_str();
  }
  std::snprintf(dst, cap, "%s", n);
}

void SerinLinkComponent::setup() {
  /* No crypto init step: Monocypher has no global state, and entropy comes
   * straight from esp_fill_random (the hardware RNG, seeded by Wi-Fi/BT). */
  sl2_rxq_init(&rxq_);
  g_self = this;

  use_f_pref_ = global_preferences->make_preference<bool>(0x53324C55 /* 'S2LU' */);
  use_f_pref_.load(&use_f_);

  /* Stored primary pin: [0]=set flag, [1..6]=MAC. Only consulted when a
   * primary_select: entity exists — with the static primary_link: key the YAML
   * is the single source of truth and the two are mutually exclusive anyway. */
  primary_pref_ = global_preferences->make_preference<uint8_t[7]>(0x5332504C /* 'S2PL' */);
  if (primary_select_ != nullptr) {
    uint8_t blob[7] = {0};
    if (primary_pref_.load(&blob) && blob[0] == 1) {
      std::memcpy(primary_dial_, blob + 1, 6);
      has_primary_dial_ = true;
    }
  }

  room_src_pref_ = global_preferences->make_preference<uint8_t>(0x53325253 /* 'S2RS' */);
  if (!room_src_pref_.load(&selected_src_) || !is_room_src_valid(selected_src_))
    selected_src_ = SL2_ROOMSRC_INTERNAL;

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_now_init: %d", static_cast<int>(err));
    this->mark_failed();
    return;
  }
  /* ESP-NOW RX REQUIRES the radio awake: with modem sleep the STA radio is
   * off between AP DTIMs, so unicast from a non-AP peer is never even ACKed
   * (dial saw sent=120 acked=0). Enforce PS off here regardless of the wifi
   * component's setting. */
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  if (ps != WIFI_PS_NONE) {
    ESP_LOGW(TAG, "wifi power save was %d — forcing NONE for ESP-NOW RX",
             static_cast<int>(ps));
    esp_wifi_set_ps(WIFI_PS_NONE);
  }
  esp_now_set_pmk(reinterpret_cast<const uint8_t *>(SL2_ESPNOW_PMK));
  esp_now_register_recv_cb(on_espnow_recv);

  port_ = sl2_port_t{};
  port_.ctx = this;
  port_.send = p_send;
  port_.peer_add = p_peer_add;
  port_.peer_del = p_peer_del;
  port_.own_mac = p_own_mac;
  port_.get_channel = p_channel;
  port_.now_ms = p_now_ms;
  port_.kv_get = p_kv_get;
  port_.kv_set = p_kv_set;
  port_.log = p_log;

  crypto_ = sl2_crypto_t{};
  crypto_.ctx = this;
  crypto_.rand_bytes = c_rand;
  crypto_.x25519_keypair = c_xkp;
  crypto_.x25519_shared = c_xsh;
  crypto_.ed25519_keypair = c_ekp;
  crypto_.ed25519_sign = c_sign;
  crypto_.ed25519_verify = c_verify;

  hvac_ = sl2_hvac_iface_t{};
  hvac_.ctx = this;
  hvac_.get_state = t_get_state;
  hvac_.apply = t_apply;
  hvac_.get_caps = t_get_caps;
  hvac_.fill_info_tlvs = t_tlvs;
  /* Installed unconditionally: without SL2_FEAT_LINK_SENSOR in CAPS the dial
   * never sends DIAL_SENSOR at all, so an unconfigured node simply never
   * calls this — no need for a second gate here. */
  hvac_.room_sensor = t_room_sensor;
  hvac_.wifi_creds = nullptr;  /* Link-OTA creds relay: future work */

  sl2_link_init(&link_, &port_, &crypto_, &hvac_);
  started_ = sl2_link_start(&link_);
  if (!started_) {
    ESP_LOGE(TAG, "sl2_link_start failed");
    this->mark_failed();
    return;
  }

  if (climate_ != nullptr) rebuild_fan_detents_();

  /* Bonded dials cache CAPS by caps_seq, which the core persists — but the
   * CONTENT can change under a stable seq (new firmware/YAML declaring vane
   * axes, different entity traits). Fingerprint the current content and bump
   * the seq once whenever it differs from the last announced one, so every
   * dial re-pulls without needing a re-pair or dial reboot. */
  struct sl2_caps_pkt cp;
  memset(&cp, 0, sizeof cp);
  hvac_get_caps(&cp);
  uint32_t fp = 2166136261u;                     /* FNV-1a over the content */
  for (size_t i = 0; i < sizeof cp; i++)
    fp = (fp ^ ((const uint8_t *)&cp)[i]) * 16777619u;
  caps_fp_pref_ = global_preferences->make_preference<uint32_t>(0x53324346 /* 'S2CF' */);
  uint32_t old_fp = 0;
  caps_fp_pref_.load(&old_fp);
  if (old_fp != fp) {
    ESP_LOGI(TAG, "caps content changed (fp %08" PRIx32 " -> %08" PRIx32 ") — announcing",
             old_fp, fp);
    sl2_link_caps_changed(&link_);
    caps_fp_pref_.save(&fp);
  }

  uint8_t mac[6];
  p_own_mac(nullptr, mac);
  ESP_LOGI(TAG, "up; MAC %02X:%02X:%02X:%02X:%02X:%02X, %d Serin Link(s) bonded, %s",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           sl2_link_dial_count(&link_),
           climate_ ? "bound to climate entity" : "spike (canned device)");
}

void SerinLinkComponent::loop() {
  if (!started_) return;
  /* ESPHome's wifi component applies ITS power-save setting when the STA
   * starts — after our setup() ran. A sleeping radio never ACKs (let alone
   * receives) the dial's unicast, so re-assert PS off whenever it creeps
   * back (observed: setup saw NONE, radio slept anyway; sent=172 acked=0). */
  uint32_t now = millis();
  if (now - last_ps_check_ms_ > 5000) {
    last_ps_check_ms_ = now;
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE) {
      ESP_LOGW(TAG, "wifi power save crept back to %d — forcing NONE",
               static_cast<int>(ps));
      esp_wifi_set_ps(WIFI_PS_NONE);
    }
  }
  /* Dial room sensor: 1 Hz stale edge. Frame-driven publishing lives in
   * room_sensor_feed(); this is the one publish path that fires on the
   * ABSENCE of frames, so it cannot live there. */
  if (link_sensor_cfg_ && now - last_dial_check_ms_ >= 1000) {
    last_dial_check_ms_ = now;
    if (!dial_stale_ && dial_temp_ms_ != 0 &&
        now - dial_temp_ms_ >= dial_stale_ms_) {
      ESP_LOGW(TAG, "Serin Link room sensor stale (%" PRIu32 " ms) — publishing unknown",
               now - dial_temp_ms_);
      publish_dial_(true);
    }
    if (sensor_rows_cfg_) check_sensor_rows_(now);
  }
  /* Its own tick: the block above is gated on link_sensor_cfg_, and
   * diagnostics are useful without a Serin Link room sensor configured. */
  if (diagnostics_cfg_ && now - last_diag_ms_ >= 1000) {
    last_diag_ms_ = now;
    publish_diagnostics_(now);
  }
  /* Keep the dropdown honest against the live bond table: a forget compacts
   * the table, so the pinned Serin Link can change SLOT without changing
   * identity (the label follows it), and a pin whose Link was forgotten
   * reverts to auto. Quiet because publish_primary_() gates on change —
   * select::publish_state itself does NOT dedup. */
  if (primary_select_ != nullptr && now - last_primary_ms_ >= 1000) {
    last_primary_ms_ = now;
    refresh_primary_select_();
  }
  sl2_rxq_frame_t f;
  while (sl2_rxq_pop(&rxq_, &f)) {
    ESP_LOGV(TAG, "rx type=%u len=%u from %02X:%02X:%02X:%02X:%02X:%02X",
             f.len >= 1 ? f.data[0] : 0, f.len,
             f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5]);
    sl2_link_on_recv(&link_, f.src, f.dst, f.data, f.len);
  }
  sl2_link_loop(&link_);
}

/* Diagnostics are POLLED state, unlike everything else this component
 * publishes, so they need an explicit discipline rather than inheriting the
 * publish-on-change rule by construction. Walked at 1 Hz:
 *
 *  - mac / firmware / linked / bonded_count / pairing_status: on change only,
 *    so steady state is silent.
 *  - last_seen: neither on-change nor on-new-probe works. The dial firmware
 *    probes background zones every 4 s + up to 1.8 s of stagger (see
 *    SL2_DIAL_LIVE_MS's comment in sl2_link.h), so on a live dial this value
 *    sawtooths 0-6 s: on-change would publish every second, on-new-probe
 *    every ~4 s, and neither carries information. Published on a liveness
 *    EDGE — the actual signal, when a dial dropped and when it returned —
 *    and otherwise at most once per 60 s so an ongoing outage's duration
 *    stays visible.
 *  - pairing_seconds: the deliberate exception. 1 Hz while a window is open,
 *    a final 0 when it closes: ~60 states per pairing attempt, and a live
 *    countdown is the entity's whole purpose.
 *
 * The first pass publishes everything, so HA never holds a diagnostics entity
 * at unknown after a restart. */
void SerinLinkComponent::publish_diagnostics_(uint32_t now) {
  const bool prime = !diag_primed_;
  diag_primed_ = true;

  if (connected_sensor_ != nullptr) {
    const bool live = sl2_link_any_live(&link_);
    if (prime || !pub_any_live_valid_ || live != pub_any_live_) {
      pub_any_live_ = live;
      pub_any_live_valid_ = true;
      connected_sensor_->publish_state(live);
    }
  }

  const int n = sl2_link_dial_count(&link_);
  if (bonded_count_sensor_ != nullptr && (prime || n != pub_bonded_count_)) {
    pub_bonded_count_ = n;
    bonded_count_sensor_->publish_state(n);
  }

  if (pairing_status_sensor_ != nullptr) {
    const char *res = sl2_link_pair_result(&link_);
    if (prime || pub_pair_result_ != res) {
      pub_pair_result_ = res;
      pairing_status_sensor_->publish_state(res);
    }
  }

  if (pairing_seconds_sensor_ != nullptr) {
    const int secs = sl2_link_pairing(&link_) ? pair_seconds_left() : 0;
    if (prime || secs != pub_pair_seconds_) {
      pub_pair_seconds_ = secs;
      pairing_seconds_sensor_->publish_state(secs);
    }
  }

  for (int i = 0; i < SL2_MAX_DIALS; i++) {
    DialRow &r = dial_rows_[i];
    if (!r.declared) continue;
    sl2_dial_view_t v;
    const bool occupied = dial_view(i, &v);

    std::string mac_s;
    std::string fw_s;
    bool live = false;
    float last_seen = NAN;
    if (occupied) {
      char buf[18];
      sl2_fmt_mac(v.mac, buf);
      mac_s = buf;
      fw_s = v.have_info ? v.fw : "";
      live = v.live;
      if (v.last_seen_ms >= 0) last_seen = v.last_seen_ms / 1000.0f;
    }

    if (r.mac != nullptr && (prime || mac_s != r.pub_mac)) {
      r.pub_mac = mac_s;
      r.mac->publish_state(mac_s);
    }
    if (r.firmware != nullptr && (prime || fw_s != r.pub_fw)) {
      r.pub_fw = fw_s;
      r.firmware->publish_state(fw_s);
    }
    const bool live_edge = !r.pub_linked_valid || live != r.pub_linked;
    if (r.linked != nullptr && (prime || live_edge)) r.linked->publish_state(live);
    if (r.last_seen != nullptr &&
        (prime || live_edge || now - r.last_seen_pub_ms >= 60000)) {
      r.last_seen_pub_ms = now;
      r.last_seen->publish_state(last_seen);
    }
    r.pub_linked = live;
    r.pub_linked_valid = true;
  }
}

void SerinLinkComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Serin Link:");
  char name[32];
  copy_zone_name(name, sizeof name);
  ESP_LOGCONFIG(TAG, "  zone name: '%s'", name);
  ESP_LOGCONFIG(TAG, "  climate: %s", climate_ ? "bound" : "none (spike mode)");
  ESP_LOGCONFIG(TAG, "  cmd debounce: %" PRIu32 " ms", cmd_debounce_ms_);
  if (vane_v_sel_ || vane_h_sel_)
    ESP_LOGCONFIG(TAG, "  vanes: V=%02X H=%02X (VANECAP npos|auto|swing)",
                  vane_v_sel_ ? vane_caps_byte(vane_v_sel_) : 0,
                  vane_h_sel_ ? vane_caps_byte(vane_h_sel_) : 0);
  ESP_LOGCONFIG(TAG, "  telemetry: wifi fw sys%s%s%s%s%s",
                outside_temp_sensor_ ? " outside_t" : "",
                (compressor_hz_sensor_ || stage_sensor_ || sub_mode_sensor_ ||
                 auto_sub_mode_sensor_) ? " compressor" : "",
                battery_sensor_ ? " sensor_batt" : "",
                runtime_sensor_ ? " runtime" : "",
                (power_sensor_ || energy_sensor_) ? " energy" : "");
  if (link_sensor_cfg_) {
    ESP_LOGCONFIG(TAG, "  Serin Link room sensor: accepted, stale after %" PRIu32
                  " ms, source=%u", dial_stale_ms_,
                  static_cast<unsigned>(selected_src_));
    if (has_primary_dial_) {
      char mac[18];
      sl2_fmt_mac(primary_dial_, mac);
      ESP_LOGCONFIG(TAG, "    primary Serin Link: %s (others ignored for measurement)", mac);
    } else {
      ESP_LOGCONFIG(TAG, "    primary Serin Link: unset (last reporting Link wins)");
    }
    int n_rows = 0;
    for (int i = 0; i < SL2_MAX_DIALS; i++)
      if (sensor_rows_[i].declared) n_rows++;
    if (n_rows > 0)
      ESP_LOGCONFIG(TAG, "    per-slot sensor rows: %d (every Link's reading, "
                    "arbitration feeds only the pair above)", n_rows);
  }
  int n_dials = sl2_link_dial_count(&link_);
  ESP_LOGCONFIG(TAG, "  bonded Serin Links: %d", n_dials);
  /* The whole table, not just a count: when a dial misbehaves this is the
   * first thing anyone reads, and it works on a config that declares no
   * diagnostics: block at all. */
  for (int i = 0; i < n_dials; i++) {
    sl2_dial_view_t v;
    if (!dial_view(i, &v)) continue;
    char mac[18];
    sl2_fmt_mac(v.mac, mac);
    /* model/fw stay empty until that dial's DIAL_INFO arrives (have_info) */
    ESP_LOGCONFIG(TAG, "    [%d] %s  %s  last seen %" PRId32 " s  "
                  "model '%s' fw '%s'  caps_seq %u  cert %u",
                  i, mac, v.live ? "live" : "DOWN",
                  v.last_seen_ms < 0 ? (int32_t) -1 : v.last_seen_ms / 1000,
                  v.have_info ? v.model : "", v.have_info ? v.fw : "",
                  static_cast<unsigned>(v.caps_seq),
                  static_cast<unsigned>(v.cert_state));
  }
  ESP_LOGCONFIG(TAG, "  rxq dropped: %u", static_cast<unsigned>(rxq_.dropped));
}

}  // namespace serin_link
}  // namespace esphome
