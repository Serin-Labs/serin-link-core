/* Host tests: what the adapter derives from a bound climate platform's
 * entities — INFO telemetry codes and CAPS/CMD/STATE setpoint handling. */
#include "adapter/fixture.h"
#include "sl2_info.h"
#include <cstdio>

using namespace adapter_host;

/* The COMPRESSOR TLV value from one INFO build; empty when absent. */
static std::vector<uint8_t> compressor_tlv(SerinLinkComponent &component) {
  uint8_t tlvs[250];
  const size_t len = component.fill_info_tlvs(tlvs, sizeof tlvs);
  for (size_t off = 0; off + 2 <= len;) {
    const uint8_t tag = tlvs[off], size = tlvs[off + 1];
    require(off + 2 + size <= len, "malformed INFO TLV");
    if (tag == SL2_TLV_COMPRESSOR) return {tlvs + off + 2, tlvs + off + 2 + size};
    off += 2 + size;
  }
  return {};
}

/* cn105 publishes these states as text; the TLV must carry the raw CN105
 * bytes the reference controller forwards, which is what a Serin Link
 * decodes (sub-mode 0x02/0x04/0x08 are bit codes, not table positions). */
static void compressor_tlv_carries_cn105_codes() {
  reset();
  SerinLinkComponent component;
  esphome::text_sensor::TextSensor stage, sub_mode, auto_sub;
  component.set_stage_sensor(&stage);
  component.set_sub_mode_sensor(&sub_mode);
  component.set_auto_sub_mode_sensor(&auto_sub);
  component.setup();
  const struct { const char *state; uint8_t code; } subs[] = {
    {"NORMAL", 0x00}, {"DEFROST", 0x02}, {"PREHEAT", 0x04}, {"STANDBY", 0x08},
  };
  for (const auto &s : subs) {
    sub_mode.publish_state(s.state);
    const auto tlv = compressor_tlv(component);
    require(tlv.size() == 4, "COMPRESSOR TLV missing");
    if (tlv[2] != s.code) {
      std::fprintf(stderr, "  %s -> 0x%02X, want 0x%02X\n", s.state, tlv[2], s.code);
      throw std::runtime_error("sub-mode byte is not the CN105 code");
    }
  }
  stage.publish_state("DIFFUSE");
  auto_sub.publish_state("AUTO_LEADER");
  const auto tlv = compressor_tlv(component);
  require(tlv.size() == 4 && tlv[1] == 0x06 && tlv[3] == 0x03,
          "stage/auto-sub byte is not the CN105 code");
}

struct ClimateAdapter : SerinLinkComponent {
  void flush() { apply_pending_(); }
};

static sl2_hvac_state_t state_of(ClimateAdapter &component) {
  sl2_hvac_state_t state{};
  require(component.hvac_get_state(&state), "STATE unavailable");
  return state;
}

/* A 0.5 °C grid is Mitsubishi's, whose °F scale is the non-linear ftab 1
 * table: linear 72 °F is 22.2 °C, off the grid, which the unit stores as 22.0
 * and its remote shows as 71 °F. Through the table, 72 °F is 22.5 °C. */
