# Pinning the room source to one dial — design

**Date:** 2026-08-13
**Repo:** `serin-link-core` (ESPHome component only). No change to `src/`,
`include/`, the wire spec, or the dial firmware.
**Status:** approved design, pending implementation plan
**Companion:** `2026-08-13-esphome-dial-diagnostics-design.md` (direction A).
Ships together with it; A is how you discover the MAC this spec asks you to
configure.

## Problem

`link_sensor:` owns exactly one set of temperature / humidity / MAC entities
(`serin_link.h:148-151`), and whichever bonded dial reported last owns them
(`serin_link.cpp:809-816`). With two dials bonded to one controller:

- The HA `temperature` entity alternates between two rooms. Its history is a
  blend of two places, and nothing in HA says which sample came from where
  except the `dial_mac` diagnostic — whose documented purpose is precisely to
  let you watch the value flap (`__init__.py:85-87`).
- Worse, the documented actuation recipe feeds that entity to the heat pump
  while `room_src_is_link()` holds (`example_cn105.yaml:135`). So the pump's
  remote-temperature input alternates between rooms too. This is not a display
  bug; it changes what the compressor does.

The wire spec admits the gap and says both adopters resolve it the same way —
last reporting dial wins (`docs/serin-link-wire-spec.md:705`).

## Goals

- One designated dial feeds the room-sensor entities and therefore the heat
  pump. Other bonded dials do not.
- Configs that don't set the new key behave exactly as they do today.
- No dial is ever pushed into the §3 retry pathology.

## Non-goals

- **Per-dial measurement entities** (survey direction C). Still one entity set;
  this spec decides *who fills it*, not how many there are.
- **Aggregation** — min / max / mean across dials (direction D).
- **Telling the non-primary dial that it lost** (direction E). §4 explains why
  that is impossible without a core change, and what the non-primary dial sees
  in the meantime.
- **Automatic election** (first-bonded, strongest-RSSI, most-recently-paired).
  §2 rejects it.

## 1. The one constraint that shapes everything

A dial re-sends its room-source edit until INFO echoes the source it asked
for, and §10d of the wire spec defines **no give-up rule** — the reference
comment in `serin_link.cpp:772-780` records this as observed behavior at
~3 Hz, forever. The BLE case already exists as the worked example: the
component cannot serve BLE, so it *accepts* the selection and reports
`SL2_ROOMST_UNAVAILABLE`, because rejecting it would spin the dial forever.

**Therefore: arbitration must never reject a selection.** Whatever a
non-primary dial asks for is accepted and echoed. What this spec changes is
whose *reading* is used, never whether an edit is honored.

## 2. Config surface

One new optional key inside the existing `link_sensor:` block:

```yaml
serin_link:
  link_sensor:
    primary_dial: "10:51:DB:8E:EB:38"   # optional; unset = today's behavior
    temperature: { name: "Room temperature" }
    humidity:    { name: "Room humidity" }
    dial_mac:    { name: "Reporting dial" }
```

- Validated with ESPHome's `cv.mac_address`, so it is normalized at config
  time and emitted into `to_code` as six byte literals via
  `set_primary_dial(...)`. **No runtime MAC string parsing exists anywhere in
  this design** — the parse-bug class is removed rather than tested.
- **Unset is the default and means last-reporting-wins**, byte-identical to
  today. This is what keeps every existing config unchanged.

Deliberately *not* an `arbitration:` enum. The survey sketched
`arbitration: last_reporting | pinned` plus `primary_dial:`, which is two keys
encoding one decision — the presence of `primary_dial:` already says which
mode you're in. One key, no invalid combinations to validate.

**Rejected: automatic election.** `first_bonded` sounds like it removes the
pair-then-configure step, but `sl2_link_forget_dial()` compacts the bond table
(`src/sl2_link.c:582`), so "slot 0" silently becomes a different dial the
moment you forget one. An implicit primary that changes identity during an
unrelated maintenance action is worse than an explicit MAC. RSSI-based
election needs per-peer RSSI the core doesn't track.

The MAC is discoverable without a serial console: direction A's `dials[].mac`
entities and the expanded `dump_config()` both print it. That is the intended
workflow — pair, read the MAC in HA, paste it here.

## 3. Behavior

In `room_sensor_feed()` (`serin_link.cpp:758`), after the existing length and
validity handling, add a single gate:

```
primary_set && src_mac != primary_dial_  ->  this frame updates no measurement state
```

Precisely, a non-primary frame:

- **does not** touch `dial_temp_dc_`, `dial_hum_pct_`, `dial_temp_ms_`,
  `dial_mac_`, `dial_has_sensor_`, and triggers no publish;
- **does** still have its `want_src` edit honored — the `is_edit` branch
  (`:781`) runs unchanged, for the §1 reason;
