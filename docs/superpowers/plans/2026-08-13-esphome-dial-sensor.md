# Dial Room Sensor on ESPHome — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a Serin Link dial's built-in temperature/humidity sensor visible in Home Assistant, and let the dial select itself as the controller's room-temperature source, when the controller is an ESPHome device running the `serin_link` component.

**Architecture:** The dial and the platform-free C core already do their halves — the dial streams `DIAL_SENSOR`, the core decodes it and dispatches to an optional `hvac->room_sensor` hook. This plan fills in the ESPHome adapter: install the hook, publish the reading through owned ESPHome sensor entities, persist the dial's room-source choice, and confirm it back over the wire via a new `SL2_TLV_ROOM_SRC` helper. Actuation (feeding the value to the heat pump) stays in YAML because ESPHome's `Climate` base class has no generic external-room-temperature input.

**Tech Stack:** C11 (core headers, host tests under plain gcc), C++17 (ESPHome component), Python (ESPHome codegen/config validation), ESP-IDF framework, ESP-NOW.

**Spec:** `docs/superpowers/specs/2026-08-13-esphome-dial-sensor-design.md` — read it before starting. Every design decision below is argued there, in particular §3's deliberate deviation from the reference controller.

## Global Constraints

- **Repo:** all work happens in `serin-link-core`. **No change to the dial firmware** (`~/serin-link/`) is in scope.
- **Vendored copies are generated.** `esphome/components/serin_link/sl2_*.{h,c}` are flattened copies of `include/serin_link/` + `src/`. Never edit them by hand: edit the canonical file, run `tools/sync_esphome.sh`, commit both. CI fails on drift. (`CONTRIBUTING.md`)
- **`serin_link.{h,cpp}` and `__init__.py` are NOT generated.** They are the component's own source and exist only in `esphome/components/serin_link/`. Editing them directly is correct.
- **No wire-format change.** `SL2_PKT_DIAL_SENSOR`, `SL2_TLV_ROOM_SRC` (`0x0B`), `SL2_FEAT_LINK_SENSOR` (`1u<<10`), `enum sl2_room_src`, `enum sl2_room_status` all already ship in `SL2_PROTO_VERSION` 2. `sl2_proto.h` must not be touched — it is byte-identical across the dial, core and controller trees.
- **Temperatures on the wire are deci-Celsius.** `SL2_DC_NA` (`0x7FFF`) means no reading; `SL2_HUM_NA` (`0xFF`) means no humidity. °F never crosses the wire.
- **Host tests build with `-std=c11 -Wall -Wextra -Werror`.** New code in `sl2_info.h` must be warning-clean under those flags.
- **Opt-in is `link_sensor:` block presence.** A config without that block must advertise no new capability and behave exactly as it does today.
- **Product naming in user-facing strings:** the dial is a "Serin Link", the heat-pump unit is a "Serin Controller". This applies to example YAML entity names and documentation prose, not to code identifiers (`dial_temp_sensor_`, `dial_mac`, etc., which stay as-is).

---

### Task 1: `SL2_TLV_ROOM_SRC` builder helper

Adds the typed TLV writer the component will use in Task 3, with host-test coverage. Self-contained: nothing else depends on it yet, and `test/run.sh` proves it.

**Files:**
- Modify: `include/serin_link/sl2_info.h` (append after `sl2_info_put_energy`, before the `COMPRESSOR value tables` comment block)
- Modify: `test/test_sl2_info.c`
- Generated: `esphome/components/serin_link/sl2_info.h` (via `tools/sync_esphome.sh`)

**Interfaces:**
- Consumes: `sl2_tlv_put()` from `sl2_proto.h`; `SL2_TLV_ROOM_SRC`, `enum sl2_room_src`, `enum sl2_room_status`, all already defined there.
- Produces: `bool sl2_info_put_room_src(uint8_t *buf, size_t cap, size_t *off, uint8_t applied_src, uint8_t status)` — returns `true` if the whole 4-byte TLV was appended, `false` (leaving `*off` untouched) if it did not fit. Task 3 calls it.

- [ ] **Step 1: Write the failing test**

Add to `test/test_sl2_info.c`, immediately after `test_energy()`:

