# Dial Diagnostics, Per-Dial Forget, and Primary-Dial Pinning — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Surface every bonded dial in Home Assistant, allow forgetting one dial without disturbing the others, and let one dial be pinned as the room-sensor source.

**Architecture:** Purely an ESPHome-adapter change. The C core already computes everything needed (`sl2_link_dial_view`, `sl2_link_forget_dial`, `sl2_link_pair_seconds_left`); the adapter has never called it. We add an optional `diagnostics:` config block whose entities are polled once per second and published only on change, four ESPHome automation actions, and one `primary_dial:` key inside the existing `link_sensor:` block that gates which dial's readings reach the measurement entities.

**Tech Stack:** ESP-IDF, ESPHome external component (Python config schema + C++ component), C11 host tests compiled with gcc.

**Specs:**
- `docs/superpowers/specs/2026-08-13-esphome-dial-diagnostics-design.md` (direction A)
- `docs/superpowers/specs/2026-08-13-esphome-primary-dial-design.md` (direction B)

## Global Constraints

- **No changes to `src/`, `include/`, or `docs/serin-link-wire-spec.md`.** If a task seems to need one, stop — it belongs to survey direction E, not this plan. This is what keeps the `vendored-copies-in-sync` CI job green without running `tools/sync_esphome.sh`.
- **Never reject a dial's room-source edit.** A dial re-sends its `want_src` edit until INFO echoes it, with no give-up rule (`serin_link.cpp:772-780`). Arbitration changes whose *reading* is used, never whether an edit is honored.
- **No runtime MAC string parsing.** MACs are validated by `cv.mac_address` in Python and emitted as byte literals. Formatting bytes → string is fine (it already exists in `publish_dial_`).
- **`SL2_MAX_DIALS` is 4.** Mirrored as a Python constant; both sides carry a comment naming the other.
- **Publish on change, never on a timer** — except `pairing_seconds` (1 Hz only while a window is open) and `last_seen` (liveness edges + a 60 s refresh).
- **Every new config key is optional**, and an existing config that sets none of them must behave byte-identically to today.
- Existing lambda API (`pair_start`, `pair_cancel`, `pairing`, `pair_result`, `dial_count`, `any_dial_live`, `forget_all_dials`) stays — no breaking changes.
- Match the codebase's comment style: explain *why*, cite observed behavior, name rejected alternatives.

---

### Task 1: Host test — bond-table compaction from the middle

The slot semantics of every diagnostics row depend on `sl2_link_forget_dial()` compacting the bond array (`src/sl2_link.c:579-590`). `test_forget()` covers removing the *first* of two and checks `sl2_link_dial_mac()`. This task covers removal from the middle of three, and — new — that `sl2_link_dial_view()` (which the diagnostics entities read, and which returns runtime state, not just the bond) follows the same shift, and that the vacated tail slot reports empty.

This is the only task with a real automated test. It runs first so the invariant is proven before code depends on it.

**Files:**
- Modify: `test/test_sl2_link.c` (add a test after `test_forget()` at line 977; register it in `main()` after `test_forget();`)

**Interfaces:**
- Consumes: existing test helpers `fresh(sl2_link_t*)`, `dial_make(fdial_t*, uint8_t tag)`, `pair_dial(sl2_link_t*, fdial_t*)`, `f_find_peer(const uint8_t[6])`, and the fake port `FPORT` / crypto `FCRYPTO` / hvac `FHVAC`.
- Produces: nothing consumed by later tasks — this is a pinning test.

- [ ] **Step 1: Write the failing test**

Insert after `test_forget()` (i.e. after line 977 of `test/test_sl2_link.c`):

```c
/* The ESPHome diagnostics rows are indexed by bond slot, and sl2_link_forget_dial
 * COMPACTS the table (src/sl2_link.c:582) — so slot N can change identity when a
 * different dial is forgotten. test_forget() pins that for removal of the first of
 * two via sl2_link_dial_mac(); this pins the middle-of-three case and, crucially,
 * that sl2_link_dial_view() follows the shift too. The view carries RUNTIME state
 * (live, model, fw), not just the persisted bond, and the diagnostics entities read
 * the view — so "the runtime half moves with the bond" is load-bearing and was
 * previously asserted nowhere. */
static void test_forget_middle_compacts(void) {
    sl2_link_t l;
    fresh(&l);
    fdial_t d1, d2, d3;
    dial_make(&d1, 0xE1);
    pair_dial(&l, &d1);
    dial_make(&d2, 0xE2);
    pair_dial(&l, &d2);
    dial_make(&d3, 0xE3);
    pair_dial(&l, &d3);
    assert(sl2_link_dial_count(&l) == 3);

    /* every dial probes, so all three have runtime state (live + last_probe_ms) */
    dial_probe(&l, &d1, 0);
    dial_probe(&l, &d2, 0);
    dial_probe(&l, &d3, 0);

    sl2_dial_view_t v;
    assert(sl2_link_dial_view(&l, 2, &v) && memcmp(v.mac, d3.mac, 6) == 0);
    assert(v.live);

    /* forget the MIDDLE one */
    assert(sl2_link_forget_dial(&l, d2.mac));
    assert(sl2_link_dial_count(&l) == 2);
    assert(!f_find_peer(d2.mac));

    /* d1 stays at 0; d3 shifts down into 1, runtime state intact */
    uint8_t mac[6];
    assert(sl2_link_dial_mac(&l, 0, mac) && memcmp(mac, d1.mac, 6) == 0);
    assert(sl2_link_dial_mac(&l, 1, mac) && memcmp(mac, d3.mac, 6) == 0);
    assert(sl2_link_dial_view(&l, 1, &v) && memcmp(v.mac, d3.mac, 6) == 0);
    assert(v.live);                       /* the runtime half moved with the bond */
    assert(v.last_seen_ms >= 0);

    /* the vacated tail slot is empty — this is what makes an unpopulated
     * diagnostics row publish "" / off / NAN rather than a stale dial */
    assert(!sl2_link_dial_view(&l, 2, &v));
    assert(!sl2_link_dial_mac(&l, 2, mac));
    assert(!sl2_link_dial_live(&l, 2));

    /* persisted: reboot decodes the same two bonds, in the same order */
    sl2_link_t l2;
    sl2_link_init(&l2, &FPORT, &FCRYPTO, &FHVAC);
    assert(sl2_link_start(&l2));
    assert(sl2_link_dial_count(&l2) == 2);
    assert(sl2_link_dial_mac(&l2, 0, mac) && memcmp(mac, d1.mac, 6) == 0);
    assert(sl2_link_dial_mac(&l2, 1, mac) && memcmp(mac, d3.mac, 6) == 0);
    printf("forget middle compacts ok\n");
}
```

