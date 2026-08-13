# Dial diagnostics and per-dial forget on ESPHome — design

**Date:** 2026-08-13
**Repo:** `serin-link-core` (ESPHome component only). No change to `src/`,
`include/`, the wire spec, or the dial firmware.
**Status:** approved design, pending implementation plan

## Problem

A controller bonds up to `SL2_MAX_DIALS` (4) dials. The C core tracks each one
in detail — MAC, liveness, time since last probe, model, firmware, applied
`caps_seq`, cert state — and exposes it through `sl2_link_dial_view()`
(`include/serin_link/sl2_link.h:206-217`), alongside `sl2_link_forget_dial()`,
`sl2_link_dial_live(idx)` and `sl2_link_pair_seconds_left()`.

The ESPHome adapter surfaces **none** of it. `SerinLinkComponent` exposes only
`dial_count()`, `any_dial_live()` and `forget_all_dials()`
(`esphome/components/serin_link/serin_link.h:88-95`); `sl2_dial_view_t` is
never called. The practical consequences:

1. **Home Assistant cannot see which dials are bonded.** A count, and "at least
   one is alive". Not which, not since when, not what firmware.
2. **Forget is all-or-nothing.** Dropping one misbehaving dial means forgetting
   all of them and re-pairing every remaining dial by hand, at the controller.
3. **Pairing is fire-and-forget.** `pair_result()` and
   `pair_seconds_left()` are not surfaced. Pressing a "Pair" button on a full
   bond table sets `pair_result = "full"` and opens no window
   (`src/sl2_link.c:146`) — from HA this is indistinguishable from success.
   The same is true of `"pin-mismatch"`, `"timeout"`, and `"cancelled"`.

This is the smallest of the multi-dial gaps and the one that blocks diagnosing
the others: today, when two dials fight over the room-sensor entity, the only
evidence in HA is the `dial_mac` text sensor flickering between two values.

## Goals

- Home Assistant can see every bonded dial: MAC, live/not, time since last
  heard, firmware.
- A single dial can be forgotten from HA, by slot or by MAC, without disturbing
  the others.
- Pairing reports its outcome — including the silent failures (`full`,
  `pin-mismatch`).
- Existing configs upgrade with zero behavior change and zero new entities.

## Non-goals

