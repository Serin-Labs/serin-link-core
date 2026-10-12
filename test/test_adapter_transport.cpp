#include "adapter/fixture.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <cstdio>

using namespace adapter_host;

static void tick(AdapterProbe &component, uint32_t elapsed = 1000) {
  now_ms += elapsed;
  component.loop();
}

static void startup_failure_recovers(int &fault) {
  reset();
  initialize_bonds({1, 2});
  const auto saved_bonds = nvs.at(SL2_KV_BONDS);
  fault = 1;
  AdapterProbe component;
  component.setup();
  require(!component.core()->started, "failed transport was reported ready");
  require(!component.failed, "transient transport failure permanently disabled component");
  tick(component, 999);
  require(!component.core()->started, "retry ignored backoff");
  tick(component, 1);
  require(component.core()->started, "transport did not retry successfully");
  require(peers.size() == 2, "retry did not install every saved peer");
  require(nvs.at(SL2_KV_BONDS) == saved_bonds, "retry changed saved bonds");
}

static void init_retry() { startup_failure_recovers(fail_init); }
static void pmk_retry() { startup_failure_recovers(fail_pmk); }
static void callback_retry() { startup_failure_recovers(fail_callback); }
static void peer_retry() { startup_failure_recovers(fail_peer); }
static void event_retry() { startup_failure_recovers(fail_event); }

static void startup_retries_are_bounded() {
  reset();
  initialize_bonds({1});
  fail_callback = 1000;
  AdapterProbe component;
  component.setup();
  for (unsigned i = 0; i < 180; ++i) tick(component);
  require(!component.failed, "recoverable failure marked component permanently failed");
  require(!component.core()->started, "persistent callback failure reported ready");
  require(callback_calls == 5, "startup retry budget is not five attempts");
}

static void startup_retry_wraps_clock() {
  reset();
  now_ms = UINT32_MAX - 500;
  fail_init = 1;
  AdapterProbe component;
  component.setup();
  tick(component, 999);
  require(!component.core()->started, "retry fired early across clock wrap");
  tick(component, 1);
  require(component.core()->started, "retry stalled across clock wrap");
}

static bool probe(AdapterProbe &component, uint8_t dial = 1) {
  sl2_probe_pkt packet{};
  packet.type = SL2_PKT_PROBE;
  packet.version = SL2_PROTO_VERSION;
  bool delivered = deliver(dial_mac(dial).data(), component.core()->own_mac, &packet, sizeof packet);
  tick(component, 1);
  return delivered && component.core()->dial[dial - 1].last_probe_ms == now_ms;
}

static void missing_peer_recovers_without_reset() {
  Fixture f;
  require(probe(f.component), "initial receive failed");
  const auto saved = nvs;
  const auto epoch = f.component.core()->epoch;
  const auto healthy_peer = peers.at(dial_mac(2));
  const int initializations = init_calls, installs = peer_calls;
  peers.erase(dial_mac(1));
  require(!probe(f.component), "lost peer still delivered encrypted traffic");
  tick(f.component, 5000);
  require(peers.count(dial_mac(1)), "missing peer was not restored");
  require(init_calls == initializations, "peer loss reset the whole transport");
  require(peer_calls == installs + 1, "repair rewrote a healthy peer");
  require(std::memcmp(&healthy_peer, &peers.at(dial_mac(2)), sizeof healthy_peer) == 0, "healthy peer changed");
  require(f.component.core()->epoch == epoch && nvs == saved, "recovery changed core session or storage");
  require(probe(f.component), "repaired peer did not deliver a probe");
}

