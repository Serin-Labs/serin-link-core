# Dial room sensor on ESPHome — design

**Date:** 2026-08-13
**Repo:** `serin-link-core` (ESPHome component). No change to the dial firmware.
**Status:** approved design, pending implementation plan

## Problem

A Serin Link dial has an SHT4x. Paired with a Home Assistant / ESPHome
controller, that reading is invisible: it never appears in HA, and it never
reaches the heat pump.

The dial's half is already complete. `maybe_send_dial_sensor()` in
`serin-link/main/espnow_client.c` streams a `DIAL_SENSOR` packet — temperature,
humidity, a has-hardware flag, and a `want_src` source-edit channel — and
Settings → System cycles the room source. The `serin-link-core` C core already
decodes the packet and dispatches it to an optional `hvac->room_sensor` hook
(`src/sl2_link.c`, `case SL2_PKT_DIAL_SENSOR`).

The gap is the ESPHome adapter:

1. `SerinLinkComponent::setup()` leaves `hvac_.room_sensor` unset, so the core
   drops every frame it decodes.
2. `hvac_get_caps()` never sets `SL2_FEAT_LINK_SENSOR`, and the dial refuses to
   transmit `DIAL_SENSOR` to a controller that does not advertise that bit
   (first guard in `maybe_send_dial_sensor`). So on ESPHome the reading never
   leaves the dial at all.

`mitsubishi-cn105-homekit` implements this (`main/link_sensor.cpp`,
`h_room_sensor()` in `main/espnow_link.cpp`), which is why it works there.

## Goals

- The dial's temperature and humidity appear in Home Assistant as ordinary
  ESPHome entities, with history and correct unavailability.
- The dial's room-source selection works against an ESPHome controller:
  the choice is accepted, persisted, and confirmed back over the wire.
- Existing configs upgrade with zero behavior change.

## Non-goals

- Feeding the reading into the climate entity from inside the component.
  ESPHome's `Climate` base class has no generic external-room-temperature
  input; every platform spells it differently (cn105 has a
  `remote_temperature` action, `thermostat` takes a `sensor:`). The component
  publishes the value and the selection state; YAML wires them to the
  platform. This keeps the component platform-agnostic.
- An HA-writable room-source `select` entity (§10).
- Per-dial entities, or averaging several dials (§4).

## 1. Receiving the reading

Install the hook next to the existing trampolines in `setup()`:

```cpp
hvac_.room_sensor = t_room_sensor;
```

The hook runs on the loop task — the core's SPSC ring (`sl2_rxq_t`) already
moves frames off the Wi-Fi task and `loop()` drains them, which is the same
contract the other hooks rely on. No locking. (The reference implementation
holds a spinlock in `LinkSensor::feed()`; its comment says so explicitly as
future-proofing, not because its drain is off-task. We do not need it.)

State added to `SerinLinkComponent`:

| field | meaning |
|---|---|
| `dial_temp_dc_` | latest valid `temp_dc`, `SL2_DC_NA` until one arrives |
| `dial_hum_pct_` | latest valid `hum_pct`, `SL2_HUM_NA` until one arrives |
| `dial_temp_ms_` | `millis()` of the last frame carrying a **valid temperature** |
| `dial_has_sensor_` | latest frame's `SL2_DSF_HAS_SENSOR` |
| `dial_mac_[6]` | MAC of the dial that last fed us |
| `dial_pub_dc_`, `dial_pub_ms_` | last values published to HA (§2 dedup) |

**Freshness follows the temperature, not the packet.** A dial that has sensing
hardware but no reading yet must not keep the stale watchdog alive:
`dial_temp_ms_` updates only when `temp_dc != SL2_DC_NA`. Humidity updates
independently and does not touch freshness. This is the one subtlety worth
copying deliberately from `LinkSensor::feed()`.

**Arbitration: last reporting dial wins.** Whichever bonded dial reported most
recently owns the entities. This matches `mitsubishi-cn105-homekit` exactly, so
both controller implementations behave identically against shared dial
firmware, and it is correct for the overwhelmingly common one-dial-per-zone
install. The wire spec (§11) leaves arbitration deliberately open. With two
dials in one room the value alternates between two readings a fraction of a
degree apart; the `dial_mac` text_sensor (§2) makes that diagnosable rather
than mysterious.

## 2. Entities and publishing

