#include "adapter/fixture.h"
#include "sl2_info.h"
#include <cstdio>

using namespace adapter_host;

static void selected(Fixture &f, uint64_t id, uint8_t health) {
  uint64_t actual;
  require(f.status(&actual) == health, "selected source health mismatch");
  require(actual == id, "selected source ID mismatch");
  // Verify the same selected identity/health reaches both INFO views.
  uint8_t tlvs[250];
  size_t len = f.component.fill_info_tlvs(tlvs, sizeof(tlvs));
  bool coarse = false, stable = false;
  for (size_t off = 0; off + 2 <= len;) {
    uint8_t tag = tlvs[off], size = tlvs[off + 1];
    require(off + 2 + size <= len, "malformed INFO TLV");
    const uint8_t *value = tlvs + off + 2;
    if (tag == SL2_TLV_ROOM_SRC) {
      require(size == 2 && value[1] == health, "coarse INFO health mismatch");
      coarse = true;
    }
    if (tag == SL2_TLV_ROOM_SOURCE_V2) {
      uint64_t wire_id = 0;
      require(size == 13, "stable INFO size mismatch");
      for (unsigned i = 0; i < 8; ++i) wire_id |= static_cast<uint64_t>(value[4 + i]) << (8 * i);
      require(wire_id == id && value[12] == health, "stable INFO source/health mismatch");
      stable = true;
    }
    off += 2 + size;
  }
  require(coarse && stable, "missing source INFO views");
}

static void never_seen_selected_dial_is_unavailable() {
  Fixture f;
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  require(f.status() == SL2_ROOMST_OK, "fresh A should be OK");
  require(f.room_temperature.emitted == std::vector<float>{22.0f}, "A must feed temperature event");
  f.select_id(link_id(2));
  require(f.status() == SL2_ROOMST_UNAVAILABLE, "never-seen B inherited A health");
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  require(f.room_temperature.emitted.size() == 1, "B selection must not feed A temperature");
  require(std::isnan(f.temperature.state), "B selection retained A HA temperature");
  require(std::isnan(f.humidity.state), "B selection retained A HA humidity");
  require(f.mac.state.empty(), "B selection retained A reading identity");
}

static void sensorless_and_edit_only_dial_stay_unavailable() {
  Fixture f;
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  f.select_id(link_id(2));
  f.feed(2, SL2_CC_NA, SL2_HUM_CC_NA, 0);
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  require(std::isnan(f.temperature.state) && std::isnan(f.humidity.state), "sensorless B retained A readings");
  f.feed(2, SL2_CC_NA, 6000); // Sensing hardware, but no valid temperature yet.
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  require(f.room_temperature.emitted.size() == 1, "edit-only B emitted a temperature");
  f.feed(1, 2500, 5000); // Chattering nonselected A cannot heal B.
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  require(std::isnan(f.humidity.state), "nonselected A leaked humidity");
}

static void equal_temperature_first_frame_publishes() {
  Fixture f(true);
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  f.select_id(link_id(2));
  now_ms += 10; // Well inside the 30-second dedup window.
  f.feed(2, 2200);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 22.0f}, "B first equal temperature was deduped");
  require(f.temperature.state == 22.0f, "B first frame missing from HA");
  require(std::isnan(f.humidity.state), "B without humidity inherited A HA humidity");
  require(f.mac.state == "02:00:00:00:00:02", "B first frame must publish B MAC");
  require(f.select.index == 2, "HA selected source must be B");
  selected(f, link_id(2), SL2_ROOMST_OK);
  size_t count = f.temperature.emitted.size();
  f.select_id(link_id(2));
  f.feed(2, 2200);
  require(f.temperature.emitted.size() == count, "same-source retry defeated dedup");
  require(f.room_temperature.emitted.size() == 2, "same-source retry duplicated event");
  now_ms += 30000;
  f.feed(2, 2200, 4800);
  require(f.humidity.state == 48.0f && f.room_temperature.emitted.size() == 3, "B heartbeat must publish its own humidity");
}

static void stale_switch_back_never_injects_expired_value() {
  Fixture f;
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  f.run_loop(90000);
  selected(f, link_id(1), SL2_ROOMST_STALE);
  require(std::isnan(f.temperature.state) && std::isnan(f.humidity.state), "stale A HA values not retracted");
  f.select_id(link_id(2));
  f.select_id(link_id(1));
  require(f.room_temperature.emitted == std::vector<float>{22.0f}, "switch back injected expired A temperature");
  selected(f, link_id(1), SL2_ROOMST_UNAVAILABLE);
  f.feed(1, 2200);
  selected(f, link_id(1), SL2_ROOMST_OK);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 22.0f}, "fresh switch-back A must publish immediately");
  require(std::isnan(f.humidity.state), "switch-back A inherited expired humidity");
  f.run_loop(90000);
  f.feed(1, 2200, 5100);
  selected(f, link_id(1), SL2_ROOMST_OK);
  require(f.temperature.state == 22.0f && f.humidity.state == 51.0f, "stale same-source recovery not published");
  require(f.room_temperature.emitted.size() == 3, "stale recovery event missing");
}