static void bad_peer_configuration_recovers() {
  for (int fault = 0; fault < 4; ++fault) {
    Fixture f;
    auto &peer = peers.at(dial_mac(1));
    if (fault == 0) peer.encrypt = false;
    if (fault == 1) peer.channel = 11;
    if (fault == 2) peer.ifidx = 1;
    if (fault == 3) peer.lmk[0] ^= 0x80;
    tick(f.component, 5000);
    const auto &fixed = peers.at(dial_mac(1));
    require(fixed.encrypt && fixed.channel == 0 && fixed.ifidx == WIFI_IF_STA &&
            std::memcmp(fixed.lmk, f.component.core()->dial[0].bond.lmk, 16) == 0,
            "misconfigured peer was not repaired");
  }
}

static void silent_link_does_not_trigger_repair() {
  Fixture f;
  const int installs = peer_calls, initializations = init_calls, callbacks = callback_calls;
  for (unsigned i = 0; i < 180; ++i) tick(f.component);
  require(peer_calls == installs && init_calls == initializations && callback_calls == callbacks,
          "silence triggered transport mutation");
}

static void reconnect_refreshes_receive_without_rekeying() {
  Fixture f;
  const auto saved = nvs;
  const auto epoch = f.component.core()->epoch;
  const int installs = peer_calls, initializations = init_calls;
  receive_callback = nullptr; // Fault injection; not an assumption about IDF reconnects.
  wifi_event(WIFI_EVENT_STA_DISCONNECTED, 201);
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  require(!receive_callback, "Wi-Fi event callback mutated the radio");
  tick(f.component);
  require(probe(f.component), "reconnect did not restore callback delivery");
  require(peer_calls == installs && init_calls == initializations, "reconnect replaced intact radio peers");
  require(f.component.core()->epoch == epoch && nvs == saved, "reconnect reset core session/storage");
  bool logged_reason = false;
  for (const auto &line : logs) if (line.find("disconnect reason=201") != std::string::npos) logged_reason = true;
  require(logged_reason, "disconnect reason was not logged");
}

static void driver_loss_recovers_without_core_restart() {
  Fixture f;
  require(probe(f.component), "initial receive failed");
  const auto epoch = f.component.core()->epoch;
  const auto last_probe = f.component.core()->dial[0].last_probe_ms;
  const auto saved = nvs;
  radio_initialized = false;
  peers.clear();
  receive_callback = nullptr;
  tick(f.component, 5000);
  require(peers.size() == 2, "uninitialized transport was not rebuilt");
  require(f.component.core()->epoch == epoch && f.component.core()->dial[0].last_probe_ms == last_probe &&
          nvs == saved, "transport recovery reinitialized the protocol core");
  require(probe(f.component), "rebuilt transport cannot receive");
}

static void repair_retries_are_bounded_and_rearmed_by_wifi() {
  Fixture f;
  peers.erase(dial_mac(1));
  const int installs = peer_calls;
  fail_peer = 100;
  for (unsigned i = 0; i < 180; ++i) tick(f.component);
  require(peer_calls == installs + 5, "runtime repair retries were not bounded");
  require(probe(f.component, 2), "failed repair prevented healthy peer delivery");
  fail_peer = 0;
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  tick(f.component);
  require(probe(f.component), "Wi-Fi event did not rearm exhausted recovery");
}

static void startup_budget_rearms_on_wifi_start() {
  reset();
  fail_init = 100;
  AdapterProbe component;
  component.setup();
  for (unsigned i = 0; i < 40; ++i) tick(component);
  require(init_calls == 5, "startup retry budget changed");
  fail_init = 0;
  wifi_event(WIFI_EVENT_STA_START);
  tick(component);
  require(component.core()->started, "STA start did not rearm failed startup");
}

static void missing_wifi_handler_can_recover_on_connection_edge() {
  reset();
  fail_event = 100;
  AdapterProbe component;
  component.setup();
  for (unsigned i = 0; i < 40; ++i) tick(component);
  require(event_calls == 5 && !component.core()->started, "event registration retry cap changed");
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  for (unsigned i = 0; i < 80; ++i) tick(component);
  require(event_calls == 10, "connection edge failed to rearm once, or persistent connection kept retrying");
  wifi_event(WIFI_EVENT_STA_DISCONNECTED);
  tick(component);
  fail_event = 0;
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  tick(component);
  require(component.core()->started && wifi_event_callback != nullptr,
          "missing event handler prevented recovery on Wi-Fi connection");
}