Two optional owned `sensor::Sensor` children and one optional
`text_sensor::TextSensor`, all created by the component's own codegen (§5):

- **temperature** — `publish_state(dial_temp_dc_ / 10.0f)`, device class
  `temperature`, unit `°C`, state class `measurement`, 1 decimal.
- **humidity** — `publish_state(dial_hum_pct_)`, device class `humidity`,
  unit `%`, state class `measurement`, 0 decimals.
- **dial_mac** — formatted `XX:XX:XX:XX:XX:XX`, entity category `diagnostic`.

Both sensors are published **whether or not the dial is the selected room
source**. Visibility and control are independent: you get the number in HA
while the heat pump still runs on its own thermistor.

Wire temperatures are deci-Celsius and °F never crosses the wire (spec §11), so
the component publishes °C and leaves display conversion to HA. The per-dial
`use_f_` display preference is a dial-facing concern and does not apply here.

**Publish dedup — a real trap, not an optimization.** In steady state the dial
sends at most every 20 s (`SENS_KEEPALIVE_MS`) or on a 0.5 °C change. But while
a source edit is unconfirmed the dial re-sends `DIAL_SENSOR` every link-task
pass — roughly 3 Hz — until `INFO` echoes the source it asked for. Publishing
per frame would spray HA with duplicates for the whole confirmation window.

The publish decision is **frame-driven**, evaluated in the hook, never on a
timer: on each frame carrying a valid temperature, publish if the value differs
from `dial_pub_dc_`, or if more than 30 s has passed since `dial_pub_ms_` (so a
motionless room still gives HA a heartbeat). Humidity rides the same decision —
one gate, both entities — so the two never drift apart in HA's history.

**Staleness.** A 1 Hz check in `loop()`, alongside the existing 5 s Wi-Fi
power-save re-assert that already establishes the periodic-work pattern there:
if `dial_temp_ms_` is older than `stale_after`, publish `NAN` to both sensors
once — HA renders that as unknown rather than a frozen number — and latch until
a fresh reading arrives. This is the one publish path that is not frame-driven,
for the obvious reason that it fires on the absence of frames.

## 3. Room source and the `ROOM_SRC` TLV

A persisted `selected_src_` (`ESPPreferenceObject`, same pattern as
`use_f_pref_` / `caps_fp_pref_`, its own preference key), default
`SL2_ROOMSRC_INTERNAL`.

On a frame the core marked `is_edit` (the core does the length check, so the
adapter must not re-derive it from `want_src`), if `want_src` is one of the
three known sources, store it and save. Anything else is dropped.

`fill_info_tlvs()` then emits `SL2_TLV_ROOM_SRC` = `{selected_src, status}`
via a new typed helper in `sl2_info.h`:

```c
/* 0x0B ROOM_SRC: u8 applied_src; u8 status */
static inline bool sl2_info_put_room_src(uint8_t *buf, size_t cap, size_t *off,
                                         uint8_t applied_src, uint8_t status) {
    uint8_t v[2] = { applied_src, status };
    return sl2_tlv_put(buf, cap, off, SL2_TLV_ROOM_SRC, v, 2);
}
```

The reference emits this TLV with a raw `sl2_tlv_put`; a wrapper matches the
other nine `put_*` helpers and gives the host suite something to test.

The TLV is emitted only when the `link_sensor:` block is configured — same
rule as the capability bit (§4), and consistent with "feature bit =
capability, TLV presence = current validity" (spec §9).

Status is computed honestly about **the feed**:

| selected | status |
|---|---|
| `INTERNAL` | `SL2_ROOMST_OK` — a thermistor has no failure mode |
| `LINK`, fresh reading | `SL2_ROOMST_OK` |
| `LINK`, reading older than `stale_after` | `SL2_ROOMST_STALE` |
| `LINK`, never received a reading | `SL2_ROOMST_UNAVAILABLE` |
| `LINK`, dial cleared `SL2_DSF_HAS_SENSOR` | `SL2_ROOMST_UNAVAILABLE` |
| `BLE` | `SL2_ROOMST_UNAVAILABLE` (see below) |

The has-hardware row is not redundant with never-received-a-reading: no sensing
hardware is a permanent no, so it answers immediately instead of making the
user wait out `stale_after` for a verdict that cannot change. It is also what
`SL2_DSF_HAS_SENSOR` exists for, and the same use the reference controller
puts it to.