Register it in `main()` immediately after the existing `test_forget();` line:

```c
    test_forget();
    test_forget_middle_compacts();
```

- [ ] **Step 2: Run the test to verify it compiles and passes or fails honestly**

Run: `test/run.sh`

Expected: the suite prints `forget middle compacts ok` and ends with `test_sl2_link: ALL OK`.

This test pins existing behavior rather than driving new code, so a pass on the first run is the expected outcome — that is the point (it is a regression guard for the diagnostics work, not a red-green cycle). If it *fails*, stop: the compaction assumption behind the whole slot design is wrong and both specs need revisiting before any adapter work.

Note `dial_probe(&l, &d, 0)` is the existing helper used by `pair_dial`; if its signature differs from `(sl2_link_t*, fdial_t*, uint8_t want)`, read it at its definition and adapt the three calls — do not change the helper.

- [ ] **Step 3: Commit**

```bash
git add test/test_sl2_link.c
git commit -m "test(link): pin bond-table compaction from the middle

The ESPHome diagnostics rows are indexed by bond slot and forget_dial
compacts the table, so slot identity shifts. test_forget covered removing
the first of two via dial_mac; this adds middle-of-three and asserts
dial_view follows the shift with its runtime half (live/model/fw) intact,
plus that the vacated tail slot reports empty."
```

---

### Task 2: C++ accessors and the `dump_config()` bond table

Adds the component-side methods the entities and actions both need, and makes the bond table visible in the log immediately — which is useful on its own, works with no YAML change at all, and gives Task 3 a way to eyeball correctness.

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h` (public methods, after the existing `forget_all_dials()` at line 95)
- Modify: `esphome/components/serin_link/serin_link.cpp` (`dump_config()` at line 1050; a MAC-format helper near the top)

**Interfaces:**
- Produces, consumed by Tasks 3, 4 and 5:
  - `bool forget_dial_slot(int idx)` — false if `idx` is out of range
  - `bool forget_dial_mac(const uint8_t mac[6])` — false if not bonded
  - `int pair_seconds_left()`
  - `bool dial_view(int idx, sl2_dial_view_t *out)` — passthrough
  - `std::string dial_mac_str(int idx)` — `""` when the slot is empty
  - free function `void sl2_fmt_mac(const uint8_t mac[6], char out[18])`

- [ ] **Step 1: Add the MAC formatting helper**

In `serin_link.cpp`, near the other file-static helpers at the top of the anonymous/static section (above `p_peer_add` at line 41 is fine), add:

```cpp
/* bytes -> "AA:BB:CC:DD:EE:FF". Extracted from publish_dial_(), which had the
 * only copy; the diagnostics rows and dump_config need the same format. */