static void recovery_defers_while_pairing() {
  Fixture f;
  f.component.pair_start();
  const auto deadline = f.component.core()->pair_deadline_ms;
  auto &candidate = peers.at(dial_mac(1));
  candidate.lmk[0] = 99;
  const int installs = peer_calls;
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  tick(f.component, 5000);
  require(peer_calls == installs && candidate.lmk[0] == 99, "repair clobbered candidate peer during pairing");
  require(f.component.core()->pair_deadline_ms == deadline && sl2_link_pairing(f.component.core()),
          "reconnect reset pairing state");
  sl2_link_pair_cancel(f.component.core());
  tick(f.component, 5000);
  require(peers.at(dial_mac(1)).lmk[0] == 0, "deferred repair did not resume after pairing");
}

/* ESPHome re-applies its power_save_mode when the STA restarts, and modem
 * sleep drops ESP-NOW RX. The connect edge forces it off at once: the 5 s
 * audit is deferred while a pairing window is open, exactly when the pairing
 * broadcasts must get through. */
static void reconnect_forces_power_save_off() {
  Fixture f;
  f.component.pair_start();
  require(f.component.pairing(), "pairing window did not open");
  wifi_event(WIFI_EVENT_STA_DISCONNECTED, 8);
  power_save = 1; // ESPHome's STA_START handler re-applied modem sleep
  const int calls = ps_calls;
  wifi_event(WIFI_EVENT_STA_CONNECTED);
  require(ps_calls == calls, "Wi-Fi event callback mutated the radio");
  tick(f.component, 1);
  require(ps_calls == calls + 1, "connect event did not call the power-save setter");
  require(power_save == WIFI_PS_NONE, "connect event left Wi-Fi power save on");
}

static void connected_power_save_checked_once_per_second() {
  Fixture f;
  wifi_connected = true;
  const int reads = ps_get_calls, writes = ps_calls;
  tick(f.component, 999);
  require(ps_get_calls == reads && ps_calls == writes, "power-save check ran before one second");
  tick(f.component, 1);
  require(ps_get_calls == reads + 1, "connected loop missed one-second power-save check");
  require(ps_calls == writes, "already-disabled power save called setter");
  tick(f.component, 0);
  tick(f.component, 999);
  require(ps_get_calls == reads + 1 && ps_calls == writes, "power-save check ran more than once per second");
  tick(f.component, 1);
  require(ps_get_calls == reads + 2 && ps_calls == writes, "power-save check did not repeat at one second");
}

static void runtime_power_save_reversion_recovers_during_pairing() {
  Fixture f;
  wifi_connected = true;
  f.component.pair_start();
  tick(f.component, 1000);
  power_save = 1; // Runtime high-performance release restored modem sleep.
  const int writes = ps_calls, installs = peer_calls, callbacks = callback_calls;
  tick(f.component, 999);
  require(power_save == 1 && ps_calls == writes, "runtime reversion ignored one-second cadence");
  tick(f.component, 1);
  require(power_save == WIFI_PS_NONE && ps_calls == writes + 1,
          "runtime power-save reversion survived one-second check during pairing");
  require(f.component.pairing() && peer_calls == installs && callback_calls == callbacks,
          "power-save correction disturbed pairing transport");
}

static void disconnected_periodic_power_save_check_is_idle() {
  Fixture f;
  power_save = 1;
  const int reads = ps_get_calls, writes = ps_calls;
  // Stay below the existing five-second transport audit to isolate the new cadence.
  for (unsigned i = 0; i < 3; ++i) tick(f.component, 1000);
  tick(f.component, 999);
  require(ps_get_calls == reads && ps_calls == writes && power_save == 1,
          "disconnected periodic check accessed power save");
  wifi_connected = true;
  tick(f.component, 1);
  require(ps_get_calls == reads + 1 && ps_calls == writes + 1 && power_save == WIFI_PS_NONE,
          "connected periodic check did not resume after disconnected loops");
}

