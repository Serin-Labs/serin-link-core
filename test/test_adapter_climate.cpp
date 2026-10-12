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

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"COMPRESSOR TLV carries CN105 codes", compressor_tlv_carries_cn105_codes},
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
