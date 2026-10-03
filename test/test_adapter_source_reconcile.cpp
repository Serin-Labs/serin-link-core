#include "adapter/fixture.h"
#include "sl2_info.h"
#include <cstdio>

using namespace adapter_host;

static void source_is(Fixture &f, uint64_t id, uint8_t coarse, uint8_t health) {
  uint64_t actual;
  require(f.status(&actual) == health && actual == id, "source_get did not reconcile source");
  uint64_t saved = 0;
  auto pref = esphome::global_preferences->make_preference<uint64_t>(0x53325249);
  require(pref.load(&saved) && saved == id, "saved stable source differs");
  uint8_t saved_coarse = 255;
  auto legacy = esphome::global_preferences->make_preference<uint8_t>(0x53325253);
  require(legacy.load(&saved_coarse) && saved_coarse == coarse, "saved coarse source differs");
  uint8_t tlvs[250];
  size_t len = f.component.fill_info_tlvs(tlvs, sizeof(tlvs));
  bool found_coarse = false, found_stable = false;
  for (size_t off = 0; off + 2 <= len;) {
    uint8_t tag = tlvs[off], size = tlvs[off + 1];
    require(off + 2 + size <= len, "malformed INFO TLV");
    const uint8_t *value = tlvs + off + 2;
    if (tag == SL2_TLV_ROOM_SRC) {
      require(size == 2 && value[0] == coarse && value[1] == health, "coarse INFO source differs");
      found_coarse = true;
    }
    if (tag == SL2_TLV_ROOM_SOURCE_V2) {
      uint64_t wire_id = 0;
      require(size == 13, "stable INFO size mismatch");
      for (unsigned i = 0; i < 8; ++i) wire_id |= static_cast<uint64_t>(value[4 + i]) << (8 * i);
      require(wire_id == id && value[12] == health, "stable INFO source differs");
      found_stable = true;
    }
    off += 2 + size;
  }
  require(found_coarse && found_stable, "missing source INFO views");
}

static void internal_after_removal(Fixture &f) {
  source_is(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMSRC_INTERNAL, SL2_ROOMST_OK);
  require(!f.component.room_src_is_link() && !f.component.has_primary() &&
          f.component.primary_cleared(), "forgotten primary MAC retained");
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 0.0f}, "forget must emit one temperature reset");
  require(std::isnan(f.temperature.state) && std::isnan(f.humidity.state) &&
          f.mac.state.empty(), "forgotten dial reading retained");
  f.run_loop();
  f.run_loop();
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 0.0f}, "loop repeated temperature reset");
}

static void selected_forget_without_select() {
  Fixture f;
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  require(f.forget(1), "selected forget failed");
  internal_after_removal(f);
}

static void selected_forget_all_without_select() {
  Fixture f;
  f.select_id(link_id(2));
  f.feed(2, 2200, 4500);
  require(f.forget_all(), "forget-all failed");
  require(f.component.dial_count() == 0, "forget-all retained bonds");
  internal_after_removal(f);
}

static void selected_slot_forget_with_select() {
  Fixture f(true);
  f.select_id(link_id(2));
  f.feed(2, 2200, 4500);
  require(f.component.forget_dial_slot(1), "selected slot forget failed");
  internal_after_removal(f);
  require(f.select.index == 0, "dropdown did not revert to Heat pump");
}

static void failed_forget_preserves_source() {
  for (bool all : {false, true}) {
    Fixture f;
    f.select_id(link_id(1));
    f.feed(1, 2200, 4500);
    const auto bonds = nvs[SL2_KV_BONDS];
    inject_storage_failure(true);
    require(!(all ? f.forget_all() : f.forget(1)), "failed persistence reported successful forget");
    f.run_loop();
    source_is(f, link_id(1), SL2_ROOMSRC_LINK, SL2_ROOMST_OK);
    require(f.component.dial_count() == 2 && nvs[SL2_KV_BONDS] == bonds, "failed forget changed bonds");
    require(f.component.has_primary() && f.component.primary_is(1), "failed forget changed primary MAC");
    require(f.room_temperature.emitted == std::vector<float>{22.0f}, "failed forget reset temperature");
    require(f.temperature.state == 22.0f && f.humidity.state == 45.0f, "failed forget cleared reading");
  }
}