static void periodic_power_save_check_wraps_clock() {
  reset();
  now_ms = UINT32_MAX - 500;
  wifi_connected = true;
  initialize_bonds({1});
  AdapterProbe component;
  component.setup();
  const int reads = ps_get_calls, writes = ps_calls;
  power_save = 1;
  tick(component, 999);
  require(ps_get_calls == reads && ps_calls == writes, "power-save check fired early across clock wrap");
  tick(component, 1);
  require(ps_get_calls == reads + 1 && ps_calls == writes + 1 && power_save == WIFI_PS_NONE,
          "power-save check stalled across clock wrap");
}

static void periodic_power_save_check_preserves_transport_audit_cadence() {
  Fixture f;
  wifi_connected = true;
  peers.erase(dial_mac(1));
  const int installs = peer_calls;
  for (unsigned i = 0; i < 4; ++i) tick(f.component, 1000);
  tick(f.component, 999);
  require(peer_calls == installs && !peers.count(dial_mac(1)), "transport audit ran before five seconds");
  tick(f.component, 1);
  require(peer_calls == installs + 1 && peers.count(dial_mac(1)), "five-second transport audit missed peer loss");
}

/* A dial's long-lived Ed25519 identity, derived from its number. */
static void dial_identity(uint8_t dial, uint8_t id_priv[64], uint8_t id_pub[32]) {
  uint8_t seed[32];
  std::memset(seed, 0x20 + dial, sizeof seed);
  crypto_ed25519_key_pair(id_priv, id_pub, seed);
}

/* A PAIR_REQ, signed by default, delivered the way the radio does: into the
 * receive queue, drained by loop(). */
static void pair_request(AdapterProbe &component, uint8_t dial,
                         uint8_t version = SL2_PROTO_VERSION,
                         size_t len = sizeof(sl2_pair_req_pkt), bool sign = true) {
  sl2_pair_req_pkt req{};
  req.type = SL2_PKT_PAIR_REQ;
  req.version = version;
  std::memcpy(req.src_mac, dial_mac(dial).data(), 6);
  uint8_t eph_priv[32], id_priv[64];
  std::memset(eph_priv, 0x10 + dial, sizeof eph_priv);
  crypto_x25519_public_key(req.eph_pub, eph_priv);
  dial_identity(dial, id_priv, req.id_pub);
  uint8_t transcript[SL2_REQ_TRANSCRIPT_LEN];
  sl2_pair_req_transcript(&req, transcript);
  if (sign) crypto_ed25519_sign(req.sig, id_priv, transcript, sizeof transcript);
  sl2_rxq_push(&component.rxq_, dial_mac(dial).data(), SL2_BCAST_MAC,
               reinterpret_cast<const uint8_t *>(&req), len);
  tick(component, 1);
}

/* Unauthenticated or malformed new-dial requests must not produce a capacity
 * verdict; the real receive drain still discards them without a response. */
static void invalid_pair_request_at_capacity(uint8_t version, size_t len, bool sign) {
  Fixture f(false, {1});
  f.component.set_max_links(1);
  f.component.pair_start();
  const std::string before = f.component.pair_result();
  const auto saved = nvs;
  const int sent = sends;
  pair_request(f.component, 2, version, len, sign);
  require(f.component.pair_result() == before,
          "invalid PAIR_REQ at capacity changed the pairing status");
  require(f.component.core()->pair == SL2_PAIR_WINDOW &&
          f.component.dial_count() == 1 && sends == sent && nvs == saved,
          "invalid PAIR_REQ at capacity was accepted or answered");
}