static void sl2_fmt_mac(const uint8_t mac[6], char out[18]) {
  std::snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
```

Then replace the inline copy inside `publish_dial_()` (currently lines 848-852) so there is one implementation:

```cpp
  if (dial_mac_sensor_ != nullptr) {
    char mac[18];
    sl2_fmt_mac(dial_mac_, mac);
    if (dial_mac_sensor_->state != mac) dial_mac_sensor_->publish_state(mac);
  }
```

- [ ] **Step 2: Add the public accessors**

In `serin_link.h`, directly after `void forget_all_dials() { sl2_link_forget_all(&link_); }` (line 95):

```cpp
  /* Per-dial management. The core's bond table is COMPACTED on forget
   * (src/sl2_link.c:582), so an index identifies a bond SLOT, not a dial —
   * forgetting slot 0 shifts every later dial down one. Callers that need a
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
  /* Raw per-dial snapshot, so a lambda can reach fields the diagnostics
   * schema deliberately does not expose (model, caps_seq, cert_state). */
  bool dial_view(int idx, sl2_dial_view_t *out) {
    return sl2_link_dial_view(&link_, idx, out);
  }
  std::string dial_mac_str(int idx);
```

`dial_mac_str` is declared here and defined in the .cpp (it needs `sl2_fmt_mac`). Add to `serin_link.cpp`, next to `copy_zone_name` (line 874):

```cpp
std::string SerinLinkComponent::dial_mac_str(int idx) {
  uint8_t mac[6];
  if (!sl2_link_dial_mac(&link_, idx, mac)) return "";
  char buf[18];
  sl2_fmt_mac(mac, buf);
  return std::string(buf);
}
```

- [ ] **Step 3: Expand `dump_config()` to print the bond table**

Replace the single line at `serin_link.cpp:1050`:

```cpp
  ESP_LOGCONFIG(TAG, "  bonded dials: %d", sl2_link_dial_count(&link_));
```

with:

```cpp
  int n_dials = sl2_link_dial_count(&link_);
  ESP_LOGCONFIG(TAG, "  bonded dials: %d", n_dials);
  for (int i = 0; i < n_dials; i++) {
    sl2_dial_view_t v;
    if (!dial_view(i, &v)) continue;
    char mac[18];
    sl2_fmt_mac(v.mac, mac);
    /* model/fw stay empty until that dial's DIAL_INFO arrives (have_info). */
    ESP_LOGCONFIG(TAG, "    [%d] %s  %s  last seen %" PRId32 " s  "
                  "model '%s' fw '%s'  caps_seq %u  cert %u",
                  i, mac, v.live ? "live" : "DOWN",
                  v.last_seen_ms < 0 ? (int32_t) -1 : v.last_seen_ms / 1000,
                  v.have_info ? v.model : "", v.have_info ? v.fw : "",
                  static_cast<unsigned>(v.caps_seq),
                  static_cast<unsigned>(v.cert_state));
  }
```

- [ ] **Step 4: Verify it compiles**

There is no host harness for the C++ adapter (`test/run.sh` is pure C), so the check here is a syntax/type check, not a behavioral test. Run the C suite to confirm nothing in the core moved:

Run: `test/run.sh`
Expected: `test_sl2_link: ALL OK`

Then confirm the component still parses as valid C++ by eye against these constraints: `sl2_dial_view_t` is declared in `sl2_link.h` (vendored copy, already included), `PRId32` requires `<cinttypes>` — check the file's existing includes and add it only if `PRIu32` is not already in use there (it is used at line 1047, so the header is already present).

Full compile happens in Task 6 via `esphome compile`; do not claim this task is verified beyond the above.

- [ ] **Step 5: Commit**

```bash
git add esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "feat(esphome): per-dial accessors and a dump_config bond table

The core has tracked per-dial state all along (sl2_link_dial_view) and the
adapter never called it: HA could see a count and 'any live', not which.
Adds forget_dial_slot/forget_dial_mac/pair_seconds_left/dial_view/
dial_mac_str, and prints the whole bond table at boot. No config change —
the log line works on an unmodified YAML."
```

---

### Task 3: The `diagnostics:` block and its entities

**Files:**
- Modify: `esphome/components/serin_link/__init__.py` (constants, schema, `AUTO_LOAD`, `to_code`)
- Modify: `esphome/components/serin_link/serin_link.h` (entity pointers, per-slot publish cache, setters)
- Modify: `esphome/components/serin_link/serin_link.cpp` (`publish_diagnostics_()`, called from `loop()`)

**Interfaces:**
- Consumes from Task 2: `dial_view()`, `dial_mac_str()`, `pair_seconds_left()`, `sl2_fmt_mac()`.
- Produces, consumed by Task 6's example YAML: config keys `diagnostics:` → `bonded_count`, `pairing_status`, `pairing_seconds`, `dials: [{mac, linked, last_seen, firmware}]`.

- [ ] **Step 1: Add the Python schema**

In `__init__.py`, add `binary_sensor` to the imports and to `AUTO_LOAD`:

```python
from esphome.components import binary_sensor, climate, select, sensor, text_sensor
```

```python
AUTO_LOAD = ["binary_sensor", "climate", "select", "sensor", "text_sensor"]
```

Add these constants next to the existing `CONF_LINK_SENSOR` group:

```python
CONF_DIAGNOSTICS = "diagnostics"
CONF_BONDED_COUNT = "bonded_count"
CONF_PAIRING_STATUS = "pairing_status"
CONF_PAIRING_SECONDS = "pairing_seconds"
CONF_DIALS = "dials"
CONF_LINKED = "linked"
CONF_LAST_SEEN = "last_seen"
CONF_FIRMWARE = "firmware"

# Mirrors SL2_MAX_DIALS in include/serin_link/sl2_link.h (via the vendored
# sl2_bond.h). If that #define ever changes, change this with it — the C side
# silently ignores rows past the limit, so the error has to be raised here.
SL2_MAX_DIALS = 4
```

Add the schemas above `CONFIG_SCHEMA`:

```python
# diagnostics: — read-only visibility into the controller's bond table, all
# component-owned entities (nothing pre-existing to bind to). Every child is
# optional; the block exists purely so an unconfigured install creates no
# entities and behaves exactly as before.
#
# A `dials:` ROW IS A BOND SLOT, NOT A DIAL. sl2_link_forget_dial() compacts
# the table (src/sl2_link.c:582), so forgetting slot 0 shifts every later dial
# down one and "Dial 2 firmware" starts describing what used to be dial 3.
# Declare `mac:` on every row: it is what makes that shift visible in HA
# instead of silently relabelling. Rows past the number of bonded dials
# publish "" / off / NAN.
DIAL_ROW_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MAC_ADDRESS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LINKED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LAST_SEEN): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)