static void unrelated_removal_preserves_selected_identity() {
  for (bool dropdown : {false, true}) {
    Fixture f(dropdown);
    f.select_id(link_id(2));
    f.feed(2, 2200, 4500);
    require(f.component.forget_dial_slot(0), "unrelated slot forget failed");
    f.run_loop();
    source_is(f, link_id(2), SL2_ROOMSRC_LINK, SL2_ROOMST_OK);
    require(f.component.has_primary() && f.component.primary_is(2), "compaction changed primary MAC");
    require(f.component.dial_mac_str(0) == "02:00:00:00:00:02", "selected bond did not compact");
    require(f.room_temperature.emitted == std::vector<float>{22.0f}, "unrelated forget reset temperature");
    if (dropdown) require(f.select.index == 1, "dropdown did not follow compacted selected bond");
    f.feed(2, 2300);
    require(f.room_temperature.emitted == std::vector<float>{22.0f, 23.0f}, "compacted selection stopped feeding temperature");
  }
}

static void direct_core_removal_reconciles_in_loop() {
  for (bool all : {false, true}) {
    Fixture f;
    f.select_id(link_id(1));
    f.feed(1, 2200, 4500);
    require(all ? sl2_link_forget_all(f.component.core())
                : sl2_link_forget_dial(f.component.core(), dial_mac(1).data()), "direct core forget failed");
    f.run_loop();
    internal_after_removal(f);
  }
}

static void startup_reconciliation_does_not_reset_temperature() {
  {
    // No v4 preference: fold an unpinned v3 Link choice, then resolve it.
    Fixture f(false, {1}, std::nullopt, SL2_ROOMSRC_LINK);
    source_is(f, link_id(1), SL2_ROOMSRC_LINK, SL2_ROOMST_UNAVAILABLE);
    f.run_loop();
    require(f.room_temperature.emitted.empty(), "v3 startup migration emitted a reset");
  }
  {
    Fixture f(false, {1}, SL2_ROOM_SOURCE_LINK_AUTO_ID, SL2_ROOMSRC_LINK);
    source_is(f, link_id(1), SL2_ROOMSRC_LINK, SL2_ROOMST_UNAVAILABLE);
    f.run_loop();
    require(f.room_temperature.emitted.empty(), "startup automatic pin emitted a reset");
  }
  for (uint64_t missing : {SL2_ROOM_SOURCE_LINK_AUTO_ID, link_id(3)}) {
    Fixture f(false, {1, 2}, missing, SL2_ROOMSRC_LINK);
    source_is(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMSRC_INTERNAL, SL2_ROOMST_OK);
    f.run_loop();
    require(f.room_temperature.emitted.empty(), "startup fallback emitted a reset");
  }
}

static void legacy_ble_and_external_survive_bond_removal() {
  {
    Fixture f(false, {1, 2}, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMSRC_BLE);
    require(f.forget_all(), "BLE forget-all failed");
    f.run_loop();
    source_is(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMSRC_BLE, SL2_ROOMST_UNAVAILABLE);
    require(f.room_temperature.emitted.empty(), "BLE bond removal emitted a reset");
  }
  {
    Fixture f;
    esphome::sensor::Sensor external;
    f.component.add_room_source("External", &external);
    uint64_t external_id = sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "External");
    external.publish_state(18.0f);
    f.select_id(external_id);
    require(f.forget_all(), "external forget-all failed");
    f.run_loop();
    source_is(f, external_id, SL2_ROOMSRC_INTERNAL, SL2_ROOMST_OK);
    external.publish_state(19.0f);
    require(f.room_temperature.emitted == std::vector<float>{18.0f, 19.0f}, "external bond removal changed temperature feed");
  }
}

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"selected forget without select", selected_forget_without_select},
    {"selected forget-all without select", selected_forget_all_without_select},
    {"selected slot forget with select", selected_slot_forget_with_select},
    {"failed forget preserves source", failed_forget_preserves_source},
    {"unrelated removal preserves identity", unrelated_removal_preserves_selected_identity},
    {"direct core removal reconciles in loop", direct_core_removal_reconciles_in_loop},
    {"startup reconciliation stays quiet", startup_reconciliation_does_not_reset_temperature},
    {"BLE and external survive bond removal", legacy_ble_and_external_survive_bond_removal},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.run(); std::printf("adapter source reconcile: PASS: %s\n", test.name); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "adapter source reconcile: FAIL: %s: %s\n", test.name, error.what());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