```c
static void test_room_src(void) {
    uint8_t buf[16];
    size_t off = 0;
    assert(sl2_info_put_room_src(buf, sizeof buf, &off,
                                 SL2_ROOMSRC_LINK, SL2_ROOMST_STALE));
    const uint8_t *v = expect_tlv(buf, off, SL2_TLV_ROOM_SRC, 2);
    assert(v[0] == SL2_ROOMSRC_LINK);
    assert(v[1] == SL2_ROOMST_STALE);

    /* applied_src names the SELECTED source even when the feed is dead —
     * a reader that renders only applied_src shows a dead feed as a live
     * one (wire spec §10d), so both bytes must survive the round trip. */
    off = 0;
    assert(sl2_info_put_room_src(buf, sizeof buf, &off,
                                 SL2_ROOMSRC_INTERNAL, SL2_ROOMST_OK));
    v = expect_tlv(buf, off, SL2_TLV_ROOM_SRC, 2);
    assert(v[0] == SL2_ROOMSRC_INTERNAL);
    assert(v[1] == SL2_ROOMST_OK);
}
```

Extend the existing `test_bounds_whole_tlv_or_nothing()` so the new helper is held to the same contract as its nine siblings. Replace that function's body with:

```c
static void test_bounds_whole_tlv_or_nothing(void) {
    uint8_t buf[8];                          /* too small for WIFI_INFO */
    size_t off = 0;
    assert(!sl2_info_put_wifi(buf, sizeof buf, &off, -61, 6, "MyNet", "10.0.0.7"));
    assert(off == 0);                        /* nothing written */
    /* a small TLV still fits after a failed big one */
    assert(sl2_info_put_batt(buf, sizeof buf, &off, 50));
    assert(off == 3);
    /* ROOM_SRC is 4 bytes on the wire (2 header + 2 value): one fits in the
     * remaining 5, a second does not, and the failure writes nothing. */
    assert(sl2_info_put_room_src(buf, sizeof buf, &off,
                                 SL2_ROOMSRC_LINK, SL2_ROOMST_OK));
    assert(off == 7);
    assert(!sl2_info_put_room_src(buf, sizeof buf, &off,
                                  SL2_ROOMSRC_LINK, SL2_ROOMST_OK));
    assert(off == 7);
}
```

Register the new case in `main()`, after the `test_energy();` line:

```c
    test_room_src();
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ~/serin-link-core && test/run.sh
```

Expected: FAIL — the compile of `test_sl2_info.c` errors with `implicit declaration of function 'sl2_info_put_room_src'` (and under `-Werror`, that is fatal). If instead it *passes*, stop: the helper already exists and this task is a no-op.

- [ ] **Step 3: Write the implementation**

In `include/serin_link/sl2_info.h`, directly after the closing brace of `sl2_info_put_energy()` and before the `/* ── COMPRESSOR value tables ... */` comment:

```c
/* 0x0B ROOM_SRC: u8 applied_src (enum sl2_room_src); u8 status
 * (enum sl2_room_status). applied_src names the SELECTED source even when
 * status is STALE — a reader that renders only applied_src shows a dead feed
 * as a live one (wire spec §10d). */
static inline bool sl2_info_put_room_src(uint8_t *buf, size_t cap, size_t *off,
                                         uint8_t applied_src, uint8_t status) {
    uint8_t v[2] = { applied_src, status };
    return sl2_tlv_put(buf, cap, off, SL2_TLV_ROOM_SRC, v, 2);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cd ~/serin-link-core && test/run.sh
```

Expected: PASS, ending with `test_sl2_info: all tests passed` and the other three suites also passing.

- [ ] **Step 5: Re-vendor the flattened copy**

```bash
cd ~/serin-link-core && tools/sync_esphome.sh
git diff --stat esphome/components/serin_link/
```

Expected: `sync_esphome.sh` prints `synced sl2 core -> .../esphome/components/serin_link`, and `git diff --stat` shows `esphome/components/serin_link/sl2_info.h` changed (only that file — if `sl2_link.c` or others also changed, the canonical tree had uncommitted drift; stop and investigate before committing).

Do **not** propagate to `~/mitsubishi-cn105-homekit/main/` here. That repo vendors the core too, but syncing it is a separate change to a separate repo; this repo's CI only checks its own copy.