DIAGNOSTICS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BONDED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PAIRING_STATUS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PAIRING_SECONDS): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_DIALS): cv.All(
            cv.ensure_list(DIAL_ROW_SCHEMA),
            cv.Length(max=SL2_MAX_DIALS),
        ),
    }
)


def _diagnostics_schema(value):
    # `diagnostics:` with no sub-keys parses as None, not {} — same treatment
    # as link_sensor: above.
    return DIAGNOSTICS_SCHEMA(value if value is not None else {})
```

Add the imports these need to the `esphome.const` import block:

```python
    CONF_MAC_ADDRESS,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    UNIT_SECOND,
```

And register the key in `CONFIG_SCHEMA`, next to `cv.Optional(CONF_LINK_SENSOR)`:

```python
        cv.Optional(CONF_DIAGNOSTICS): _diagnostics_schema,
```

- [ ] **Step 2: Add the C++ entity storage and setters**

In `serin_link.h`, in the public section after the Task 2 accessors:

```cpp
  /* diagnostics: — component-owned read-only entities over the bond table.
   * Rows are bond SLOTS (see the compaction note above), which is why the
   * MAC entity exists at all. */
  void set_bonded_count_sensor(sensor::Sensor *s) { bonded_count_sensor_ = s; }
  void set_pairing_status_sensor(text_sensor::TextSensor *s) { pairing_status_sensor_ = s; }
  void set_pairing_seconds_sensor(sensor::Sensor *s) { pairing_seconds_sensor_ = s; }
  void add_dial_row(int idx, text_sensor::TextSensor *mac, binary_sensor::BinarySensor *linked,
                    sensor::Sensor *last_seen, text_sensor::TextSensor *firmware) {
    if (idx < 0 || idx >= SL2_MAX_DIALS) return;
    dial_rows_[idx].mac = mac;
    dial_rows_[idx].linked = linked;
    dial_rows_[idx].last_seen = last_seen;
    dial_rows_[idx].firmware = firmware;
    dial_rows_[idx].declared = true;
    diagnostics_cfg_ = true;
  }
```

Add `#include "esphome/components/binary_sensor/binary_sensor.h"` to the header's include block.

In the protected section, next to the other dial state:

```cpp
  /* Per-slot diagnostics entities plus the last value published for each, so
   * the 1 Hz walk can publish on change only (the component's standing rule)
   * without asking the entity what it last held. */
  struct DialRow {
    bool declared{false};
    text_sensor::TextSensor *mac{nullptr};
    binary_sensor::BinarySensor *linked{nullptr};
    sensor::Sensor *last_seen{nullptr};
    text_sensor::TextSensor *firmware{nullptr};
    std::string pub_mac;
    std::string pub_fw;
    bool pub_linked{false};
    bool pub_linked_valid{false};
    uint32_t last_seen_pub_ms{0};      /* millis() of the last last_seen publish */
  };
  void publish_diagnostics_(uint32_t now);
  bool diagnostics_cfg_{false};
  DialRow dial_rows_[SL2_MAX_DIALS];
  sensor::Sensor *bonded_count_sensor_{nullptr};
  text_sensor::TextSensor *pairing_status_sensor_{nullptr};
  sensor::Sensor *pairing_seconds_sensor_{nullptr};
  int pub_bonded_count_{-1};
  std::string pub_pair_result_;
  int pub_pair_seconds_{-1};
  uint32_t last_diag_ms_{0};
  bool diag_primed_{false};            /* first pass publishes everything */
```

- [ ] **Step 3: Implement the 1 Hz publish walk**

In `serin_link.cpp`, add before `dump_config()`:

```cpp
/* Diagnostics are POLLED state, unlike everything else this component
 * publishes, so they need an explicit discipline rather than inheriting the
 * publish-on-change rule by construction. Walked at 1 Hz:
 *
 *  - mac / firmware / linked / bonded_count / pairing_status: on change only.
 *    Silent in steady state.
 *  - last_seen: neither on-change nor on-new-probe works. The dial firmware
 *    probes background zones every 4 s + up to 1.8 s of stagger
 *    (SL2_DIAL_LIVE_MS's comment in sl2_link.h), so on a live dial this value
 *    sawtooths 0-6 s: on-change publishes every second, on-new-probe every
 *    ~4 s, and neither carries information. Published on a liveness EDGE
 *    (the actual signal: when a dial dropped and when it returned) and
 *    otherwise at most once per 60 s so an ongoing outage's duration stays
 *    visible.
 *  - pairing_seconds: the deliberate exception — 1 Hz while a window is open,
 *    a final 0 when it closes. ~60 states per pairing attempt, and a live
 *    countdown is the entity's whole purpose.
 *
 * The first pass publishes everything so HA never holds a diagnostics entity
 * at unknown after a restart. */
void SerinLinkComponent::publish_diagnostics_(uint32_t now) {
  const bool prime = !diag_primed_;
  diag_primed_ = true;

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
    /* 1 Hz while open (we are already on the 1 Hz tick), plus the closing 0 */
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
    if (r.linked != nullptr && (prime || live_edge)) {
      r.linked->publish_state(live);
    }
    if (r.last_seen != nullptr &&
        (prime || live_edge || now - r.last_seen_pub_ms >= 60000)) {
      r.last_seen_pub_ms = now;
      r.last_seen->publish_state(last_seen);
    }
    r.pub_linked = live;
    r.pub_linked_valid = true;
  }
}
```

