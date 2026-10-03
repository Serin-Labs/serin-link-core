#pragma once
#include "esphome_host.h"
#include "serin_link.h"
#include <array>
#include <initializer_list>
#include <stdexcept>

namespace adapter_host {
using esphome::serin_link::SerinLinkComponent;
inline std::array<uint8_t, 6> dial_mac(uint8_t n) { return {2, 0, 0, 0, 0, n}; }
inline uint64_t link_id(uint8_t n) {
  return sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, dial_mac(n).data());
}
inline void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
inline void initialize_bonds(std::initializer_list<uint8_t> dials) {
  sl2_dial_bond_t records[SL2_MAX_DIALS]{};
  require(dials.size() <= SL2_MAX_DIALS, "fixture bond limit");
  size_t i = 0;
  for (uint8_t n : dials) {
    std::memcpy(records[i].mac, dial_mac(n).data(), 6);
    ++i;
  }
  uint8_t blob[SL2_BONDS_BLOB_MAX];
  size_t len = sl2_bonds_encode(records, static_cast<int>(i), blob, sizeof(blob));
  require(len != 0, "fixture bond encoding");
  nvs[SL2_KV_BONDS] = {blob, blob + len};
}
inline void inject_storage_failure(bool fail) { fail_storage = fail; }

// Uses the public adapter lifecycle and APIs. Platform entities record
// published values; all source arbitration, cache and core logic is real.
struct AdapterProbe : SerinLinkComponent {
  sl2_link_t *core() { return &link_; }
  bool has_primary() const { return has_primary_dial_; }
  bool primary_is(uint8_t dial) const {
    return std::memcmp(primary_dial_, dial_mac(dial).data(), 6) == 0;
  }
  bool primary_cleared() const {
    const uint8_t empty[6]{};
    return std::memcmp(primary_dial_, empty, 6) == 0;
  }
};
struct Fixture {
  AdapterProbe component;
  esphome::sensor::Sensor temperature, humidity;
  esphome::text_sensor::TextSensor mac;
  esphome::Trigger<float> room_temperature;
  esphome::select::Select select;

  explicit Fixture(bool with_select = false,
                   std::initializer_list<uint8_t> dials = {1, 2},
                   std::optional<uint64_t> initial_source = SL2_ROOM_SOURCE_INTERNAL_ID,
                   uint8_t legacy_source = SL2_ROOMSRC_INTERNAL) {
    reset();
    initialize_bonds(dials);
    if (initial_source) initialize_source_preference(*initial_source);
    auto legacy = esphome::global_preferences->make_preference<uint8_t>(0x53325253);
    require(legacy.save(&legacy_source), "fixture coarse source preference");
    component.set_link_sensor_enabled();
    component.set_dial_temp_sensor(&temperature);
    component.set_dial_hum_sensor(&humidity);
    component.set_dial_mac_sensor(&mac);
    component.add_room_temp_trigger(&room_temperature);
    if (with_select) component.set_room_source_select(&select);
    component.setup();
    require(!component.failed, "adapter setup failed");
    require(component.dial_count() == static_cast<int>(dials.size()), "adapter bond load failed");
  }
  static void initialize_source_preference(uint64_t source) {
    // The stable source preference is populated before actual setup(), so
    // tests can cover boot selection without exposing production internals.
    const auto *bytes = reinterpret_cast<const uint8_t *>(&source);
    preferences[0x53325249] = {bytes, bytes + sizeof(source)};
  }
  uint8_t status(uint64_t *id = nullptr) {
    uint32_t revision;
    uint64_t selected;
    uint8_t result;
    require(component.room_source_get(&revision, &selected, &result), "source get failed");
    if (id != nullptr) *id = selected;
    return result;
  }
  void select_id(uint64_t id) {
    uint32_t revision;
    uint64_t selected;
    uint8_t health;
    require(component.room_source_get(&revision, &selected, &health), "source get failed");
    require(component.room_source_set(revision, id) == SL2_ROOM_SET_OK, "source set failed");
  }
  void feed(uint8_t dial, int16_t temp, uint16_t hum = SL2_HUM_CC_NA,
            uint8_t flags = SL2_DSF_HAS_SENSOR, uint8_t want = SL2_ROOMSRC_NOEDIT) {
    sl2_dial_sensor_pkt packet{};
    packet.type = SL2_PKT_DIAL_SENSOR;
    packet.version = SL2_PROTO_VERSION;
    packet.flags = flags;
    packet.temp_cc = temp;
    packet.hum_cc = hum;
    packet.want_src = want;
    component.room_sensor_feed(dial_mac(dial).data(), &packet, want != SL2_ROOMSRC_NOEDIT);
  }
  void run_loop(uint32_t advance_ms = 1000) {
    now_ms += advance_ms;
    component.loop();
  }
  bool forget(uint8_t dial) { return component.forget_dial_mac(dial_mac(dial).data()); }
  bool forget_all() { return component.forget_all_dials(); }
};
}