- [ ] **Step 6: Commit**

```bash
cd ~/serin-link-core
git add include/serin_link/sl2_info.h esphome/components/serin_link/sl2_info.h test/test_sl2_info.c
git commit -m "feat(info): typed builder for the ROOM_SRC TLV

The reference controller writes 0x0B with a raw sl2_tlv_put; a wrapper
matches the other nine put_* helpers and gives the host suite something
to hold to the whole-TLV-or-nothing contract."
```

---

### Task 2: Receive the reading and publish it to Home Assistant

Installs the core hook, adds the `link_sensor:` YAML block, and publishes the dial's temperature/humidity as ESPHome entities. **Deliberately does not set the capability bit** — so at the end of this task the dial still does not transmit and the entities stay empty. That is the correct intermediate state: Task 3 turns the feature on all at once, and nothing half-live ever reaches the wire.

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h`
- Modify: `esphome/components/serin_link/serin_link.cpp`
- Modify: `esphome/components/serin_link/__init__.py`

**Interfaces:**
- Consumes: `sl2_hvac_iface_t::room_sensor` (declared in `sl2_link.h`): `void (*)(void *ctx, const uint8_t src_mac[6], const struct sl2_dial_sensor_pkt *p, bool is_edit)`. The core has already length-checked the frame; `is_edit` is true only when `want_src` was present **and** not `SL2_ROOMSRC_NOEDIT`. Never re-derive that from the packet.
- Produces, for Task 3:
  - `bool link_sensor_cfg_` — true when the YAML block is present.
  - `uint32_t dial_temp_ms_` — `millis()` of the last frame carrying a valid temperature; `0` = never.
  - `uint32_t dial_stale_ms_` — the configured staleness window.
  - `void SerinLinkComponent::room_sensor_feed(const uint8_t src_mac[6], const struct sl2_dial_sensor_pkt *p, bool is_edit)` — Task 3 adds the `is_edit` branch to its body.

- [ ] **Step 1: Add the state and setters to the header**

In `esphome/components/serin_link/serin_link.h`, after the `set_energy_sensor` line in the telemetry-bindings block, add:

```cpp
  /* link_sensor: — the dial's OWN room sensor (DIAL_SENSOR, wire spec §10d).
   * Unlike the telemetry bindings above (which point at entities the user
   * already has), these entities are owned by this component: the dial is
   * the data source, so there is nothing pre-existing to bind to. Presence
   * of the YAML block is the opt-in and gates SL2_FEAT_LINK_SENSOR. */
  void set_link_sensor_enabled() { link_sensor_cfg_ = true; }
  void set_dial_temp_sensor(sensor::Sensor *s) { dial_temp_sensor_ = s; }
  void set_dial_hum_sensor(sensor::Sensor *s) { dial_hum_sensor_ = s; }
  void set_dial_mac_sensor(text_sensor::TextSensor *s) { dial_mac_sensor_ = s; }
  void set_dial_stale_after(uint32_t ms) { dial_stale_ms_ = ms; }

  /* A bonded dial reported its own room sensor (called from the trampoline). */
  void room_sensor_feed(const uint8_t src_mac[6],
                        const struct sl2_dial_sensor_pkt *p, bool is_edit);
```

In the `protected:` section, after the `batt_low_latch_` member, add:

```cpp
  /* Publish the dial reading to HA. stale=true publishes NAN (HA renders it
   * as unknown rather than a frozen number) and latches until a fresh
   * reading arrives. */
  void publish_dial_(bool stale);
  bool link_sensor_cfg_{false};
  sensor::Sensor *dial_temp_sensor_{nullptr};
  sensor::Sensor *dial_hum_sensor_{nullptr};
  text_sensor::TextSensor *dial_mac_sensor_{nullptr};
  uint32_t dial_stale_ms_{90000};
  int16_t dial_temp_dc_{SL2_DC_NA};
  uint8_t dial_hum_pct_{SL2_HUM_NA};
  /* millis() of the last frame carrying a VALID temperature; 0 = never.
   * Freshness follows the TEMPERATURE, not the frame: a dial with sensing
   * hardware but no reading yet must not hold off the stale watchdog. */
  uint32_t dial_temp_ms_{0};
  bool dial_has_sensor_{false};
  uint8_t dial_mac_[6]{};
  int16_t dial_pub_dc_{SL2_DC_NA};   /* last value published to HA */
  uint32_t dial_pub_ms_{0};          /* millis() of that publish; 0 = never */
  bool dial_stale_{false};           /* NAN already published */
  uint32_t last_dial_check_ms_{0};