Confirmation latency is bounded by the dial's `INFO` pull, which the core
serves on `SL2_WANT_INFO` throttled by `SL2_PULL_THROTTLE_MS`. No push needed.

### The BLE edge case, and a deliberate deviation from the reference

The dial offers a `Sensor` (BLE) room source whenever the controller sets
`SL2_FEAT_SENSOR_BATT` — which an ESPHome config does merely by binding
`battery_sensor:`. The component has no BLE room source to honor.

The reference firmware *rejects* an unhonorable edit
(`RoomAvg::legacySrcSelectable`). Because the dial re-sends until `INFO`
reports the source it asked for, with no timeout and no give-up
(`apply_info_tlvs` clears `wantSrc` only on a match), rejection means a
`DIAL_SENSOR` edit repeating at ~3 Hz forever.

So: **accept any of the three known sources into `selected_src_`, and let
status tell the truth.** BLE resolves to `UNAVAILABLE`, the dial renders
"Sensor — unavailable", the retry terminates, and the user cycles on. That is
exactly what `SL2_ROOMST_UNAVAILABLE` is specified to mean ("selected but never
had a reading"), and the reference itself returns `UNAVAILABLE` for BLE on a
build compiled without BLE.

The alternative — matching the reference bit-for-bit and eating the retry storm
— was considered and rejected.

## 4. CAPS

```cpp
if (link_sensor_configured_) out->features |= SL2_FEAT_LINK_SENSOR;
```

Set if and only if the `link_sensor:` block is present, following the existing
"unbound binding = feature bit unset" rule in `hvac_get_caps()`. A config that
does not opt in never advertises the bit, so the dial never sends the packet
and never grows a room-source cycle in its Settings. Upgrading the component
without touching YAML changes nothing.

The existing `caps_fp_pref_` fingerprint in `setup()` already detects the
changed capability word on first boot after the YAML edit and calls
`sl2_link_caps_changed()` to announce it to bonded dials. Nothing to add.

## 5. YAML surface

Inline sub-keys on the `serin_link:` block, matching the existing telemetry
bindings rather than introducing a `sensor:` platform. Presence is the opt-in.

```yaml
serin_link:
  id: serin        # not `link` — collides with libc link() in generated code
  climate_id: hvac
  link_sensor:
    temperature:
      name: "Living Room Serin Link Temperature"
    humidity:
      name: "Living Room Serin Link Humidity"
    dial_mac:
      name: "Living Room Serin Link"
    stale_after: 90s
```

Schema:

| key | type | default |
|---|---|---|
| `temperature` | `sensor.sensor_schema(...)` | absent |
| `humidity` | `sensor.sensor_schema(...)` | absent |
| `dial_mac` | `text_sensor.text_sensor_schema(...)` | absent |
| `stale_after` | `cv.positive_time_period_milliseconds` | `90s` |

`90s` is three missed 20 s keepalives plus slack. Every child is optional:
`link_sensor: {}` is legal and means "accept the dial as a room source, create
no HA entities" — the capability bit is what the block declares, the entities
are a convenience on top.

Codegen uses `await sensor.new_sensor(conf)` / `text_sensor.new_text_sensor`,
registering each child on the parent via setters, as with the existing
bindings. `AUTO_LOAD` already carries `sensor` and `text_sensor`.

A public getter for the actuation automation:

```cpp
bool room_src_is_link() const { return selected_src_ == SL2_ROOMSRC_LINK; }
```

## 6. Actuation recipes (documentation, not code)

**cn105** — the dial temperature drives `remote_temperature`, gated on the
selection:

```yaml
serin_link:
  id: serin        # not `link` — collides with libc link() in generated code
  climate_id: hvac
  link_sensor:
    temperature:
      id: dial_temp
      name: "Living Room Serin Link Temperature"
      on_value:
        - if:
            condition:
              lambda: 'return id(serin).room_src_is_link();'
            then:
              - lambda: 'id(hvac).set_remote_temperature(x);'

climate:
  - platform: cn105
    id: hvac
    remote_temperature_timeout: 3min   # cn105 reverts to internal on its own
```

This is not a speculative API: `esphome/bench_cn105.yaml` already drives
`id(hp).set_remote_temperature(x)` from a sensor lambda and sets
`remote_temperature_timeout`, on the bench config that was verified on
hardware. The recipe adds only the `room_src_is_link()` gate.

`remote_temperature_timeout` is the platform's own safety net and is the right
place for it: if the dial dies, cn105 falls back to the internal thermistor
without the component having to model the fallback.

**thermostat** — no automation; point the platform's `sensor:` at the dial
temperature entity's id.

Both go in `esphome/example_cn105.yaml` and `esphome/example_generic.yaml`,
plus the README quickstart.

## 7. Files touched

Canonical:

- `include/serin_link/sl2_info.h` — add `sl2_info_put_room_src()`.
- `esphome/components/serin_link/serin_link.h` — state, setters, getter.
- `esphome/components/serin_link/serin_link.cpp` — hook + trampoline, publish
  and stale logic in `loop()`, `ROOM_SRC` TLV in `fill_info_tlvs()`, cap bit in
  `hvac_get_caps()`, preference in `setup()`, `dump_config()` line.
- `esphome/components/serin_link/__init__.py` — `link_sensor` schema + codegen.
- `test/test_sl2_info.c` — coverage for the new helper.
- `esphome/example_cn105.yaml`, `esphome/example_generic.yaml`, `README.md`,
  `docs/serin-link-wire-spec.md` (§11 arbitration note: record that the ESPHome
  adopter uses last-reporting-dial-wins, and the BLE-selection resolution).

Generated: re-run `tools/sync_esphome.sh` after the `sl2_info.h` edit and
commit both copies. Per `CONTRIBUTING.md` the vendored `esphome/components/
serin_link/sl2_*.{h,c}` files are never edited directly, and CI fails on drift.
`serin_link.{h,cpp}` and `__init__.py` are *not* generated — they are the
component's own source and live only in that directory.

No wire-layout change: `SL2_PKT_DIAL_SENSOR`, `SL2_TLV_ROOM_SRC`,
`SL2_FEAT_LINK_SENSOR` and every struct involved already ship in
`SL2_PROTO_VERSION` 2. `sl2_proto.h` is untouched, so the byte-identical
guarantee across the dial, controller and core trees holds without a
re-vendor there.

## 8. Testing and verification

- **Host suite** (`test/run.sh`, plain gcc, no hardware): `test_sl2_info.c`
  gains a case for `sl2_info_put_room_src` — correct tag, length 2, byte order
  — and the new helper joins `test_bounds_whole_tlv_or_nothing`, which already
  holds the other `put_*` helpers to that contract at a `cap` boundary.
- **Compile**: `esphome compile esphome/example_cn105.yaml` against the local
  component path, plus one compile of a config *without* `link_sensor:` to
  prove the opt-in path builds and the schema stays optional.
- **Hardware** (AtomS3 Lite + a dial, the rig that verified `ea9f2be`):
  1. reading appears in HA, updates on change, goes unknown ~90 s after the
     dial is powered down, recovers;
  2. Settings → System on the dial cycles Internal → Link and the choice
     sticks across a controller reboot;
  3. selecting Link with `remote_temperature` wired changes what the heat pump
     controls on;
  4. with `battery_sensor:` bound, selecting Sensor shows unavailable on the
     dial and the `DIAL_SENSOR` re-send stops (the §3 deviation, verified by
     log rate).

The adapter has no host harness in this repo and this change does not build
one. The commit message must state which of these actually ran.

## 9. Risks

- **Two dials, one zone** — value alternates. Accepted, documented, diagnosable
  via `dial_mac`. §10 is the fix if it ever bites.
- **User forgets the actuation wiring** — the dial says the source is Link and
  reports `OK` (the feed *is* live) while the heat pump still runs on its
  thermistor. Inherent to leaving actuation in YAML; mitigated by making the
  recipe prominent in both examples and the README.
- **Concurrent dial-side edits** — `serin-link/main/espnow_client.c` is under
  active modification. This design depends on its behavior, not its text, and
  the two behaviors relied on (cap gating before send; `wantSrc` cleared only
  on an `INFO` match) are load-bearing enough to be unlikely to change silently
  — but re-read both before implementing.

## 10. Future work

- A `select` entity for the room source, so HA can change it too, with the
  dial's `want_src` edit and the HA select as two writers of one persisted
  value. Natural, and out of scope here.
- Pinning a specific dial as the source when several offer one — the open
  question in wire spec §11. Would want a wire answer shared with the reference
  controller, not a private ESPHome rule.
