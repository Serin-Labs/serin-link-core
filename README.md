# serin-link-core

The controller-side reference implementation of **Serin Link** — the
encrypted ESP-NOW protocol a [Serin Link](https://github.com/Serin-Labs) (a
wall-mount rotary thermostat head) uses to pair with and control heat-pump /
HVAC controllers. Any ESP32 firmware that embeds this core becomes a zone a
Serin Link can control: the it builds its whole UI from the capabilities the
controller declares, so nothing about it is vendor-specific.

The repo ships two things:

- **`libserinlink`** — a dependency-free C library implementing the
  controller role (signed pairing with trust-on-first-use key pinning,
  multi-Link bond table, state fan-out, capability descriptors), portable to
  any platform through a small port/crypto interface.
- **An ESPHome component (`serin_link`)** that binds the core to *any* ESPHome
  `climate` entity — Mitsubishi CN105, generic `thermostat`, midea, daikin,
  gree, … — turning the device into a Serin Link zone with a few lines of YAML.

The wire protocol is specified in
[`docs/serin-link-wire-spec.md`](docs/serin-link-wire-spec.md) (current wire
version: 2, `SL2_PROTO_VERSION`) and hardware-verified against ESPHome (CN105
and generic climate platforms) and
[mitsubishi-cn105-homekit](https://github.com/akifbayram/mitsubishi-cn105-homekit),
an independent open-source CN105/HomeKit firmware.

## ESPHome quickstart

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf   # required: raw nvs_*, esp_now encrypted peers

external_components:
  - source: github://Serin-Labs/serin-link-core@v0.1.1
    components: [serin_link]

climate:
  - platform: thermostat   # any climate platform works
    id: hvac
    # ...

serin_link:
  id: serin
  climate_id: hvac

button:
  - platform: template
    name: "Pair Serin Link"
    on_press:
      - lambda: 'id(serin).pair_start(60000);'
```

Put the Serin Link in pairing mode, press **Pair Serin Link** (Home Assistant or the
ESPHome web UI) within the 60 s window, done. The Serin Link pulls the zone's
capabilities and renders only what the entity actually supports: its mode ring
shows the entity's modes, the setpoint clamps to the entity's visual range,
fan detents follow its discrete fan modes.

Complete examples in [`esphome/`](esphome/):

- [`example_cn105.yaml`](esphome/example_cn105.yaml) — Mitsubishi heat pump via
  [echavet/MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome),
  including vane-axis bindings and device-link health.
- [`example_generic.yaml`](esphome/example_generic.yaml) — the core
  `thermostat` platform (any climate entity works the same way).
- [`example_spike.yaml`](esphome/example_spike.yaml) — minimal coexistence
  config (no climate entity; canned state) for radio/pairing bring-up.
- [`bench_band.yaml`](esphome/bench_band.yaml) /
  [`bench_dryrun.yaml`](esphome/bench_dryrun.yaml) /
  [`bench_cn105.yaml`](esphome/bench_cn105.yaml) — hardware-free bench
  configs exercising dual-setpoint (HEAT_COOL band) zones, the CN105
  surface without a heat pump attached, and a field-shaped CN105 config
  (remote HA temp sensor, diagnostics) with `serin_link` added.

### The Serin Link's own temperature sensor

A Serin Link has a built-in temperature/humidity sensor. Add a
`link_sensor:` block and it becomes two Home Assistant entities, and the Serin Link
offers itself to the heat pump as a room-temperature source (Settings →
System on the Serin Link cycles Internal / Link — plus Sensor if you've also bound
a `battery_sensor:`):

```yaml
serin_link:
  id: serin        # not `link` — that collides with libc link()
  climate_id: hvac
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
    humidity:
      name: "Serin Link Humidity"
```

The block is the opt-in: without it the Serin Link never transmits its reading and
its Settings never grow the source cycle, so an existing config is unaffected
by upgrading.

The reading reaches Home Assistant either way. What it does to the *heat pump*
is up to your climate platform, because ESPHome's `Climate` has no generic
external-temperature input — see `example_cn105.yaml` (an `on_value:`
automation into `set_remote_temperature`, gated on
`id(serin).room_src_is_link()` so cycling back to Internal actually hands
control back) or `example_generic.yaml` (the `thermostat` platform's
`sensor:` points straight at the Serin Link's reading — simpler, but with no
`remote_temperature_timeout` equivalent: a stale Serin Link leaves it with no
control temperature at all, rather than falling back).

### More than one Serin Link

A controller bonds up to four Serin Links. They all get the STATE stream, and a
command from any of them is accepted — but the `link_sensor:` entities above
are a *single* set, and by default whichever one reported last owns them. Two
Links in two rooms therefore make that temperature — and any heat pump fed
from it — alternate between rooms. Pin one:

```yaml
  link_sensor:
    primary_link: "10:51:DB:8E:EB:38"
    temperature:
      name: "Serin Link Temperature"
```

Unset, the old last-reporting-wins behavior is unchanged. Set, only that
Link's readings are used; the others are still bonded, still control the heat
pump, and their room-source selection is still honored — only their
*measurement* is ignored, with one log line per ignored Serin Link.

To choose at runtime instead of baking a MAC into the config, use
`primary_select:` — a Home Assistant dropdown listing `Auto (last reporting)`
and `Serin Link 1` … `Serin Link 4`:

```yaml
  link_sensor:
    primary_select:
      name: "Primary Serin Link"
```

The two keys are mutually exclusive — a config error if you set both, so there
is never a precedence question between a compile-time pin and a stored one.

The dropdown numbers bond *slots*, but the choice is **persisted as a MAC**.
That distinction is the whole point: forgetting a Serin Link compacts the bond
table, so a stored slot would quietly re-point the pin at a different room. Two
consequences you can watch for:

- Forget an *earlier* Link and the pinned one shifts down a slot. The pin holds
  — the dropdown just relabels itself from "Serin Link 2" to "Serin Link 1".
- Forget the *pinned* Link and the pin reverts to `Auto`, with a warning in the
  log. Leaving it would strand the room source at unavailable with no way back
  short of a reflash. Note this is about **forgetting**, not going offline: an
  offline pinned Link keeps its pin, which is the entire feature.

Selecting an empty slot is ignored, logged, and the dropdown snaps back to
what is actually in force.

Two consequences worth knowing before you pin:

- **A pinned Serin Link that goes offline takes the room source down with it** —
  the source reports stale, then unavailable. That is deliberate: silently
  substituting a different room's temperature is the bug being fixed. If you
  want failover instead of correctness here, don't pin.
- **The MAC follows the physical unit**, so re-pairing the same Serin Link keeps
  the key working; swapping in a different one means editing it.

### Seeing the bond table

`diagnostics:` exposes what the controller knows about each bonded Serin Link, and
is what tells you the MAC to paste above:

```yaml
  diagnostics:
    bonded_count:    { name: "Bonded Serin Links" }
    pairing_status:  { name: "Pairing" }       # idle/listening/confirming/paired/
                                               # timeout/cancelled/full/pin-mismatch
    pairing_seconds: { name: "Pairing Window" }
    links:
      - mac_address: { name: "Serin Link 1 MAC" }
        linked:      { name: "Serin Link 1 Linked" }
        last_seen:   { name: "Serin Link 1 Last Seen" }
        firmware:    { name: "Serin Link 1 Firmware" }
```

Every child is optional and the block creates no entities you don't ask for.
`pairing_status` is worth having even alone: pressing pair on a full bond
table sets it to `full` and opens no window, which is otherwise
indistinguishable from success.

**A `links:` row is a bond slot, not a Serin Link.** Forgetting a Serin Link *compacts*
the table, so the Serin Link in slot 1 moves into slot 0 and "Serin Link 1 …" starts
describing it. Declaring `mac_address:` on every row is what makes that
visible rather than silent. Rows beyond the number of bonded Serin Links publish
empty/off/unknown.

The same table is printed at boot with no configuration at all:

```
[C][serin_link:...]   bonded Serin Links: 2
[C][serin_link:...]     [0] 10:51:DB:8E:EB:38  live  last seen 3 s   model 'Serin Link' fw '1.4.2'  caps_seq 7  cert 2
[C][serin_link:...]     [1] 10:51:DB:8E:F1:84  DOWN  last seen 412 s model 'Serin Link' fw '1.4.0'  caps_seq 7  cert 0
```

### Grouping each Serin Link as its own device

By default every entity lands on the controller's device, so `Serin Link 2 Last
Seen` sits in a flat list next to the heat pump's own sensors. ESPHome
sub-devices fix that with config alone — the component is unaware of them:

```yaml
esphome:
  name: serin-master
  devices:
    - id: link1
      name: "Serin Link 1"
    - id: link2
      name: "Serin Link 2"

serin_link:
  diagnostics:
    links:
      - mac_address: { name: "MAC",       device_id: link1 }
        linked:      { name: "Connected", device_id: link1 }
        last_seen:   { name: "Last Seen", device_id: link1 }
        firmware:    { name: "Firmware",  device_id: link1 }
      - mac_address: { name: "MAC",       device_id: link2 }
        linked:      { name: "Connected", device_id: link2 }
```

Home Assistant then shows a *Serin Link 1* device with `Connected`, `MAC`,
`Last Seen` and `Firmware` under it. Entity names can drop their prefix,
because the device already carries it.

Remember that a row is a bond slot: sub-device "Serin Link 1" is whichever
Link currently occupies slot 0, not a fixed unit. The `MAC` entity is what
tells you which one that is.

### Pairing and forgetting from automations

```yaml
button:
  - platform: template
    name: "Pair Serin Link"
    on_press:
      - serin_link.pair_start: { window: 60s }
  - platform: template
    name: "Forget Serin Link 1"
    on_press:
      - serin_link.forget_link: { slot: 0 }
```

Actions: `serin_link.pair_start` (`window:`, default 60 s, templatable),
`serin_link.pair_cancel`, `serin_link.forget_link`, and
`serin_link.forget_all_links`. `forget_link` takes **exactly one** of `slot:`
(a bond-table position, templatable) or `mac_address:` (one specific Serin Link,
resolved at compile time) — declaring both, or neither, is a config error.

For anything the schema doesn't expose (a Serin Link's model, `caps_seq`, or
cert state), `id(serin).dial_view(i, &v)` hands a lambda the raw snapshot.

### A note on "dial"

Everything you type or see — YAML keys, entity names, log lines — says **Serin
Link**. The C core and the wire spec keep the older *dial* vocabulary
(`sl2_dial_view_t`, `SL2_MAX_DIALS`, `DIAL_SENSOR`, `sl2_link_forget_dial`),
and so do the C++ identifiers that mirror them, because that vocabulary is
shared with the Serin Link firmware and both CN105 controller repos. If you
reach into a lambda you will meet it there; nowhere else.

`link_mac:` was called `dial_mac:` up to v0.1.3-beta.4. The old key still works
and logs a deprecation warning; it will be removed in a later release.

## Trust model

Honest summary — the spec's §3 has the full story:

- Both ends generate a **per-unit Ed25519 identity** on first boot. Pairing
  packets are signed, and each side **pins** the other's identity key at first
  bond (TOFU). A different key showing up later is refused until the user
  explicitly forgets the bond.
- All bonded traffic is **LMK-encrypted unicast** (per-bond key from an
  X25519 ephemeral exchange, HKDF-SHA256). Pairing is the only plaintext.
- `esp_now_set_pmk()` gets a **documented public constant** — it is not a
  secret and not a trust anchor (it never leaves the radio driver).
- Accepted risk, stated plainly: first contact during the button-gated pairing
  window is trust-on-first-use, the same posture as Zigbee permit-join. There
  is no CA and no "genuine device" gating — any firmware may implement this
  protocol.

## Layout

```
include/serin_link/   canonical headers (dependency-free C)
  sl2_proto.h         wire format: packets, enums, TLVs, ftab, transcripts
  sl2_crypto.h        crypto hooks (Ed25519/X25519) — bind libsodium/Monocypher
  sl2_sha256.h        SHA-256/HMAC/HKDF pinned in-tree (portable KDF)
  sl2_port.h          platform port: send/peers/clock/kv (≤250B MTU, 6B addr)
  sl2_bond.h          multi-dial bond-table codec (pure, host-tested)
  sl2_rxq.h           SPSC frame ring: radio callback -> loop context
  sl2_link.h          controller-role core API
src/sl2_link.c        the core: pairing+TOFU pinning, fan-out, pulls, liveness
docs/                 wire specification
test/run.sh           host test suite (plain gcc, no hardware)
esphome/              ESPHome component + example YAML
tools/sync_esphome.sh re-vendor the core into esphome/components/serin_link/
```

## ESPHome component details

Two modes:

- **`climate_id:` bound** — CAPS derive from the entity's `ClimateTraits`
  (modes, presets, discrete fan modes → detents, visual temp range/step,
  two-point target, target humidity), STATE from the entity's published state,
  its commands route through a `ClimateCall`. Generic entities have no
  positional vanes, so no vane axes are declared (the Serin Link hides those pages) —
  unless the platform exposes vanes as select entities: bind them with
  `vane_v_select:` / `vane_h_select:` (option order = wire positions;
  "auto"/"swing" options map to the wire codes) and the Serin Link gets both axes.
  A capability fingerprint (NVS) bumps `caps_seq` whenever the declared
  content changes across a reboot/reflash, so bonded Serin Links re-pull
  automatically. °C/°F display preference persists via ESPHome preferences.
- **Without `climate_id`** — canned capabilities and a wiggling room
  temperature: a coexistence smoke test for radio bring-up.

Notes:

- `esp-idf` framework required. Do **not** add an `espnow:` block — ESPHome's
  built-in component has no link-layer encryption and owns the recv callback;
  `serin_link`'s config validation rejects it.
- `hvac_link:` (optional lambda returning `bool`) binds the platform's
  device-link health to the Serin Link's offline face. A climate entity exists
  whether or not the device behind it answers, so without this a dead UART
  would read as "Connected" with a frozen temperature. CN105:
  `hvac_link: !lambda 'return id(hvac).isHeatpumpConnected();'`. Unset, a
  fallback heuristic reports link-down while an entity that claims a room
  temperature has none (NaN) — which catches never-connected devices on any
  platform; mid-run link loss still needs the lambda where the platform
  exposes a signal.
- Wire input is validated before it reaches the entity: setpoints clamp to
  the entity's visual range, out-of-range humidity is ignored, unknown
  modes/presets are no-ops.
- Serin Link edits are debounced: a CMD burst (e.g. scrolling through setpoints)
  merges into one `ClimateCall`, applied after `cmd_debounce:` (default
  `300ms`, `0s` = apply immediately) of quiet. The STATE echoed to Serin Links
  reflects the commanded values from the first CMD on — an optimistic
  overlay masks each field until the entity publishes it back (or a 10 s
  safety timeout lets the truth through), so async platforms like CN105
  can't snap the Serin Link back with a stale echo mid-adjustment.
- The component vendors flattened copies of the core (ESPHome compiles all
  sources in a component dir). Never edit those copies directly: edit the
  canonical files (`include/serin_link/`, `src/`), re-run
  `tools/sync_esphome.sh`, and commit both — CI fails on drift.
- Telemetry for the Serin Link's info pages binds through optional sensor keys
  (`outside_temp_sensor`, `compressor_hz_sensor`, `stage_sensor`,
  `sub_mode_sensor`, `auto_sub_mode_sensor`, `battery_sensor` (+
  `battery_low_threshold`, default 10%), `runtime_sensor`, `power_sensor`,
  `energy_sensor`) — each group sets its CAPS feature bit and INFO TLV; Wi-Fi,
  firmware, and system info are always served. Any platform's entities
  work; `example_cn105.yaml` shows the cn105 set.
- The Link-OTA credential relay (`SL2_FEAT_LINK_OTA_CREDS`) is not wired yet —
  the Serin Link hides its firmware-update path against this controller.
- Compile-verified against ESPHome 2026.6.5 / IDF 5.5.

## Adapting any controller firmware

Implement two structs, call three functions:

```c
sl2_link_init(&link, &port, &crypto, &hvac);
sl2_link_start(&link);          /* after Wi-Fi/radio is up */
/* every loop: drain your radio's rx into sl2_link_on_recv(), then */
sl2_link_loop(&link);
```

`sl2_port_t` is the platform (send/peers/clock/key-value store),
`sl2_hvac_iface_t` is your device (semantic state/apply/caps). The core owns
everything else: pairing, crypto policy, bond storage, scheduling, fan-out.
See the spec's §12 and the ESPHome component as a worked example.
[mitsubishi-cn105-homekit](https://github.com/akifbayram/mitsubishi-cn105-homekit)
is the reference non-ESPHome adopter.

## Tests

```
test/run.sh
```

Plain gcc, no hardware. Covers: wire layout offsets, tolerant short/long
decode, TLV codec, the Mitsubishi °F table, pairing transcripts, bond-blob
codec, and the full core FSM against a fake port with deterministic toy crypto
— pair/reboot/persist, pin-mismatch refusal, bad-signature drop, bond-table
limit, re-key timeout, STATE cadence (first-live/change/heartbeat/pull/offline),
CMD apply + echo-to-all-Links, CAPS/INFO pulls, credential relay, forget, and
stranger/broadcast rejection. CI runs the suite plus an ESPHome compile on
every push.

Real-curve crypto interop happens on hardware (libsodium on the Serin Link,
vendored Monocypher on the ESPHome side); the toy
crypto pins FSM logic only.

## License

Apache-2.0 — see [LICENSE](LICENSE).