```

- [ ] **Step 2: Implement the feed and publish logic**

In `esphome/components/serin_link/serin_link.cpp`, add both functions immediately before the `/* trampolines: sl2 C hooks -> the component */` comment:

```cpp
void SerinLinkComponent::room_sensor_feed(const uint8_t src_mac[6],
                                          const struct sl2_dial_sensor_pkt *p,
                                          bool is_edit) {
  (void) is_edit;   /* source edits: see the ROOM_SRC handling (added next) */
  const bool got_temp = p->temp_dc != SL2_DC_NA;
  dial_has_sensor_ = (p->flags & SL2_DSF_HAS_SENSOR) != 0;
  if (src_mac != nullptr) memcpy(dial_mac_, src_mac, 6);
  if (p->hum_pct != SL2_HUM_NA) dial_hum_pct_ = p->hum_pct;
  if (!got_temp) return;
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
    std::snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
                  dial_mac_[0], dial_mac_[1], dial_mac_[2],
                  dial_mac_[3], dial_mac_[4], dial_mac_[5]);
    if (dial_mac_sensor_->state != mac) dial_mac_sensor_->publish_state(mac);
  }
}
```

- [ ] **Step 3: Add the trampoline and install the hook**

In the trampoline block, after `t_tlvs`:

```cpp
static void t_room_sensor(void *ctx, const uint8_t src_mac[6],
                          const struct sl2_dial_sensor_pkt *p, bool is_edit) {
  static_cast<SerinLinkComponent *>(ctx)->room_sensor_feed(src_mac, p, is_edit);
}
```

In `setup()`, after the `hvac_.fill_info_tlvs = t_tlvs;` line:

```cpp
  /* Installed unconditionally: without SL2_FEAT_LINK_SENSOR in CAPS the dial
   * never sends DIAL_SENSOR at all, so an unconfigured node simply never
   * calls this — no need for a second gate here. */
  hvac_.room_sensor = t_room_sensor;
```

- [ ] **Step 4: Add the staleness check to `loop()`**

In `SerinLinkComponent::loop()`, after the closing brace of the Wi-Fi power-save block and before `sl2_rxq_frame_t f;`:

```cpp
  /* Dial room sensor: 1 Hz stale edge. Frame-driven publishing lives in
   * room_sensor_feed(); this is the one publish path that fires on the
   * ABSENCE of frames, so it cannot live there. */
  if (link_sensor_cfg_ && now - last_dial_check_ms_ >= 1000) {
    last_dial_check_ms_ = now;
    if (!dial_stale_ && dial_temp_ms_ != 0 &&
        now - dial_temp_ms_ >= dial_stale_ms_) {
      ESP_LOGW(TAG, "dial room sensor stale (%" PRIu32 " ms) — publishing unknown",
               now - dial_temp_ms_);
      publish_dial_(true);
    }
  }
```

- [ ] **Step 5: Add the `dump_config()` line**

In `SerinLinkComponent::dump_config()`, after the `telemetry:` line:

```cpp
  if (link_sensor_cfg_)
    ESP_LOGCONFIG(TAG, "  dial room sensor: accepted, stale after %" PRIu32 " ms",
                  dial_stale_ms_);
```

- [ ] **Step 6: Add the YAML schema and codegen**

In `esphome/components/serin_link/__init__.py`, extend the `esphome.const` import to:

```python
from esphome.const import (
    CONF_HUMIDITY,
    CONF_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)
```

After the `CONF_ENERGY_SENSOR` definition, add:

```python
CONF_LINK_SENSOR = "link_sensor"
CONF_DIAL_MAC = "dial_mac"
CONF_STALE_AFTER = "stale_after"

