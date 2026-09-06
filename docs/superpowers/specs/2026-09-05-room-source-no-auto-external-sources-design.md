# Room-temperature source: retire Auto, add external sources

Supersedes the "automatic Serin Link arbitration" and "arbitrary HA
temperature deferred" points of
`2026-08-25-controller-room-temperature-source-design.md`. Everything else in
that spec stands.

## Problem

On a controller with one bonded Link the picker showed three entries:
`Internal`, `Auto (last reporting)`, `Serin Link 1`. Two of them did the same
thing, one described the wrong sensor, and the Home Assistant temperature that
actually fed the heat pump appeared nowhere. Cause: the ESPHome component's
catalog lists only what the component itself knows (a fixed Internal entry,
an arbitration rule, bond slots). Sources wired in YAML through
`set_remote_temperature` are invisible to it, so every YAML carries its own
guard lambdas to keep HA and the dial from fighting.

## Decisions

1. **Auto is removed from both controllers.** Every Link selection pins one
   dial by MAC. "Whichever dial reported last" is not a source a user can
   reason about.
2. **The built-in entry is named `Heat pump`.** It means "the controller feeds
   no remote temperature; the heat pump uses its own sensor". `Internal` is
   firmware vocabulary.
3. **ESPHome YAML can declare external sources bound to ESPHome sensors.** The
   component owns arbitration: it subscribes to the bound sensors, lists them in
   the catalog and the HA select, reports their health, and feeds the heat pump
   through the one existing trigger. YAML guards go away.
4. **YAML stays minimal.** Every new key is optional. A working config is
   shorter after this change than before it.

## Catalog

Order and content, both controllers:

| Position | ESPHome | HomeKit |
|---|---|---|
| 0 | `Heat pump` (`SL2_ROOM_SOURCE_INTERNAL_ID`) | `Heat pump` |
| … | one entry per `sources:` item | `Average` (if selectable), BLE sensors |
| … | `Serin Link N` per bond slot | `Serin Link` / `Serin Link N` per bond |

ESPHome keeps numbering Links unconditionally: the HA dropdown is sized at
build time from `max_links`, the dial's catalog from the live bond count, and
the two must name a slot identically. HomeKit keeps its existing rule
(numbered only when more than one is bonded) because it has no build-time
list to agree with.

`SL2_ROOM_SOURCE_LINK_AUTO_ID` and `SL2_ROOM_SOURCE_NS_LINK_AUTO` remain
defined in `sl2_proto.h`, commented as retired, so the three vendored header
copies stay byte-identical and old builds keep compiling. Neither controller
lists it; a `ROOM_SOURCE_SET` naming it returns `SL2_ROOM_SET_BAD_SOURCE`.

## External source ids

New namespace `SL2_ROOM_SOURCE_NS_EXTERNAL = 4`. The id is the namespace in
the top byte and a 56-bit FNV-1a hash of the source's display name in the low
bytes, computed by a new inline helper `sl2_room_source_name_id(ns, name)` in
`sl2_proto.h` next to `sl2_room_source_mac_id`. Kind is
`SL2_ROOM_KIND_SENSOR`; flags are `SELECTABLE`.

Consequences, deliberately accepted:

- Stable across reflashes and reorderings of the `sources:` list.
- Renaming a source in YAML is the same as removing it: the stored id no
  longer matches the catalog and the selection falls back to `Heat pump` at
  boot, exactly as a forgotten pinned Link does today.
- A hash collision between two names in one YAML is rejected at
  configuration time.

No packet layout, TLV, feature bit, or protocol version changes.

## ESPHome YAML

```yaml
sensor:
  - platform: homeassistant
    id: ha_room_temp
    name: Home Assistant          # becomes the catalog label
    entity_id: sensor.virtual_temperature_main
    filters: [ ... ]              # unchanged

serin_link:
  link_sensor:
    links:
    on_room_temperature:
      - lambda: 'id(hp).set_remote_temperature(x);'
  room_temperature_source:
    sources:
      - sensor: ha_room_temp
```

Schema:

- `room_temperature_source:` — unchanged: optional, bare key allowed, still
  requires `link_sensor:`. Gains one optional child:
- `sources:` — list of `{ sensor: <id>, name: <string, optional> }`. `name`
  defaults to the bound sensor's ESPHome name. Duplicate names are a
  configuration error. Maximum count is bounded so the catalog still fits
  `SL2_ROOM_CATALOG_MAX` with `SL2_MAX_DIALS` Links.