static void unsigned_pair_request_at_capacity_is_ignored() {
  invalid_pair_request_at_capacity(SL2_PROTO_VERSION, sizeof(sl2_pair_req_pkt), false);
}

static void old_version_pair_request_at_capacity_is_ignored() {
  invalid_pair_request_at_capacity(SL2_PAIR_AUTH_MIN_VER - 1, sizeof(sl2_pair_req_pkt), true);
}

static void short_pair_request_at_capacity_is_ignored() {
  invalid_pair_request_at_capacity(SL2_PROTO_VERSION, SL2_PAIR_MIN_LEN - 1, true);
}

static void minimum_length_signed_pair_request_at_capacity_reports_full() {
  Fixture f(false, {1});
  f.component.set_max_links(1);
  f.component.pair_start();
  const int sent = sends;
  const auto saved = nvs;
  pair_request(f.component, 2, SL2_PROTO_VERSION, SL2_PAIR_MIN_LEN);
  require(std::strcmp(f.component.pair_result(), "full") == 0,
          "valid minimum-length PAIR_REQ at capacity did not report full");
  require(f.component.core()->pair == SL2_PAIR_WINDOW &&
          f.component.dial_count() == 1 && sends == sent && nvs == saved,
          "valid PAIR_REQ at capacity was accepted or answered");
}

/* max_links: sizes every per-slot entity and the room-source dropdown, so a
 * Link bonded past it would work but have no row in Home Assistant. The
 * window still opens (a bonded Link may re-pair); a new Link is refused. */
static void pairing_refused_past_max_links() {
  Fixture f(false, {1});
  f.component.set_max_links(1);
  f.component.pair_start();
  require(f.component.pairing() && std::strcmp(f.component.pair_result(), "listening") == 0,
          "pairing window did not open at max_links");
  const int sent = sends;
  pair_request(f.component, 2);
  require(std::strcmp(f.component.pair_result(), "full") == 0, "new Link past max_links did not report full");
  require(f.component.dial_count() == 1 && sends == sent, "a PAIR_REQ past max_links was answered");
  tick(f.component, 60001);
  require(!f.component.pairing() && std::strcmp(f.component.pair_result(), "full") == 0,
          "the window's timeout did not keep the full verdict");
}

/* The core now opens the window on a full bond table too, and refuses a new
 * Link itself. With max_links unset or equal to the table size, the outcome
 * matches the max_links filter: a fifth Link reads "full", a bonded one
 * re-pairs. */
static void full_bond_table_refuses_new_and_repairs_bonded() {
  for (int max : {0, SL2_MAX_DIALS}) {
    Fixture f(false, {1, 2, 3, 4});
    if (max) f.component.set_max_links(max);
    uint8_t id_priv[64];
    dial_identity(1, id_priv, f.component.core()->dial[0].bond.id_pub);
    f.component.pair_start();
    require(f.component.pairing(), "pairing window did not open on a full bond table");
    const int sent = sends;
    pair_request(f.component, 5);
    require(std::strcmp(f.component.pair_result(), "full") == 0 && sends == sent &&
            f.component.dial_count() == SL2_MAX_DIALS, "a fifth Link was not refused as full");
    pair_request(f.component, 1);
    require(std::strcmp(f.component.pair_result(), "confirming") == 0,
            "a bonded Link could not re-pair on a full bond table");
  }
}

/* At the cap, the bonded Link presenting its pinned identity re-pairs. */
static void bonded_link_repairs_at_max_links() {
  Fixture f(false, {1});
  f.component.set_max_links(1);
  uint8_t id_priv[64];
  dial_identity(1, id_priv, f.component.core()->dial[0].bond.id_pub); // pinned at first pairing
  f.component.pair_start();
  pair_request(f.component, 1);
  require(std::strcmp(f.component.pair_result(), "confirming") == 0,
          "the bonded Link could not re-pair at max_links");
}