# link_sensor: — entities fed by the DIAL's own sensor, owned by this
# component (the bindings above point at entities the user already has; here
# the dial is the data source, so there is nothing to bind to). Presence of
# the block is the opt-in and is what sets SL2_FEAT_LINK_SENSOR: without it
# the dial never transmits and its Settings never grow a room-source cycle,
# so an existing config upgrades with no behavior change. Every child is
# optional — `link_sensor: {}` means "accept the dial as a room source,
# create no HA entities".
LINK_SENSOR_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Which dial is currently feeding. Diagnostic: it only matters when
        # two dials bonded to one controller both offer a sensor, where the
        # value alternates between them (last reporting dial wins).
        cv.Optional(CONF_DIAL_MAC): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # 90s = three missed 20s dial keepalives plus slack.
        cv.Optional(
            CONF_STALE_AFTER, default="90s"
        ): cv.positive_time_period_milliseconds,
    }
)
```

Inside `CONFIG_SCHEMA`, after the `CONF_ENERGY_SENSOR` entry:

```python
        cv.Optional(CONF_LINK_SENSOR): LINK_SENSOR_SCHEMA,
```

In `to_code()`, after the `set_cmd_debounce` line:

```python
    if CONF_LINK_SENSOR in config:
        ls = config[CONF_LINK_SENSOR]
        cg.add(var.set_link_sensor_enabled())
        cg.add(var.set_dial_stale_after(ls[CONF_STALE_AFTER].total_milliseconds))
        if CONF_TEMPERATURE in ls:
            cg.add(var.set_dial_temp_sensor(await sensor.new_sensor(ls[CONF_TEMPERATURE])))
        if CONF_HUMIDITY in ls:
            cg.add(var.set_dial_hum_sensor(await sensor.new_sensor(ls[CONF_HUMIDITY])))
        if CONF_DIAL_MAC in ls:
            cg.add(
                var.set_dial_mac_sensor(
                    await text_sensor.new_text_sensor(ls[CONF_DIAL_MAC])
                )
            )
```

- [ ] **Step 7: Verify the host suite still passes**

```bash
cd ~/serin-link-core && test/run.sh
```

Expected: PASS. This task touches no core C, so this is a regression check only — it proves the Task 1 vendored header is still consistent.

- [ ] **Step 8: Verify it compiles under ESPHome**

ESPHome is **not installed** in this environment, so this step needs a one-time install (it needs network access):

```bash
cd ~/serin-link-core
python3 -m venv /tmp/esphome-venv && /tmp/esphome-venv/bin/pip install -q esphome
/tmp/esphome-venv/bin/esphome config esphome/example_cn105.yaml
```

Expected from `esphome config`: the YAML validates and the expanded config is printed. This catches schema errors (bad `cv.` names, wrong `esphome.const` imports) in seconds without a full toolchain download.

Then the real compile, which also exercises the C++:

```bash
/tmp/esphome-venv/bin/esphome compile esphome/example_cn105.yaml
```

Expected: `INFO Successful cleanup` / a built binary. First run downloads the IDF toolchain and takes several minutes.

**If ESPHome cannot be installed** (no network), do not fake this. Record in the commit message that the compile was NOT run, and flag it when reporting the task complete — the hardware pass in Task 3 then becomes the first real compile.

- [ ] **Step 9: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/serin_link.h \
        esphome/components/serin_link/serin_link.cpp \
        esphome/components/serin_link/__init__.py
git commit -m "feat(esphome): publish the dial's room sensor to Home Assistant

Install the core's room_sensor hook and expose the dial's temperature and
humidity as owned ESPHome entities under an opt-in link_sensor: block.

Freshness follows the temperature, not the frame: a dial with sensing
hardware but no reading yet must not hold off the stale watchdog.
Publishing is frame-driven with a 30s heartbeat and value dedup, because
an unconfirmed source edit makes the dial re-send at ~3 Hz.

The capability bit is deliberately NOT set yet, so the dial does not
transmit and the feature stays dark until the ROOM_SRC half lands."
```

---

### Task 3: Room-source selection, the `ROOM_SRC` TLV, and the capability bit

