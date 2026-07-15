#include "serin_link.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/network/util.h"

#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <nvs.h>
#include <sodium.h>
#include <esp_random.h>

namespace esphome {
namespace serin_link {

static const char *const TAG = "serin_link";
static SerinLinkComponent *g_self = nullptr;

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

/* ── libsodium crypto (Ed25519 + X25519 only; HKDF is pinned in sl2_sha256.h) ── */

/* Two libsodiums can end up in one image: the espressif/libsodium managed
 * component this component pins, and the upstream copy that ESPHome's noise-c
 * PlatformIO lib drags in whenever `api: encryption:` is configured. If the
 * upstream one wins the link, its default sysrandom backend wants
 * getrandom()//dev/urandom — absent on ESP-IDF — and sodium_init() aborts the
 * whole app (boot loop into safe mode). Registering an esp_random-backed
 * implementation before sodium_init() is correct no matter which copy linked. */
static const char *rb_esp32_name(void) { return "esp32"; }
static uint32_t rb_esp32_random(void) { return esp_random(); }
static void rb_esp32_buf(void *buf, size_t size) { esp_fill_random(buf, size); }
static randombytes_implementation rb_esp32_impl = {
    rb_esp32_name,   /* implementation_name */
    rb_esp32_random, /* random */
    nullptr,         /* stir */
    nullptr,         /* uniform */
    rb_esp32_buf,    /* buf */
    nullptr,         /* close */
};

static int c_rand(void *, uint8_t *buf, size_t len) {
  randombytes_buf(buf, len);
  return 0;
}

static int c_xkp(void *, uint8_t priv[32], uint8_t pub[32]) {
  randombytes_buf(priv, 32);
  return crypto_scalarmult_curve25519_base(pub, priv);
}

static int c_xsh(void *, const uint8_t priv[32], const uint8_t peer[32],
                 uint8_t out[32]) {
  int rc = crypto_scalarmult_curve25519(out, priv, peer);
  return rc;
}


static int c_ekp(void *, uint8_t priv[64], uint8_t pub[32]) {
  return crypto_sign_ed25519_keypair(pub, priv);
}

static int c_sign(void *, const uint8_t priv[64], const uint8_t *msg,
                  size_t msg_len, uint8_t sig[64]) {
  return crypto_sign_ed25519_detached(sig, nullptr, msg, msg_len, priv);
}

static int c_verify(void *, const uint8_t pub[32], const uint8_t *msg,
                    size_t msg_len, const uint8_t sig[64]) {
  return crypto_sign_ed25519_verify_detached(sig, msg, msg_len, pub);
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
  const std::string cur = s->state;
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
  out->hvac_link = sl2_hvac_link_infer(
      static_cast<bool>(hvac_link_fn_), hvac_link_fn_ ? hvac_link_fn_() : false,
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
    ESP_LOGI(TAG, "CMD from dial: mask=0x%04x mode=%u fan=%u band=%.1f..%.1fC", mask,
             cmd->mode, cmd->fan, sl2_dc_to_c(cmd->set_low_dc), sl2_dc_to_c(cmd->set_high_dc));
  } else {
    ESP_LOGI(TAG, "CMD from dial: mask=0x%04x mode=%u fan=%u set=%.1fC", mask,
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

static size_t h_tlvs(void *, uint8_t *, size_t) { return 0; }

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

/* ── component ────────────────────────────────────────────────────────── */

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
  randombytes_set_implementation(&rb_esp32_impl);
  if (sodium_init() < 0) {
    ESP_LOGE(TAG, "sodium_init failed");
    this->mark_failed();
    return;
  }
  sl2_rxq_init(&rxq_);
  g_self = this;

  use_f_pref_ = global_preferences->make_preference<bool>(0x53324C55 /* 'S2LU' */);
  use_f_pref_.load(&use_f_);

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
  hvac_.fill_info_tlvs = h_tlvs;
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
  ESP_LOGI(TAG, "up; MAC %02X:%02X:%02X:%02X:%02X:%02X, %d dial(s) bonded, %s",
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
  sl2_rxq_frame_t f;
  while (sl2_rxq_pop(&rxq_, &f)) {
    ESP_LOGV(TAG, "rx type=%u len=%u from %02X:%02X:%02X:%02X:%02X:%02X",
             f.len >= 1 ? f.data[0] : 0, f.len,
             f.src[0], f.src[1], f.src[2], f.src[3], f.src[4], f.src[5]);
    sl2_link_on_recv(&link_, f.src, f.dst, f.data, f.len);
  }
  sl2_link_loop(&link_);
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
  ESP_LOGCONFIG(TAG, "  bonded dials: %d", sl2_link_dial_count(&link_));
  ESP_LOGCONFIG(TAG, "  rxq dropped: %u", static_cast<unsigned>(rxq_.dropped));
}

}  // namespace serin_link
}  // namespace esphome