Call it from `loop()`. The existing 1 Hz dial-sensor tick is gated on `link_sensor_cfg_`, so diagnostics need their own tick — add directly after that block (after line 1017):

```cpp
  if (diagnostics_cfg_ && now - last_diag_ms_ >= 1000) {
    last_diag_ms_ = now;
    publish_diagnostics_(now);
  }
```

- [ ] **Step 4: Wire it up in `to_code`**

In `__init__.py`, at the end of `to_code`, after the `CONF_LINK_SENSOR` block:

```python
    if CONF_DIAGNOSTICS in config:
        diag = config[CONF_DIAGNOSTICS]
        if CONF_BONDED_COUNT in diag:
            cg.add(var.set_bonded_count_sensor(await sensor.new_sensor(diag[CONF_BONDED_COUNT])))
        if CONF_PAIRING_STATUS in diag:
            cg.add(
                var.set_pairing_status_sensor(
                    await text_sensor.new_text_sensor(diag[CONF_PAIRING_STATUS])
                )
            )
        if CONF_PAIRING_SECONDS in diag:
            cg.add(
                var.set_pairing_seconds_sensor(
                    await sensor.new_sensor(diag[CONF_PAIRING_SECONDS])
                )
            )
        for idx, row in enumerate(diag.get(CONF_DIALS, [])):
            mac = (
                await text_sensor.new_text_sensor(row[CONF_MAC_ADDRESS])
                if CONF_MAC_ADDRESS in row
                else cg.nullptr
            )
            linked = (
                await binary_sensor.new_binary_sensor(row[CONF_LINKED])
                if CONF_LINKED in row
                else cg.nullptr
            )
            last_seen = (
                await sensor.new_sensor(row[CONF_LAST_SEEN])
                if CONF_LAST_SEEN in row
                else cg.nullptr
            )
            firmware = (
                await text_sensor.new_text_sensor(row[CONF_FIRMWARE])
                if CONF_FIRMWARE in row
                else cg.nullptr
            )
            cg.add(var.add_dial_row(idx, mac, linked, last_seen, firmware))
```

- [ ] **Step 5: Validate the schema**

Run: `esphome config esphome/example_spike.yaml` after adding a `diagnostics:` block to it (Task 6 makes this permanent; for now add it temporarily, or run against a scratch YAML).

Expected: config validates and prints the resolved `diagnostics:` block. Then check the guard rail:

Run the same with five `dials:` rows.
Expected: validation error naming the length limit.

If `esphome` is not installed, at minimum run `python3 -m py_compile esphome/components/serin_link/__init__.py` and say plainly in the commit and the final report that schema validation was not executed.

- [ ] **Step 6: Commit**

```bash
git add esphome/components/serin_link/__init__.py esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "feat(esphome): diagnostics entities for the bond table

Adds an optional diagnostics: block — bonded count, pairing status and
window countdown, and per-slot dial rows (MAC, linked, last seen, firmware).
Rows are bond SLOTS: forget_dial compacts the table, so the MAC entity is
what makes a shift visible instead of silently relabelling a row.

last_seen is published on liveness edges plus a 60 s refresh rather than on
change: dials probe every ~4 s, so on-change or on-new-probe would publish
a sawtooth that carries no information."
```

---

### Task 4: Automation actions

Replaces the raw lambdas in the example configs with real ESPHome actions, and makes per-dial forget reachable from a template button.

**Files:**
- Modify: `esphome/components/serin_link/__init__.py` (action classes + registrations)

**Interfaces:**
- Consumes from Task 2: `forget_dial_slot(int)`, `forget_dial_mac(const uint8_t[6])`; and the pre-existing `pair_start(uint32_t)`, `pair_cancel()`, `forget_all_dials()`.
- Produces: YAML actions `serin_link.pair_start`, `serin_link.pair_cancel`, `serin_link.forget_dial`, `serin_link.forget_all_dials`.

- [ ] **Step 1: Add the action machinery**

In `__init__.py`, add the imports:

```python
from esphome import automation
from esphome.core import HexInt
```

Neither `CONF_SLOT` nor `CONF_WINDOW` exists in ESPHome 2026.6.5's `const.py` (verified), so both are defined locally next to the other `CONF_` constants:

```python
CONF_SLOT = "slot"
CONF_WINDOW = "window"
```

Add after the `serin_link_ns` / class declaration:

```python
PairStartAction = serin_link_ns.class_("PairStartAction", automation.Action)
PairCancelAction = serin_link_ns.class_("PairCancelAction", automation.Action)
ForgetDialAction = serin_link_ns.class_("ForgetDialAction", automation.Action)
ForgetAllDialsAction = serin_link_ns.class_("ForgetAllDialsAction", automation.Action)
```