Turns the feature on. Persists the dial's source choice, reports it back with an honest status, and sets `SL2_FEAT_LINK_SENSOR` so the dial starts transmitting.

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h`
- Modify: `esphome/components/serin_link/serin_link.cpp`

**Interfaces:**
- Consumes: `sl2_info_put_room_src()` (Task 1); `link_sensor_cfg_`, `dial_temp_ms_`, `dial_stale_ms_`, `room_sensor_feed()` (Task 2).
- Produces: `bool SerinLinkComponent::room_src_is_link() const` — the public getter YAML automations call to gate actuation (`id(serin).room_src_is_link()` — the component id is `serin` in both examples, never `link`, which collides with libc `link()`). Task 4 documents it.

- [ ] **Step 1: Add the selection state to the header**

In `esphome/components/serin_link/serin_link.h`, in the public section next to `pair_start` / `dial_count` (the other lambda-facing accessors):

```cpp
  /* For actuation automations in YAML: only feed the heat pump when the
   * dial actually selected itself as the room source. */
  bool room_src_is_link() const { return selected_src_ == SL2_ROOMSRC_LINK; }
```

In the `protected:` section, after `last_dial_check_ms_`:

```cpp
  /* Health of the SELECTED room source, for the ROOM_SRC TLV. */
  uint8_t room_src_status_() const;
  uint8_t selected_src_{SL2_ROOMSRC_INTERNAL};
  ESPPreferenceObject room_src_pref_;
```

- [ ] **Step 2: Accept and persist the dial's edit**

In `room_sensor_feed()`, replace the `(void) is_edit;` line with the block
below. **Its position matters**: it sits above the `if (!got_temp) return;`
early exit, because a source edit must be honored whether or not that same
frame carried a usable reading — a dial with no sensor selecting `Internal`
sends exactly such a frame.

```cpp
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
  if (is_edit && (p->want_src == SL2_ROOMSRC_INTERNAL ||
                  p->want_src == SL2_ROOMSRC_BLE ||
                  p->want_src == SL2_ROOMSRC_LINK) &&
      selected_src_ != p->want_src) {
    selected_src_ = p->want_src;
    room_src_pref_.save(&selected_src_);
    ESP_LOGI(TAG, "room source -> %u (set from dial)",
             static_cast<unsigned>(selected_src_));
  }
```

- [ ] **Step 3: Implement the status**

Add immediately before `SerinLinkComponent::room_sensor_feed` in the `.cpp`:

```cpp
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
```

- [ ] **Step 4: Emit the TLV**

In `fill_info_tlvs()`, immediately before the closing `return off;`:

```cpp
  /* Feature bit = capability, TLV presence = current validity (spec §9): a
   * node that did not opt in emits neither. */
  if (link_sensor_cfg_)
    sl2_info_put_room_src(buf, cap, &off, selected_src_, room_src_status_());
```

- [ ] **Step 5: Set the capability bit**

In `hvac_get_caps()`, after the `battery_sensor_` line:

```cpp
  if (link_sensor_cfg_) out->features |= SL2_FEAT_LINK_SENSOR;
```

No fingerprint work is needed: `setup()` already runs an FNV-1a over the whole `sl2_caps_pkt`, so the new bit changes the fingerprint and `sl2_link_caps_changed()` announces it to bonded dials on the first boot after the YAML edit.

- [ ] **Step 6: Load the persisted selection**

In `setup()`, after the two `use_f_pref_` lines:

```cpp
  room_src_pref_ = global_preferences->make_preference<uint8_t>(0x53325253 /* 'S2RS' */);
  if (!room_src_pref_.load(&selected_src_)) selected_src_ = SL2_ROOMSRC_INTERNAL;
```

The key must not collide with the existing `0x53324C55` ('S2LU') or `0x53324346` ('S2CF').

- [ ] **Step 7: Extend `dump_config()`**

Replace the `dial room sensor:` line added in Task 2 with:

```cpp
  if (link_sensor_cfg_)
    ESP_LOGCONFIG(TAG, "  dial room sensor: accepted, stale after %" PRIu32
                  " ms, source=%u", dial_stale_ms_,
                  static_cast<unsigned>(selected_src_));