- `link_sensor: on_room_temperature:` — unchanged location and signature
  (`x` is a float in °C). Semantics widen to "reading of the selected room
  source, whatever kind it is", plus the value `0` when `Heat pump` is chosen.
- `link_sensor: primary_select:` — retired with the same `_removed` pattern
  used for `primary_link:` and `stale_after:`, pointing at
  `room_temperature_source:`.

The HA `Room Temperature Source` select's option list is
`["Heat pump"] + [source names] + ["Serin Link 1".."Serin Link N"]`.

## Component behaviour

**Selection state.** `selected_source_id_` stays the single persisted truth.
`room_source_project_()` now yields one of three shapes: heat pump, pinned
Link MAC, or external index. `has_primary_dial_` is true for every Link
selection; the unpinned Link state no longer exists.

**Feeding.** `on_room_temperature` fires:

- for a pinned Link: as today, from `publish_dial_`, only for frames from the
  pinned MAC;
- for an external source: from the bound sensor's state callback whenever it
  delivers a non-NaN value and that source is selected;
- on any selection change: once, immediately, with the new source's last
  known value if it has one, and with `0` when the new selection is
  `Heat pump`. A source with no value yet fires nothing until it has one.

**Health** (`room_src_status_()`), reported in `ROOM_SOURCE_V2` and to the
dial:

| Selected | OK | STALE | UNAVAILABLE |
|---|---|---|---|
| Heat pump | always | — | — |
| Pinned Link | fresh frame from that MAC | last frame older than 90 s | dial has no sensor, or no frame yet |
| External | value within 90 s | value older than 90 s | no value yet, or NaN |

The 90 s window is the existing `dial_stale_ms_`; it is reused, not made
configurable.

**v3 compatibility** (`DIAL_SENSOR.want_src` from a pre-catalog dial):

- `INTERNAL` → `Heat pump`.
- `LINK` → the id of the dial that sent the frame. This replaces the previous
  mapping to Auto and is the natural meaning of "use me".
- `BLE` → unchanged (accepted, reported UNAVAILABLE).

**Migration at boot.** Applied once, in the existing preference-load path:

- Stored `LINK_AUTO_ID` → the sole bonded Link's id when exactly one is
  bonded, otherwise `Heat pump`.
- v3 blobs with `selected_src_ == LINK` and no pin → same rule.
- Stored id absent from the current catalog (external source removed or
  renamed, Link forgotten) → `Heat pump`. This generalises the existing
  "no longer bonded" fallback.

**Logging.** Add one INFO line on every accepted `ROOM_SOURCE_SET` from a
dial naming the resulting source, since today the wire path is silent.

## HomeKit controller

- `room_catalog_build`: drop the `links > 1` Auto entry; rename `Internal`
  to `Heat pump`.
- `h_room_source_set`: remove the `LINK_AUTO_ID → ROOM_MEMBER_LINK` arm.
- v3 `want_src == LINK` → pin the sending dial's MAC (`roomSourceId`).
- Boot migration: stored `LINK_AUTO_ID` or an unpinned `ROOM_MEMBER_LINK`
  single → sole bonded Link, else internal.
- Its Average member set is unchanged: a pinned Link contributes as before.

## Dial

No code change. The dial renders whatever names the catalog carries. Two
follow-ups already identified and tracked separately: gate the Settings hint
on `SL2_FEAT_ROOM_CATALOG`, and give the empty-catalog press a visible
response.

## Testing

- `test/test_sl2_proto.c`: `sl2_room_source_name_id` is deterministic, keeps
  the namespace byte, and differs for two distinct names.
- `test/test_sl2_link.c`: a `ROOM_SOURCE_SET` for `LINK_AUTO_ID` is answered
  `BAD_SOURCE` by a controller whose catalog omits it (host-side hooks).
- `test/esphome/`: `pass_room_sources.yaml` (two sources, one with an
  explicit name), `fail_room_sources_duplicate_name.yaml`,
  `fail_room_sources_no_link_sensor.yaml`, `fail_primary_select_removed.yaml`.
  Update `pass_room_temperature_source.yaml` and drop
  `fail_room_source_with_primary_select.yaml`.
- Bench, controller "Heat Pump - Main" with dial #4: select each of
  `Heat pump`, `Home Assistant`, `Serin Link 1` from the dial and from HA;
  confirm the heat pump's echoed room temperature follows the chosen source
  within one status cycle, that `Heat pump` reverts immediately, and that the
  dial's Settings row shows the chosen name with the matching status.

## Out of scope

- Any change to the HA-side automation that produces
  `sensor.virtual_temperature_main`.
- Making the stale window configurable.
- Dial-side UI changes (tracked separately, see Dial).