static void fahrenheit_setpoint_round_trips_on_half_degree_grid() {
  reset();
  ClimateAdapter component;
  esphome::climate::Climate climate;
  climate.traits.visual_step = 0.5f;
  component.set_climate(&climate);
  component.setup();
  sl2_caps_pkt caps{};
  require(component.hvac_get_caps(&caps), "CAPS unavailable");
  require(caps.ftab_id == 1, "0.5 °C grid climate did not advertise the Mitsubishi °F table");
  /* 61..87 °F: 88 °F is 30.5 °C, past this climate's 30 °C visual max. */
  for (int f = SL2_FTAB1_MIN_F; f < SL2_FTAB1_MAX_F; ++f) {
    sl2_cmd_pkt cmd{};
    cmd.set_dc = sl2_display_to_dc(f, true, caps.ftab_id);
    require(component.hvac_apply(SL2_CM_TEMP, &cmd), "setpoint CMD rejected");
    require(sl2_dc_to_display(state_of(component).set_dc, true, caps.ftab_id) == f,
            "immediate STATE echo did not show the commanded °F");
    component.flush();
    now_ms += 11000; // past the overlay hold: STATE is the entity's own value
    if (sl2_dc_to_display(state_of(component).set_dc, true, caps.ftab_id) != f) {
      std::fprintf(stderr, "  %d F -> entity %.2f C\n", f, climate.target_temperature);
      throw std::runtime_error("°F setpoint did not round-trip through the entity");
    }
  }
  require(climate.target_temperature == 30.0f, "sweep did not reach the top of the range");
  sl2_cmd_pkt cmd{};
  cmd.set_dc = sl2_display_to_dc(72, true, caps.ftab_id);
  require(component.hvac_apply(SL2_CM_TEMP, &cmd), "72 °F CMD rejected");
  component.flush();
  require(climate.target_temperature == 22.5f, "72 °F did not apply as 22.5 °C");
  now_ms += 11000;
  require(sl2_dc_to_display(state_of(component).set_dc, true, caps.ftab_id) == 72,
          "72 °F did not round-trip as 72 °F");
}

/* An off-grid CMD (a linear-°F dial, any upstream rounding) snaps before it
 * is staged, so the echo already shows what the entity will confirm instead
 * of snapping back when the overlay expires. Band setpoints too. */
static void off_grid_setpoints_snap_to_half_degree() {
  reset();
  ClimateAdapter component;
  esphome::climate::Climate climate;
  climate.traits.visual_step = 0.5f;
  climate.traits.feature_flags = esphome::climate::CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE;
  component.set_climate(&climate);
  component.setup();
  sl2_cmd_pkt cmd{};
  cmd.set_dc = 222;
  require(component.hvac_apply(SL2_CM_TEMP, &cmd), "setpoint CMD rejected");
  cmd.set_low_dc = 202;
  cmd.set_high_dc = 243;
  require(component.hvac_apply(SL2_CM_TEMP_BAND, &cmd), "band CMD rejected");
  component.flush();
  require(climate.target_temperature == 22.0f, "off-grid setpoint applied off the grid");
  require(climate.target_temperature_low == 20.0f && climate.target_temperature_high == 24.5f,
          "off-grid band applied off the grid");
  const sl2_hvac_state_t echo = state_of(component);
  require(echo.set_low_dc == 200 && echo.set_high_dc == 245, "band echo did not show the snapped values");
}

/* Any other step keeps linear °F (ftab 0) and applies setpoints as sent. */
static void other_steps_keep_linear_fahrenheit() {
  for (float step : {0.1f, 1.0f}) {
    reset();
    ClimateAdapter component;
    esphome::climate::Climate climate;
    climate.traits.visual_step = step;
    component.set_climate(&climate);
    component.setup();
    sl2_caps_pkt caps{};
    require(component.hvac_get_caps(&caps) && caps.ftab_id == 0,
            "non-0.5 step climate advertised the Mitsubishi °F table");
    sl2_cmd_pkt cmd{};
    cmd.set_dc = 222;
    require(component.hvac_apply(SL2_CM_TEMP, &cmd), "setpoint CMD rejected");
    component.flush();
    require(climate.target_temperature == sl2_dc_to_c(222), "setpoint was snapped on a non-0.5 grid");
  }
}

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"COMPRESSOR TLV carries CN105 codes", compressor_tlv_carries_cn105_codes},
    {"°F setpoint round-trips on a 0.5 °C grid", fahrenheit_setpoint_round_trips_on_half_degree_grid},
    {"off-grid setpoints snap to 0.5 °C", off_grid_setpoints_snap_to_half_degree},
    {"other steps keep linear °F", other_steps_keep_linear_fahrenheit},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.run(); std::printf("adapter climate: PASS: %s\n", test.name); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "adapter climate: FAIL: %s: %s\n", test.name, error.what());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