```

- [ ] **Step 8: Verify it compiles**

```bash
cd ~/serin-link-core && /tmp/esphome-venv/bin/esphome compile esphome/example_cn105.yaml
```

Expected: builds clean. Also compile a config *without* the block, to prove the opt-in path is intact:

```bash
/tmp/esphome-venv/bin/esphome compile esphome/example_generic.yaml
```

Expected: builds clean (that example has no `link_sensor:` yet — Task 4 adds one).

Same honesty rule as Task 2 Step 8 if ESPHome is unavailable.

- [ ] **Step 9: Verify on hardware**

The rig is an AtomS3 Lite plus a bonded dial — the same setup that verified commit `ea9f2be`. Re-read the dial-side send path first (`maybe_send_dial_sensor` and the `wantSrc` clear in `apply_info_tlvs`, both in `~/serin-link/main/espnow_client.c`); that file is under active edit and this design depends on its behavior.

1. Flash the AtomS3 with a `link_sensor:` config. The dial's temperature appears in HA within ~20 s and updates on a 0.5 °C change.
2. Power the dial down. After ~90 s the HA entity goes unknown; the log shows `dial room sensor stale`. Power it back up — the value returns.
3. On the dial: Settings → System, press to cycle Internal → Link. The dial's own display confirms the source (that only happens once `INFO` carries the matching `ROOM_SRC`). Reboot the AtomS3; the selection survives.
4. Wire the cn105 `remote_temperature` automation from the design doc §6 and confirm the heat pump acts on the dial's reading with Link selected, and stops when cycled back to Internal.
5. With `battery_sensor:` bound, cycle the dial to `Sensor`. It must read as unavailable on the dial, and the `DIAL_SENSOR` log rate must **drop back to the 20 s keepalive** rather than sitting at ~3 Hz. This is the §3 deviation; if the rate stays high, the edit is not being accepted and the branch in Step 2 is wrong.

- [ ] **Step 10: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/serin_link.h \
        esphome/components/serin_link/serin_link.cpp
git commit -m "feat(esphome): accept the dial as a room source

Persist the dial's want_src edit, report it back via the ROOM_SRC TLV with
a status computed from the feed's own freshness, and declare
SL2_FEAT_LINK_SENSOR so the dial starts transmitting at all.

BLE is accepted as a selection and reported UNAVAILABLE rather than
rejected: the dial offers it whenever battery_sensor: is bound, and it
re-sends an unconfirmed edit at ~3 Hz forever, so rejection is a retry
storm where UNAVAILABLE is the honest answer."
```

State in the commit message which verification steps actually ran.

---

### Task 4: Examples and documentation

The recipes are the feature for anyone who did not write it. Without them a user gets a number in HA and no idea how to make the heat pump use it.

**Files:**
- Modify: `esphome/example_cn105.yaml`
- Modify: `esphome/example_generic.yaml`
- Modify: `README.md`
- Modify: `docs/serin-link-wire-spec.md` (§11 inventory note)

**Interfaces:**
- Consumes: the `link_sensor:` schema (Task 2) and `room_src_is_link()` (Task 3). Every YAML key and lambda used here must match those exactly.

- [ ] **Step 1: Add the cn105 recipe**

In `esphome/example_cn105.yaml`, extend the `serin_link:` block with:

```yaml
  # The dial's own SHT4x, published to Home Assistant and offered to the heat
  # pump as a room-temperature source. Adding this block is what makes the
  # dial transmit at all, and what puts the source cycle in its Settings.
  link_sensor:
    temperature:
      id: dial_temp
      name: "Serin Link Temperature"
      # Feed the heat pump ONLY while the dial is the selected source —
      # cycling back to Internal on the dial must actually hand control back.
      on_value:
        - if:
            condition:
              lambda: 'return id(serin).room_src_is_link();'
            then:
              - lambda: 'id(hvac).set_remote_temperature(x);'
    humidity:
      name: "Serin Link Humidity"
```

The ids above are this file's actual ids: the component is `serin` and the climate entity is `hvac`. Note the existing comment on that `id:` line — **`id: link` collides with libc `link()` in the generated code**, which is why the component is not called `link` here and must not be in any new example.

Then, on the `climate:` entry:

```yaml
    remote_temperature_timeout: 3min   # cn105 reverts to its internal
                                       # thermistor if the dial goes quiet
```

This is not a speculative API: `esphome/bench_cn105.yaml` already drives `id(hp).set_remote_temperature(x)` from a sensor lambda and sets `remote_temperature_timeout`, on a config verified on hardware. Cross-check the exact spelling there before writing it.