/* Outside a pairing window the core ignores PAIR_REQ, so a neighbour's Link
 * pairing elsewhere must not turn the pairing status to "full". */
static void foreign_pair_request_without_window_is_ignored() {
  Fixture f(false, {1});
  f.component.set_max_links(1);
  const std::string before = f.component.pair_result();
  pair_request(f.component, 2);
  require(f.component.pair_result() == before, "a PAIR_REQ outside a window changed the pairing status");
}

static void pairing_opens_below_max_links() {
  Fixture f(false, {1});
  f.component.set_max_links(2);
  f.component.pair_start();
  require(f.component.pairing() && std::strcmp(f.component.pair_result(), "listening") == 0,
          "pairing refused below max_links");
  pair_request(f.component, 2);
  require(std::strcmp(f.component.pair_result(), "confirming") == 0,
          "a valid PAIR_REQ below max_links was not answered");
}

static void power_save_failure_retries() {
  Fixture f;
  power_save = 1;
  fail_ps = 1;
  tick(f.component, 5000);
  require(power_save == 1, "fault injection did not block first power-save fix");
  tick(f.component, 1000);
  require(power_save == WIFI_PS_NONE, "power-save repair did not retry");
}

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"init failure retries", init_retry},
    {"PMK failure retries", pmk_retry},
    {"callback failure retries", callback_retry},
    {"bond peer failure retries", peer_retry},
    {"event registration retries", event_retry},
    {"startup retries bounded", startup_retries_are_bounded},
    {"retry clock wrap", startup_retry_wraps_clock},
    {"missing peer recovers without reset", missing_peer_recovers_without_reset},
    {"peer configuration repaired", bad_peer_configuration_recovers},
    {"silence does not reset transport", silent_link_does_not_trigger_repair},
    {"reconnect restores receiver only", reconnect_refreshes_receive_without_rekeying},
    {"driver loss preserves core session", driver_loss_recovers_without_core_restart},
    {"repair retries bounded and rearmed", repair_retries_are_bounded_and_rearmed_by_wifi},
    {"startup rearmed by STA start", startup_budget_rearms_on_wifi_start},
    {"missing event handler connection fallback", missing_wifi_handler_can_recover_on_connection_edge},
    {"repair defers during pairing", recovery_defers_while_pairing},
    {"power-save failure retries", power_save_failure_retries},
    {"connect forces power save off", reconnect_forces_power_save_off},
    {"connected power save checked once per second", connected_power_save_checked_once_per_second},
    {"runtime power-save reversion recovers during pairing", runtime_power_save_reversion_recovers_during_pairing},
    {"disconnected periodic power-save check is idle", disconnected_periodic_power_save_check_is_idle},
    {"periodic power-save clock wrap", periodic_power_save_check_wraps_clock},
    {"periodic power save preserves transport audit cadence", periodic_power_save_check_preserves_transport_audit_cadence},
    {"pairing refused past max_links", pairing_refused_past_max_links},
    {"unsigned PAIR_REQ at capacity ignored", unsigned_pair_request_at_capacity_is_ignored},
    {"old-version PAIR_REQ at capacity ignored", old_version_pair_request_at_capacity_is_ignored},
    {"short PAIR_REQ at capacity ignored", short_pair_request_at_capacity_is_ignored},
    {"minimum-length signed PAIR_REQ at capacity reports full", minimum_length_signed_pair_request_at_capacity_reports_full},
    {"bonded Link re-pairs at max_links", bonded_link_repairs_at_max_links},
    {"foreign PAIR_REQ outside a window", foreign_pair_request_without_window_is_ignored},
    {"full bond table: new refused, bonded re-pairs", full_bond_table_refuses_new_and_repairs_bonded},
    {"pairing opens below max_links", pairing_opens_below_max_links},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.run(); std::printf("adapter transport: PASS: %s\n", test.name); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "adapter transport: FAIL: %s: %s\n", test.name, error.what());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