And the registrations, after `FINAL_VALIDATE_SCHEMA`:

```python
@automation.register_action(
    "serin_link.pair_start",
    PairStartAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SerinLinkComponent),
            cv.Optional(
                CONF_WINDOW, default="60s"
            ): cv.templatable(cv.positive_time_period_milliseconds),
        }
    ),
)
async def pair_start_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    window = config[CONF_WINDOW]
    if cg.is_template(window):
        templ = await cg.templatable(window, args, cg.uint32)
        cg.add(var.set_window(templ))
    else:
        cg.add(var.set_window(window.total_milliseconds))
    return var


@automation.register_action(
    "serin_link.pair_cancel",
    PairCancelAction,
    cv.Schema({cv.GenerateID(): cv.use_id(SerinLinkComponent)}),
)
async def pair_cancel_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def _exactly_one_target(config):
    # slot: identifies a bond SLOT and is templatable; mac: identifies a dial
    # and is resolved at compile time (cv.mac_address), which is what keeps
    # runtime MAC string parsing out of the component entirely.
    has_slot = CONF_SLOT in config
    has_mac = CONF_MAC_ADDRESS in config
    if has_slot == has_mac:
        raise cv.Invalid(
            "serin_link.forget_dial needs exactly one of `slot:` or "
            "`mac_address:` — `slot:` is a bond-table position (which shifts "
            "when another dial is forgotten), `mac_address:` is a specific dial."
        )
    return config


@automation.register_action(
    "serin_link.forget_dial",
    ForgetDialAction,
    cv.All(
        cv.Schema(
            {
                cv.GenerateID(): cv.use_id(SerinLinkComponent),
                cv.Optional(CONF_SLOT): cv.templatable(
                    cv.int_range(min=0, max=SL2_MAX_DIALS - 1)
                ),
                cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
            }
        ),
        _exactly_one_target,
    ),
)
async def forget_dial_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_SLOT in config:
        templ = await cg.templatable(config[CONF_SLOT], args, cg.int_)
        cg.add(var.set_slot(templ))
    else:
        # Same idiom as wifi's set_bssid (wifi/__init__.py:546): a Python list
        # of HexInt renders as a braced initializer, which binds to a
        # const std::array<uint8_t,6>& parameter. A C array parameter would
        # NOT — it decays to a pointer and cannot take a braced-init-list.
        cg.add(var.set_mac([HexInt(i) for i in config[CONF_MAC_ADDRESS].parts]))
    return var


@automation.register_action(
    "serin_link.forget_all_dials",
    ForgetAllDialsAction,
    cv.Schema({cv.GenerateID(): cv.use_id(SerinLinkComponent)}),
)
async def forget_all_dials_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
```

- [ ] **Step 2: Add the C++ action classes**

In `serin_link.h`, after the `SerinLinkComponent` class (before the namespace close):

```cpp
/* Automation actions. Parented rather than free functions so a config with
 * two serin_link components can address each one. */
template<typename... Ts> class PairStartAction : public Action<Ts...>,
                                                 public Parented<SerinLinkComponent> {
 public:
  TEMPLATABLE_VALUE(uint32_t, window)
  void play(Ts... x) override {
    this->parent_->pair_start(this->window_.value(x...));
  }
};

template<typename... Ts> class PairCancelAction : public Action<Ts...>,
                                                  public Parented<SerinLinkComponent> {
 public:
  void play(Ts... x) override { (void) std::make_tuple(x...); this->parent_->pair_cancel(); }
};

/* Exactly one of slot/mac is set — enforced in the config schema, so the
 * runtime just prefers the MAC when present. */
template<typename... Ts> class ForgetDialAction : public Action<Ts...>,
                                                  public Parented<SerinLinkComponent> {
 public:
  TEMPLATABLE_VALUE(int, slot)
  /* std::array, not uint8_t[6]: the codegen emits a braced initializer
     (see the note in __init__.py), which a C array parameter cannot take. */
  void set_mac(const std::array<uint8_t, 6> &mac) {
    mac_ = mac;
    has_mac_ = true;
  }
  void play(Ts... x) override {
    const bool ok = has_mac_ ? this->parent_->forget_dial_mac(mac_.data())
                             : this->parent_->forget_dial_slot(this->slot_.value(x...));
    if (!ok) ESP_LOGW("serin_link", "forget_dial: no such dial");
  }

 protected:
  std::array<uint8_t, 6> mac_{};
  bool has_mac_{false};
};

template<typename... Ts> class ForgetAllDialsAction : public Action<Ts...>,
                                                      public Parented<SerinLinkComponent> {
 public:
  void play(Ts... x) override { (void) std::make_tuple(x...); this->parent_->forget_all_dials(); }
};
```

Add to the header's includes: `#include "esphome/core/automation.h"`, `#include "esphome/core/log.h"`, `<array>`, `<cstring>`, `<tuple>`. Add `using namespace esphome;`-free explicit qualification if the file does not already open the `esphome` namespace at that point — it does (`namespace esphome { namespace serin_link {`), so `Action`, `Parented` and `TEMPLATABLE_VALUE` resolve unqualified.

- [ ] **Step 3: Validate**

Run: `esphome config` on a scratch YAML exercising each action, including the error path:

```yaml
button:
  - platform: template
    name: "Pair"
    on_press:
      - serin_link.pair_start: { window: 90s }
  - platform: template
    name: "Forget slot 0"
    on_press:
      - serin_link.forget_dial: { slot: 0 }
  - platform: template
    name: "Forget by MAC"
    on_press:
      - serin_link.forget_dial: { mac_address: "10:51:DB:8E:EB:38" }
```

Expected: validates. Then a `forget_dial:` with both `slot:` and `mac_address:` must fail with the message from `_exactly_one_target`, and one with neither must fail the same way.

- [ ] **Step 4: Commit**

```bash
git add esphome/components/serin_link/__init__.py esphome/components/serin_link/serin_link.h
git commit -m "feat(esphome): pair/forget automation actions

serin_link.pair_start/pair_cancel/forget_dial/forget_all_dials, so configs
stop reaching for raw lambdas. forget_dial takes exactly one of slot: (a
bond-table position, templatable) or mac_address: (a specific dial, resolved
at compile time by cv.mac_address) — which is what keeps runtime MAC string
parsing out of the component."
```

---

### Task 5: `primary_dial:` — pin the room source to one dial (direction B)

**Files:**
- Modify: `esphome/components/serin_link/__init__.py` (`LINK_SENSOR_SCHEMA`, `to_code`)
- Modify: `esphome/components/serin_link/serin_link.h` (`set_primary_dial`, state)
- Modify: `esphome/components/serin_link/serin_link.cpp` (`room_sensor_feed()` gate, `dump_config()`)

**Interfaces:**
- Consumes from Task 2: `sl2_fmt_mac()`.
- Produces: config key `link_sensor: primary_dial:`.

- [ ] **Step 1: Add the schema key**

In `LINK_SENSOR_SCHEMA`, add:

```python
        # primary_dial: — which bonded dial feeds the entities above. Unset
        # (the default) keeps the historical behavior: whichever dial reported
        # last owns them, which with two dials in two rooms makes the entity —
        # and the heat pump fed from it — alternate between rooms.
        #
        # Set, only this dial's readings are used. Other dials are NOT
        # rejected: their room-source EDITS are still honored, because a dial
        # re-sends an unacknowledged edit at ~3 Hz forever (wire spec §10d has
        # no give-up rule). Arbitration decides whose reading is used, never
        # whether an edit is accepted.
        #
        # The MAC is discoverable from diagnostics: dials: [].mac_address or
        # the bond table in dump_config().
        cv.Optional(CONF_PRIMARY_DIAL): cv.mac_address,
```

with the constant `CONF_PRIMARY_DIAL = "primary_dial"` beside the other `link_sensor` constants.

In `to_code`'s `CONF_LINK_SENSOR` block:

```python
        if CONF_PRIMARY_DIAL in ls:
            # list of HexInt -> braced initializer -> const std::array<uint8_t,6>&
            cg.add(
                var.set_primary_dial([HexInt(i) for i in ls[CONF_PRIMARY_DIAL].parts])
            )
```

- [ ] **Step 2: Add the C++ state and setter**

In `serin_link.h`, public, next to `set_dial_stale_after`:

```cpp
  /* primary_dial: — pin the room source to one dial. Unset = last reporting
   * dial wins (the historical behavior). */
  void set_primary_dial(const std::array<uint8_t, 6> &mac) {
    std::memcpy(primary_dial_, mac.data(), 6);
    has_primary_dial_ = true;
  }
```

Protected, next to `dial_mac_`:

```cpp
  uint8_t primary_dial_[6]{};
  bool has_primary_dial_{false};
  /* One log line per ignored dial, not per frame: non-primary DIAL_SENSOR
   * frames arrive at up to 3 Hz while a source edit is unconfirmed. */
  uint8_t ignored_logged_[SL2_MAX_DIALS][6]{};
  uint8_t n_ignored_logged_{0};
```

- [ ] **Step 3: Add the gate in `room_sensor_feed()`**

In `serin_link.cpp`, inside `room_sensor_feed()`, immediately **after** the `is_edit` block that updates `selected_src_` (currently ending at line 786) and **before** the `dial_has_sensor_` update at line 792:

```cpp
  /* primary_dial: — a non-primary dial's READING is ignored, its EDIT is not.
   * Placed after the is_edit branch deliberately: rejecting the edit would
   * spin that dial at ~3 Hz forever (§10d has no give-up rule), so the
   * selection is always honored and only the measurement is gated. Everything
   * downstream is then correct by construction, because dial_temp_ms_ only
   * ever advances from the primary — so room_src_status_() reports STALE /
   * UNAVAILABLE when the primary dies even while another dial chatters. */
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
      ESP_LOGI(TAG, "ignoring room sensor from %s: primary_dial is %s", got, want);
    }
    return;
  }
```

- [ ] **Step 4: Report it in `dump_config()`**

Extend the existing `link_sensor_cfg_` line (`serin_link.cpp:1046-1049`):