- [ ] **Step 2: Add the generic recipe**

In `esphome/example_generic.yaml`, extend the `serin_link:` block:

```yaml
  # The dial's own sensor. On the `thermostat` platform no automation is
  # needed — point the platform's sensor: at this entity's id and it becomes
  # the control temperature directly.
  link_sensor:
    temperature:
      id: dial_temp
      name: "Serin Link Temperature"
    humidity:
      name: "Serin Link Humidity"
    # Which dial is feeding. Only interesting when two dials bonded to one
    # controller both offer a sensor: last reporting dial wins, so the value
    # alternates and this names the culprit.
    dial_mac:
      name: "Serin Link"
```

- [ ] **Step 3: Document it in the README**

Add a subsection after the ESPHome quickstart, before the telemetry material:

```markdown
### The dial's own temperature sensor

A Serin Link dial has a built-in temperature/humidity sensor. Add a
`link_sensor:` block and it becomes two Home Assistant entities, and the dial
offers itself to the heat pump as a room-temperature source (Settings →
System on the dial cycles Internal / Sensor / Link):

    serin_link:
      id: serin        # not `link` — that collides with libc link()
      climate_id: hvac
      link_sensor:
        temperature:
          name: "Serin Link Temperature"
        humidity:
          name: "Serin Link Humidity"

The block is the opt-in: without it the dial never transmits its reading and
its Settings never grow the source cycle, so an existing config is unaffected
by upgrading.

The reading reaches Home Assistant either way. What it does to the *heat pump*
is up to your climate platform, because ESPHome's `Climate` has no generic
external-temperature input — see `example_cn105.yaml` (an `on_value:`
automation into `set_remote_temperature`, gated on
`id(serin).room_src_is_link()`) or `example_generic.yaml` (the `thermostat`
platform just takes the entity as its `sensor:`).
```

- [ ] **Step 4: Record the arbitration decision in the wire spec**

In `docs/serin-link-wire-spec.md` §11, extend the "Second temperature sensor / follow-me" bullet:

```markdown
- **Second temperature sensor / follow-me:** implemented as `DIAL_SENSOR`
  (§10d). Arbitration is per-controller and does not yet pin *which* dial's
  sensor is the source when several dials on the same controller offer one.
  Both existing adopters resolve it the same way — last reporting dial wins —
  and the ESPHome component additionally accepts a `BLE` selection it cannot
  serve, reporting `UNAVAILABLE`, because a rejected edit is re-sent
  indefinitely (§10d has no give-up rule for the dial).
```

- [ ] **Step 5: Validate the examples**

```bash
cd ~/serin-link-core
/tmp/esphome-venv/bin/esphome config esphome/example_cn105.yaml
/tmp/esphome-venv/bin/esphome config esphome/example_generic.yaml
```

Expected: both validate. This catches a mistyped lambda id or a key that does not exist in the Task 2 schema — the most likely defect in a docs task.

- [ ] **Step 6: Commit**

```bash
cd ~/serin-link-core
git add esphome/example_cn105.yaml esphome/example_generic.yaml \
        README.md docs/serin-link-wire-spec.md
git commit -m "docs(esphome): recipes for the dial's room sensor

Publishing the reading is half the feature; without a worked actuation
example the user gets a number in HA and no way to make the heat pump use
it. cn105 gets the gated set_remote_temperature automation, thermostat
just takes the entity as its sensor. Also records in the wire spec that
both adopters arbitrate multiple dials as last-reporting-wins."
```

---

## Notes for the executor

- **Task order is load-bearing.** Task 2 deliberately omits the capability bit so the feature never goes half-live on the wire. Do not "helpfully" add it early.
- **`~/serin-link/main/espnow_client.c` is being edited concurrently** by the user. This plan depends on two behaviors there, not on its text: the cap gate at the top of `maybe_send_dial_sensor`, and `wantSrc` being cleared only when `INFO` reports a matching source. Re-read both before Task 3.
- **ESPHome is not installed here.** Tasks 2–4 have real verification steps that need it. If the install fails, say so explicitly in the commit and in the task report rather than skipping quietly — an unverified claim is worse than a known gap.