static void nonselected_slot_readings_remain_independent() {
  Fixture f;
  esphome::sensor::Sensor a_temp, a_hum, b_temp, b_hum;
  f.component.add_sensor_row(0, &a_temp, &a_hum);
  f.component.add_sensor_row(1, &b_temp, &b_hum);
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  f.feed(2, 1900, 6000);
  require(a_temp.state == 22.0f && b_temp.state == 19.0f, "per-slot readings should see both dials");
  require(f.temperature.state == 22.0f && f.humidity.state == 45.0f, "nonselected B changed selected readings");
  f.select_id(link_id(2));
  require(a_temp.state == 22.0f && b_hum.state == 60.0f, "selection change cleared diagnostic rows");
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  f.feed(2, 1900, 6000);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 19.0f}, "B recovery must feed its own value");
}

static void internal_and_external_transitions_do_not_reuse_dial_history() {
  Fixture f;
  esphome::sensor::Sensor external;
  f.component.add_room_source("External", &external);
  uint64_t external_id = sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "External");
  external.publish_state(18.0f);
  f.select_id(link_id(1));
  f.feed(1, 2200, 4500);
  f.select_id(external_id);
  selected(f, external_id, SL2_ROOMST_OK);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 18.0f}, "external selection must feed its own value");
  f.feed(2, 2200);
  require(f.mac.state == "02:00:00:00:00:02", "unselected Link telemetry must publish new identity");
  require(std::isnan(f.humidity.state), "unselected B inherited A humidity");
  require(f.room_temperature.emitted.size() == 2, "Link frame fed while external selected");
  f.select_id(link_id(2));
  f.feed(2, 2200);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 18.0f, 22.0f}, "Link return must publish first reading");
  f.select_id(SL2_ROOM_SOURCE_INTERNAL_ID);
  require(f.room_temperature.emitted.back() == 0.0f, "internal selection must clear remote temperature");
  selected(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMST_OK);
  f.select_id(link_id(2));
  f.feed(2, 2200);
  require(f.room_temperature.emitted.back() == 22.0f && f.room_temperature.emitted.size() == 5, "internal return skipped first Link sample");
}

static void legacy_edits_reset_health_and_allow_catalog_recovery() {
  Fixture f;
  f.feed(1, 2200, 4500, SL2_DSF_HAS_SENSOR, SL2_ROOMSRC_LINK);
  selected(f, link_id(1), SL2_ROOMST_OK);
  f.feed(2, SL2_CC_NA, SL2_HUM_CC_NA, 0, SL2_ROOMSRC_BLE);
  selected(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMST_UNAVAILABLE);
  require(std::isnan(f.temperature.state) && std::isnan(f.humidity.state), "BLE edit retained outgoing Link readings");
  f.select_id(SL2_ROOM_SOURCE_INTERNAL_ID);
  require(f.room_temperature.emitted.back() == 0.0f, "catalog internal must clear legacy BLE remote temperature");
  selected(f, SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOMST_OK);
  f.feed(2, SL2_CC_NA, SL2_HUM_CC_NA, 0, SL2_ROOMSRC_LINK);
  selected(f, link_id(2), SL2_ROOMST_UNAVAILABLE);
  f.feed(2, 2200);
  selected(f, link_id(2), SL2_ROOMST_OK);
  require(f.room_temperature.emitted == std::vector<float>{22.0f, 0.0f, 22.0f}, "legacy B first equal sample must publish");
}

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"never-seen selected dial", never_seen_selected_dial_is_unavailable},
    {"sensorless/edit-only selected dial", sensorless_and_edit_only_dial_stay_unavailable},
    {"equal-temperature first frame", equal_temperature_first_frame_publishes},
    {"stale switch-back and recovery", stale_switch_back_never_injects_expired_value},
    {"independent sensor rows", nonselected_slot_readings_remain_independent},
    {"internal/external source transitions", internal_and_external_transitions_do_not_reuse_dial_history},
    {"legacy edit transitions", legacy_edits_reset_health_and_allow_catalog_recovery},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.run(); std::printf("adapter source health: PASS: %s\n", test.name); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "adapter source health: FAIL: %s: %s\n", test.name, error.what());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
