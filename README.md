# serin-link-core

The controller-side reference implementation of **Serin Link** — the
encrypted ESP-NOW protocol a [Serin dial](https://github.com/Serin-Labs) (a
wall-mount rotary thermostat head) uses to pair with and control heat-pump /
HVAC controllers. Any ESP32 firmware that embeds this core becomes a zone a
Serin dial can control: the dial builds its whole UI from the capabilities the
controller declares, so nothing about it is vendor-specific.

The repo ships two things:

- **`libserinlink`** — a dependency-free C library implementing the
  controller role (signed pairing with trust-on-first-use key pinning,
  multi-dial bond table, state fan-out, capability descriptors), portable to
  any platform through a small port/crypto interface.
- **An ESPHome component (`serin_link`)** that binds the core to *any* ESPHome
  `climate` entity — Mitsubishi CN105, generic `thermostat`, midea, daikin,
  gree, … — turning the device into a Serin-dial zone with a few lines of YAML.

The wire protocol is specified in
[`docs/serin-link-wire-spec.md`](docs/serin-link-wire-spec.md) (current wire
version: 2, `SL2_PROTO_VERSION`) and hardware-verified against ESPHome (CN105
and generic climate platforms) and the native Serin Controller firmware.

## ESPHome quickstart

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf   # required: raw nvs_*, esp_now encrypted peers, libsodium

external_components:
  - source: github://Serin-Labs/serin-link-core@v0.1.0
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
    name: "Pair Serin Dial"
    on_press:
      - lambda: 'id(serin).pair_start(60000);'
```

Put the dial in pairing mode, press **Pair Serin Dial** (Home Assistant or the
ESPHome web UI) within the 60 s window, done. The dial pulls the zone's
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
  sl2_crypto.h        crypto hooks (Ed25519/X25519) — bind libsodium
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
  dial commands route through a `ClimateCall`. Generic entities have no
  positional vanes, so no vane axes are declared (the dial hides those pages) —
  unless the platform exposes vanes as select entities: bind them with
  `vane_v_select:` / `vane_h_select:` (option order = wire positions;
  "auto"/"swing" options map to the wire codes) and the dial gets both axes.
  A capability fingerprint (NVS) bumps `caps_seq` whenever the declared
  content changes across a reboot/reflash, so bonded dials re-pull
  automatically. °C/°F display preference persists via ESPHome preferences.
- **Without `climate_id`** — canned capabilities and a wiggling room
  temperature: a coexistence smoke test for radio bring-up.

Notes:

- `esp-idf` framework required. Do **not** add an `espnow:` block — ESPHome's
  built-in component has no link-layer encryption and owns the recv callback;
  `serin_link`'s config validation rejects it.
- `hvac_link:` (optional lambda returning `bool`) binds the platform's
  device-link health to the dial's offline face. A climate entity exists
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
- The component vendors flattened copies of the core (ESPHome compiles all
  sources in a component dir). Never edit those copies directly: edit the
  canonical files (`include/serin_link/`, `src/`), re-run
  `tools/sync_esphome.sh`, and commit both — CI fails on drift.
- The Link-OTA credential relay (`SL2_FEAT_LINK_OTA_CREDS`) is not wired yet —
  the dial hides its firmware-update path against this controller.
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
See the spec's §12 and the ESPHome component as a worked example. The native
Serin Controller firmware is the reference non-ESPHome adopter.

## Tests

```
test/run.sh
```

Plain gcc, no hardware. Covers: wire layout offsets, tolerant short/long
decode, TLV codec, the Mitsubishi °F table, pairing transcripts, bond-blob
codec, and the full core FSM against a fake port with deterministic toy crypto
— pair/reboot/persist, pin-mismatch refusal, bad-signature drop, bond-table
limit, re-key timeout, STATE cadence (first-live/change/heartbeat/pull/offline),
CMD apply + echo-to-all-dials, CAPS/INFO pulls, credential relay, forget, and
stranger/broadcast rejection. CI runs the suite plus an ESPHome compile on
every push.

Real-curve crypto interop happens on hardware (libsodium both ends); the toy
crypto pins FSM logic only.

## License

Apache-2.0 — see [LICENSE](LICENSE).