```cpp
  if (link_sensor_cfg_) {
    ESP_LOGCONFIG(TAG, "  dial room sensor: accepted, stale after %" PRIu32
                  " ms, source=%u", dial_stale_ms_,
                  static_cast<unsigned>(selected_src_));
    if (has_primary_dial_) {
      char mac[18];
      sl2_fmt_mac(primary_dial_, mac);
      ESP_LOGCONFIG(TAG, "    primary dial: %s (others ignored for measurement)", mac);
    } else {
      ESP_LOGCONFIG(TAG, "    primary dial: unset (last reporting dial wins)");
    }
  }
```

- [ ] **Step 5: Validate**

Run: `esphome config` on a scratch YAML with `link_sensor: primary_dial: "10:51:DB:8E:EB:38"`.
Expected: validates, MAC normalized.

Run with `primary_dial: "not-a-mac"`.
Expected: `cv.mac_address` rejects it.

- [ ] **Step 6: Commit**

```bash
git add esphome/components/serin_link/__init__.py esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "feat(esphome): primary_dial pins the room source to one dial

With two dials bonded, the room-sensor entity went to whichever reported
last — so the entity, and the heat pump fed from it, alternated between
rooms. primary_dial: pins it; unset keeps the old behavior exactly.

A non-primary dial's reading is ignored, its source EDIT is not: a dial
re-sends an unacknowledged edit at ~3 Hz with no give-up rule, so refusing
one would spin it forever."
```

---

### Task 6: Examples, README, and end-to-end verification

**Files:**
- Modify: `esphome/example_spike.yaml` (the config CI compiles — this is what gives the new schema compile coverage)
- Modify: `esphome/example_cn105.yaml` (replace lambdas with actions; document the new keys)
- Modify: `README.md`

- [ ] **Step 1: Add diagnostics to the CI-compiled example**

In `esphome/example_spike.yaml`, inside the `serin_link:` block:

```yaml
  # Visibility into the bond table. Rows are bond SLOTS, not dials: forgetting
  # one dial shifts the later ones down, which is why every row declares
  # mac_address — it makes the shift visible instead of silently relabelling.
  diagnostics:
    bonded_count:    { name: "Bonded dials" }
    pairing_status:  { name: "Pairing" }
    pairing_seconds: { name: "Pairing window" }
    dials:
      - mac_address: { name: "Dial 1 MAC" }
        linked:      { name: "Dial 1 linked" }
        last_seen:   { name: "Dial 1 last seen" }
        firmware:    { name: "Dial 1 firmware" }
      - mac_address: { name: "Dial 2 MAC" }
        linked:      { name: "Dial 2 linked" }
        last_seen:   { name: "Dial 2 last seen" }
        firmware:    { name: "Dial 2 firmware" }
```

- [ ] **Step 2: Convert `example_cn105.yaml` to the actions**

Replace the two existing template buttons (lines 150-157) with:

```yaml
button:
  - platform: template
    name: "Pair Serin Dial"
    on_press:
      - serin_link.pair_start: { window: 60s }

  - platform: template
    name: "Forget Dial 1"
    on_press:
      - serin_link.forget_dial: { slot: 0 }

  - platform: template
    name: "Forget All Dials"
    on_press:
      - serin_link.forget_all_dials:
```

And in its `link_sensor:` block, add the commented key:

```yaml
    # With more than one dial bonded, pin which one feeds the entities above —
    # otherwise whichever reported last wins and the value (and the heat pump
    # fed from it) alternates between rooms. Get the MAC from the diagnostics
    # entities or the bond table in the boot log.
    # primary_dial: "10:51:DB:8E:EB:38"
```

- [ ] **Step 3: Document in README.md**

Find the section documenting `link_sensor:` and add `primary_dial:` to it, plus a new subsection for `diagnostics:` and the actions. Cover, in prose: that a row is a bond slot and why `mac_address:` matters; that a pinned dial going offline takes the room source down with it *by design* (substituting another room silently is the bug being fixed); and that the MAC survives re-pairing the same physical dial but not swapping in a different one.

- [ ] **Step 4: Full verification**

Run each and paste real output into the final report — do not summarize a command you did not run:

```bash
test/run.sh
esphome config esphome/example_spike.yaml
esphome config esphome/example_cn105.yaml
esphome compile esphome/example_spike.yaml
```

`esphome compile` is the only step that proves the C++ actually builds; the host suite does not compile the adapter. If the toolchain is unavailable locally, say so explicitly rather than implying the code was compiled — CI runs this job on push.

- [ ] **Step 5: Commit**

```bash
git add esphome/example_spike.yaml esphome/example_cn105.yaml README.md
git commit -m "docs(esphome): document diagnostics, actions, and primary_dial

example_spike.yaml carries a diagnostics block so the CI compile job covers
the new schema and every entity platform in it."
```

---

## Verification Summary

| what | how | covered by |
|---|---|---|
| bond-table compaction, `dial_view` after a shift | `test/run.sh` | Task 1 (real automated test) |
| config schema, both error paths | `esphome config` | Tasks 3, 4, 5 |
| C++ builds | `esphome compile esphome/example_spike.yaml` | Task 6 + CI |
| publish gating, the `primary_dial` gate | **hardware only** — no host harness exists for the C++ adapter | the bench checklists in both specs |

The last row is the honest gap: `test/run.sh` compiles pure C, so nothing automated exercises `publish_diagnostics_()` or the `room_sensor_feed()` gate. Both specs carry a bench checklist for two bonded dials; run it before calling this feature done on hardware.