- **Any change to arbitration.** Which dial's room sensor wins is untouched by
  *this* spec. Pinning it to one dial is direction B, designed separately in
  `2026-08-13-esphome-primary-dial-design.md` and implemented alongside this
  one; the two compose (A's `dials[].mac` entities are how you discover the
  MAC that B's `primary_dial:` key wants). Directions C and D — per-dial
  measurement entities and aggregation — remain future work.
- **Per-dial room-sensor entities.** `link_sensor:` keeps its single entity
  set. This spec adds *diagnostic* entities only; it does not split the
  measurement entities.
- **Anything on the wire.** No packet, TLV, capability bit, or core API
  changes. `tools/sync_esphome.sh` re-vendoring is therefore not involved, and
  the `vendored-copies-in-sync` CI job is unaffected.
- **A pairing window entity that HA can write.** Pairing stays initiated by an
  action (button/automation); this spec only reports its state.

## 1. Slot semantics — what a row identifies

`sl2_link_forget_dial()` **compacts** the bond table
(`src/sl2_link.c:582`): removing entry *i* shifts every later entry down and
zeroes the tail. `sl2_link_dial_view(idx)` indexes that same compacted array.

So a diagnostics row is **a bond-table slot, not a dial**. Forget slot 0 and
the dial that was in slot 1 is now in slot 0 — the entities named "Dial 2 …"
would start describing it.

This is accepted rather than fixed, with one mitigation and one rejected
alternative:

- **Mitigation:** the `mac:` entity is the anchor. A config that declares
  `mac:` for each row makes a compaction visible in HA the instant it happens,
  instead of silently re-labelling. The schema comment says so plainly, and
  the documented example declares `mac:` on every row.
- **Rejected: MAC-keyed rows** (`- mac_address: "10:51:DB:8E:EB:38"`).
  ESPHome config is static, so this would mean pair → read the MAC from the
  log → edit YAML → reflash, and a slot for a dial that never pairs would sit
  permanently unknown. That friction belongs to per-dial *measurement*
  entities (survey direction C), not to a diagnostics surface.

An empty slot (index ≥ `dial_count()`, i.e. `sl2_link_dial_view()` returned
false) publishes: `mac` = `""`, `linked` = off, `last_seen` = `NAN`,
`firmware` = `""`.

## 2. Entities and the YAML surface

A new optional `diagnostics:` block on the component, following the
`link_sensor:` pattern already established: the entities are **owned by the
component** (there is nothing pre-existing to bind to), presence of the block
is the opt-in, and every child is optional.

```yaml
serin_link:
  id: serin
  climate_id: hvac

  diagnostics:
    bonded_count:    { name: "Bonded dials" }
    pairing_status:  { name: "Pairing" }
    pairing_seconds: { name: "Pairing window" }
    dials:
      - mac:       { name: "Dial 1 MAC" }
        linked:    { name: "Dial 1 linked" }
        last_seen: { name: "Dial 1 last seen" }
        firmware:  { name: "Dial 1 firmware" }
      - mac:       { name: "Dial 2 MAC" }
        linked:    { name: "Dial 2 linked" }
```

| key | platform | value |
|---|---|---|
| `bonded_count` | `sensor` | `sl2_link_dial_count()`, 0–4 |
| `pairing_status` | `text_sensor` | `sl2_link_pair_result()` verbatim: `idle`, `listening`, `confirming`, `paired`, `timeout`, `cancelled`, `full`, `pin-mismatch` (the enumeration in the `sl2_link.h:192` comment omits `cancelled`, which `pair_cancel()` does set — `src/sl2_link.c:171`; trust the code) |
| `pairing_seconds` | `sensor` | `sl2_link_pair_seconds_left()`, 0 when closed |
| `dials[].mac` | `text_sensor` | `AA:BB:CC:DD:EE:FF`, `""` when the slot is empty |
| `dials[].linked` | `binary_sensor` | `sl2_dial_view_t.live` |
| `dials[].last_seen` | `sensor` (s) | `last_seen_ms / 1000`; `NAN` when never heard (`-1`) or slot empty |
| `dials[].firmware` | `text_sensor` | `sl2_dial_view_t.fw`, `""` until DIAL_INFO arrives |

Constraints:

- `dials:` is `cv.ensure_list`, length ≤ `SL2_MAX_DIALS` (4), validated in
  Python against a constant that mirrors the C `#define`. A longer list is a
  config error naming the limit.
- `binary_sensor` joins `AUTO_LOAD` (currently `climate`, `select`, `sensor`,
  `text_sensor`).
- All diagnostics entities default to `entity_category: diagnostic`, matching
  the existing `dial_mac` text sensor (`__init__.py:88`).
- `model` and `cert_state` deliberately get **no** schema keys (YAGNI). The
  component instead exposes a public accessor (§4) so a template sensor can
  read any field of `sl2_dial_view_t` without the schema growing.

## 3. Publishing discipline

The component's rule is publish-on-change, never on a timer; the one existing
exception is the room-sensor stale edge, which fires on the *absence* of
frames (`serin_link.cpp:1006-1017`). Diagnostics are polled state, so they need
an explicit discipline rather than inheriting one.

A 1 Hz walk in `loop()`, sharing the existing 1 Hz tick, calls
`sl2_link_dial_view()` for each declared slot and:

- **`mac`, `firmware`, `linked`, `bonded_count`, `pairing_status`** — publish
  only when the value differs from the last published one. In steady state
  this is silent.
- **`last_seen`** — needs its own rule, because neither change-only nor
  publish-on-new-probe works. The dial firmware probes background zones every
  4 s plus up to 1.8 s of stagger (`sl2_link.h:165-172`), so on a live dial
  this value sawtooths 0–6 s: change-only publishes every second, and
  publish-on-drop publishes every ~4 s. Neither carries information. The rule
  is therefore: **publish immediately on either `linked` edge, and otherwise
  at most once per 60 s in either state.** The edges are the signal — when a
  dial dropped and when it came back — and the 60 s refresh keeps an ongoing
  outage's duration visible without a per-probe sawtooth.
- **`pairing_seconds`** — the deliberate exception: publish at 1 Hz for as long
  as a window is open, then one final `0` when it closes. Bounded at ~60
  states per pairing attempt, and a live countdown is the entity's whole
  purpose.

Every declared entity publishes once on the first 1 Hz tick after `setup()`,
so HA never shows a diagnostics entity as unknown after a restart.

## 4. C++ and action surface

New public methods on `SerinLinkComponent`, alongside the existing lambda API
(which is unchanged — no config breaks):

```cpp
bool forget_dial_slot(int idx);                  // false if idx >= dial_count()
bool forget_dial(const std::string &mac);        // "AA:BB:.." or "aabbcc..";
                                                 // false on parse fail / not bonded
int  pair_seconds_left();
bool dial_view(int idx, sl2_dial_view_t *out);   // passthrough, for lambdas
std::string dial_mac_str(int idx);               // "" when the slot is empty
```

`dial_view()` is the escape hatch that keeps `model` and `cert_state` out of
the schema: a template text sensor can read them in four lines of YAML.

ESPHome automation actions, so example configs stop needing raw lambdas:

```yaml
button:
  - platform: template
    name: "Pair a dial"
    on_press:
      - serin_link.pair_start: { window: 60s }

  - platform: template
    name: "Forget dial 1"
    on_press:
      - serin_link.forget_dial: { slot: 0 }      # or: mac: "10:51:DB:8E:EB:38"
```

Actions: `serin_link.pair_start` (`window:`, default 60 s), `pair_cancel`,
`forget_dial` (`slot:` xor `mac:` — supplying both, or neither, is a config
error), `forget_all_dials`. All arguments templatable. Each action carries an
optional `id:` for multi-component configs, per ESPHome convention.

A forget that finds no such dial logs a warning and does nothing; it is not a
runtime failure.

## 5. `dump_config()`

Currently one line: `bonded dials: 2` (`serin_link.cpp:1050`). It becomes the
bond table, which is what anyone actually reads when a dial misbehaves:

```
  bonded dials: 2
    [0] 10:51:DB:8E:EB:38  live  last seen 3 s    model "Serin Link" fw "1.4.2"  caps_seq 7  cert OK
    [1] 10:51:DB:8E:F1:84  DOWN  last seen 412 s  model "Serin Link" fw "1.4.0"  caps_seq 7  cert none
```

`model` and `fw` are separate fields of `sl2_dial_view_t` and print separately;
both are empty until that dial's DIAL_INFO arrives (`have_info`).

Free — no entities, no config, and it works on a config that declares no
`diagnostics:` block at all.

## 6. Files touched

| file | change |
|---|---|
| `esphome/components/serin_link/__init__.py` | `DIAGNOSTICS_SCHEMA`, `dials:` list validation, `binary_sensor` in `AUTO_LOAD`, entity construction in `to_code`, four automation actions |
| `esphome/components/serin_link/serin_link.h` | entity pointers, per-slot last-published cache, new public methods |
| `esphome/components/serin_link/serin_link.cpp` | 1 Hz diagnostics walk + publish gating, forget/pair methods, MAC parse/format helpers, expanded `dump_config()` |
| `esphome/example_spike.yaml` | a `diagnostics:` block with two dial rows — this is the config CI compiles |
| `esphome/example_cn105.yaml` | replace the `pair_start` / `forget_all_dials` lambdas with the new actions; add diagnostics + a per-dial forget button |
| `test/test_sl2_link.c` | compaction invariant test (§7) |
| `README.md` | document `diagnostics:` and the actions |

No `src/`, no `include/`, no `docs/serin-link-wire-spec.md`.

## 7. Testing and verification

**Host test — the compaction invariant.** The slot design rests on
`sl2_link_forget_dial()` compacting the table. `test_forget()`
(`test/test_sl2_link.c:957`) already covers part of this: two dials, forget the
first, assert `dial_mac(0)` is now the second. What it does **not** cover, and
what this design newly depends on:

- **Removal from the middle** of three — the shift loop moves exactly one
  entry and zeroes the vacated tail.
- **`sl2_link_dial_view()` after a compaction.** The existing test checks
  `dial_mac()`; the diagnostics entities read `dial_view()`, which returns
  runtime state (`live`, `model`, `fw`) carried by the same struct copy. That
  the runtime half follows the bond across a shift is assumed by every
  diagnostics row and asserted nowhere.
- **`dial_view(idx)` returning false for the vacated tail slot**, which is what
  drives the empty-slot publish values.

Add `test_forget_middle_compacts()` alongside the existing test rather than
extending it — the two-dial case is a distinct regression worth keeping
separately. It runs in the existing `host-tests` CI job.

**Compile coverage.** `example_spike.yaml` is the config the `esphome-compile`
CI job builds, so the `diagnostics:` block goes there — the new schema and
every entity platform are then compiled on every push.

**Schema validation.** `esphome config` on a file declaring five `dials:` rows
must fail with the limit named; `forget_dial` with both `slot:` and `mac:` must
fail at validation, not at runtime.

**Honest gap.** There is no host harness for the C++ adapter — `test/run.sh`
compiles pure C only. The publish-gating logic (§3) is therefore verified by
compile plus a bench run on real hardware (`bench_dryrun.yaml` with a second
dial paired), not by unit test. This spec does not claim unit coverage of it.

**Bench checklist** (two dials, `bench_dryrun.yaml`):
1. Both bonded → both rows populate; MACs match `dump_config()`.
2. Power off dial 2 → its `linked` goes off within the liveness window,
   `last_seen` climbs, dial 1 untouched.
3. `forget_dial: {slot: 0}` → dial 1's entities take dial 2's values (the
   documented compaction), `bonded_count` → 1, slot 1 empties.
4. Pair with a full table → `pairing_status` reads `full`, `pairing_seconds`
   stays 0.
5. Normal pairing → `listening` with a visible countdown, then `paired`.

## 8. Risks

- **Slot compaction surprises a user** who declared four rows and forgot the
  first one. Mitigated by the `mac:` anchor and documentation, not by code.
  Accepted: the alternative (stable MAC-keyed rows) imports reflash friction
  into a diagnostics feature.
- **Entity count.** Four rows × four entities + three globals = 19 entities if
  a user declares everything, each costing RAM and an API slot. All optional
  and opt-in; the documented example declares two rows.
- **`pairing_seconds` state churn** — bounded to the pairing window, and only
  if the entity is declared.
- **`SL2_MAX_DIALS` mirrored in Python.** The limit is a C `#define`; the
  schema needs its own copy. If the C constant ever changes, the Python
  constant must change with it. Noted in a comment on both sides.

## 9. Future work

- Survey directions B/C/D: pinned room source, per-dial measurement entities,
  aggregation. This spec's `dial_view()` accessor and slot semantics are the
  groundwork for them.
- Direction E: per-dial `ROOM_SRC` attribution in INFO, which needs the wire
  spec and the dial firmware to move together.
- A `cert_state` entity, if device attestation becomes user-facing rather than
  a tooling concern.