- is logged once per source MAC at `ESP_LOGI` ("ignoring room sensor from
  %s: primary_dial is %s"), so a mistyped MAC is diagnosable from the log
  rather than presenting as a silently dead sensor. Once per MAC, not per
  frame — these arrive at up to 3 Hz.

Everything downstream is unchanged and correct by construction, because the
existing code already derives everything from the fields above:
`room_src_status_()` (`:738`) computes freshness from `dial_temp_ms_`, which
now only ever advances from the primary, so a dead primary reports `STALE` /
`UNAVAILABLE` even while a non-primary dial is chattering happily. The 1 Hz
stale edge (`:1009`) needs no change at all.

**A primary that is bonded but silent, or not bonded at all** (MAC typo,
dial not yet paired) resolves to `UNAVAILABLE` through that same existing
path. That is the honest answer, and the log line above says why.

## 4. What the non-primary dial sees, and why that's the ceiling here

`selected_src_` stays a single global value: it is a zone-level statement
("this zone follows a Link dial"), not a per-dial one. Every dial's Settings
therefore shows the same selection, and `room_src_status_()` reports the
primary's freshness to all of them.

For a non-primary dial that selected Link, this is a partial truth: it shows
Link/OK while the reading in use is another dial's. Fixing it properly means
telling *that* dial "another Link is the source", which requires the INFO TLV
to be built per dial with dial identity in hand. INFO **is** already built per
dial (`src/sl2_link.c:422-424` calls the hook inside the per-dial
`serve_pulls()`), but the hook signature carries no dial identity:

```c
size_t (*fill_info_tlvs)(void *ctx, uint8_t *buf, size_t cap);
```

Adding that parameter is a core API change, which means `src/`, `include/`,
re-vendoring through `tools/sync_esphome.sh`, and a decision about what status
code a losing dial should see without triggering §1. That is survey direction
E, and it is out of scope here by design — this spec is component-only and
ships without touching the wire.

## 5. Files touched

| file | change |
|---|---|
| `esphome/components/serin_link/__init__.py` | `CONF_PRIMARY_DIAL` in `LINK_SENSOR_SCHEMA` (`cv.mac_address`), emit `set_primary_dial()` in `to_code` |
| `esphome/components/serin_link/serin_link.h` | `primary_dial_[6]`, `has_primary_dial_`, `set_primary_dial()`, the once-per-MAC log latch |
| `esphome/components/serin_link/serin_link.cpp` | the gate in `room_sensor_feed()`, the log line, `dump_config()` line |
| `esphome/example_cn105.yaml` | document the key, commented out, pointing at the diagnostics entities as the way to get the MAC |
| `README.md` | `primary_dial:` in the `link_sensor:` docs |

No `src/`, no `include/`, no re-vendoring.

## 6. Testing and verification

- **Schema:** `esphome config` rejects a malformed MAC (`cv.mac_address` does
  this); a config with `primary_dial:` and no `temperature:` still validates
  (every `link_sensor:` child stays optional).
- **Compile:** covered by the existing `esphome-compile` CI job once the key
  appears in a compiled example.
- **Honest gap:** as in the companion spec, there is no host harness for the
  C++ adapter, so the gate itself is verified on hardware, not by unit test.
- **Bench checklist** (two dials bonded, both with sensors):
  1. `primary_dial:` unset → the entity still alternates between dials
     (today's behavior, proving back-compat rather than assuming it).
  2. `primary_dial:` = dial A → the `dial_mac` entity pins to A and never
     shows B; B's readings change nothing in HA.
  3. Select "Link" as the room source **on dial B** → the edit is accepted and
     stops re-sending (watch for the absence of the ~3 Hz repeat in the log),
     and the pump follows **A's** temperature.
  4. Power off dial A → the entity goes stale/unknown and `room_src_status_()`
     reports STALE→UNAVAILABLE even though B is still reporting. This is the
     point of the feature: the pump does not silently switch rooms.
  5. Mistyped `primary_dial:` → one `ignoring room sensor from …` log line per
     dial, entity never populates.

## 7. Risks

- **A pinned dial that dies takes the room source down with it.** That is the
  intended semantic (silently substituting another room is the bug being
  fixed), but it is a behavior change for anyone who pins and then expects
  redundancy. Documented in the README next to the key; direction D
  (aggregation) is the answer for people who want failover.
- **Partial truth on the non-primary dial's face** (§4). Bounded and
  documented; E is the fix.
- **The MAC is discovered, not declared.** Pairing must happen before the key
  can be set, and a re-paired dial keeps its MAC so the key survives
  re-pairing. Forgetting and pairing a *different* physical dial requires
  editing the key — noted in the README.
