# serin-link-core

**Serin Link** is an encrypted ESP-NOW protocol connecting the
[Serin Link](https://github.com/Serin-Labs) rotary control to heat pump and HVAC
controllers. This repository provides:

- **`libserinlink`** — a portable, dependency-free C controller library for signed pairing,
  persistent bonds, state distribution, and capability reporting.
- **An ESPHome component (`serin_link`)** that connects the core to an ESPHome
  `climate` entity, including Mitsubishi CN105, `thermostat`, `midea`, `daikin`,
  and `gree` platforms.

The Link displays the capabilities declared by its controller. New pairing
requires [wire protocol v5](docs/serin-link-wire-spec.md) on both peers.

## Contents

- [ESPHome quickstart](#esphome-quickstart)
- [The Serin Link's own temperature sensor](#the-serin-links-own-temperature-sensor)
- [More than one Serin Link](#more-than-one-serin-link)
- [Screen on/off from Home Assistant](#screen-onoff-from-home-assistant)
- [Night brightness](#night-brightness)
- [Pairing and forgetting from automations](#pairing-and-forgetting-from-automations)
- [Trust model](#trust-model)
- [ESPHome component details](#esphome-component-details)
- [Adapting any controller firmware](#adapting-any-controller-firmware)
- [Tests](#tests)
- [License](#license)

## ESPHome quickstart

Requires ESPHome 2026.3.0 or newer with ESP-IDF. Merge this fragment into an
existing node, retaining its name, Wi-Fi settings, and climate configuration.
Use `api:` for Home Assistant access or `web_server:` for the web interface.

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf   # required by serin_link

external_components:
  - source: github://Serin-Labs/serin-link-core@v0.1.6
    components: [serin_link]

climate:
  - platform: thermostat   # use your existing climate platform
    id: hvac
    # Retain the platform's required settings and actions.

serin_link:
  id: serin
  climate_id: hvac
  pair_button:     # a component-owned "Pair Serin Link" button, default 60 s
```

Press **Pair Serin Link**, then start pairing on the Link within 60 seconds.
For access without the network, configure a
[physical pairing button](#pairing-and-forgetting-from-automations).

Controls follow the climate entity's modes, visual temperature range, and
discrete fan speeds. Fan Auto requires `CLIMATE_FAN_AUTO`; `CLIMATE_FAN_ON`
alone provides no fan control.

Complete examples in [`esphome/`](esphome/):

- [`example_package.yaml`](esphome/example_package.yaml) — recommended CN105
  configuration for two Links, using [`packages/cn105.yaml`](esphome/packages/cn105.yaml).
- [`example_cn105.yaml`](esphome/example_cn105.yaml) — expanded configuration using
  [echavet/MitsubishiCN105ESPHome](https://github.com/echavet/MitsubishiCN105ESPHome),
  including vane-axis bindings, device-link health, and per-Link sub-devices.
- [`example_generic.yaml`](esphome/example_generic.yaml) — the core
  `thermostat` platform with the Link's temperature as its control input.
- [`example_spike.yaml`](esphome/example_spike.yaml) — simulated state for radio
  and pairing tests without a climate entity.
- [`bench_band.yaml`](esphome/bench_band.yaml) /
  [`bench_dryrun.yaml`](esphome/bench_dryrun.yaml) /
  [`bench_cn105.yaml`](esphome/bench_cn105.yaml) — test configurations for
  dual-setpoint (HEAT_COOL) zones, simulated CN105 controls, and a CN105
  controller with a Home Assistant temperature sensor and diagnostics.

Outside this checkout, replace local component and package references with
the examples' `v0.1.6` URLs. Use the same release for both.

### The Serin Link's own temperature sensor

`link_sensor:` publishes the Link's temperature and humidity to Home Assistant
and enables selection under **Settings → Room Sensor**:

```yaml
serin_link:
  id: serin
  climate_id: hvac
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
    humidity:
      name: "Serin Link Humidity"
```

To supply the selected reading to the heat pump, use `on_room_temperature:`.
It receives `x` in °C, or `0` when `Heat pump` is selected. CN105 uses `0` to
restore its internal sensor; other climate platforms must implement their
own handling of this reset value.

For CN105, set a finite `remote_temperature_timeout` so lost readings trigger
fallback to the internal sensor. The
[upstream default is `never`](https://github.com/echavet/MitsubishiCN105ESPHome/blob/4b63f3b4fab5ec55e8c8b3e917440a6e829fe04a/components/cn105/climate.py#L547).
Merge these settings into the existing configuration:

```yaml
climate:
  - platform: cn105
    id: hvac
    name: "Heat Pump"
    remote_temperature_timeout: 3min
    # Retain the existing UART and other climate settings.

serin_link:
  climate_id: hvac
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
    on_room_temperature:
      - lambda: 'id(hvac).set_remote_temperature(x);'
```

In [`example_generic.yaml`](esphome/example_generic.yaml), the thermostat reads
the Link sensor directly and has no fallback sensor. An unknown reading requests
OFF, subject to the configured five-minute minimum heating run time.

### More than one Serin Link

A controller supports up to four bonded Serin Links. Set the installation's
limit with `max_links:`:

```yaml
serin_link:
  max_links: 2
```

`max_links:` generates MAC, Connected, Last Seen, and Firmware entities for
each slot and limits new bonds. At capacity, existing Links may pair again;
new bonds are refused with status `full`. Remove a bond or increase the limit to add one.

All bonded Links receive state updates and can issue commands. Select the
room-temperature source from the Link's Room Sensor menu or add a Home
Assistant selector:

```yaml
  room_temperature_source:
    name: "Room Temperature Source"
```

The selection is shared by Home Assistant and all paired Links. Options are
`Heat pump`, configured external sources, and bonded Links. Home Assistant
reserves one Link entry per `max_links:` slot; selecting an empty slot has no
effect.

Changing the selected Link clears the shared temperature and humidity until
that Link reports a fresh reading, even when returning to a previous selection.
Per-Link entities retain their own readings.

Add external temperature sensors under `room_temperature_source.sources`.
This fragment adds a Home Assistant source to the CN105 configuration above:

```yaml
sensor:
  - platform: homeassistant
    id: ha_room_temp
    name: "Home Assistant"
    entity_id: sensor.living_room_temperature
    unit_of_measurement: "°C"         # the HA entity's unit: °C or °F

serin_link:
  link_sensor:
    on_room_temperature:
      - lambda: 'id(hvac).set_remote_temperature(x);'
  room_temperature_source:
    sources:
      - sensor: ha_room_temp          # label defaults to the sensor's name
```

Set `unit_of_measurement:` to match the source: `°C` or `°F`. Fahrenheit is
converted to Celsius. An omitted unit is treated as Celsius with a warning;
other units are rejected. Home Assistant sensors do not inherit their entity's
unit. `battery_sensor:` is battery telemetry, not a temperature source.

External source health depends on a valid reading and, when the ESPHome API is
enabled, a Home Assistant connection. This connection requirement also applies
to locally attached external sensors. An unchanged reading remains valid;
selected valid readings are resent every 30 seconds to prevent a timeout.

A valid cached reading is used when an external source is selected. Otherwise,
the source waits for a fresh reading without issuing a temperature or reset
value. The previous source stops feeding the callback, and the climate's
remote-temperature timeout controls fallback.

Link selections are stored by MAC address, while entity names use bond slots.
Removing a bond compacts the table:

- Removing an earlier Link changes the selected Link's slot label, but keeps
  the same device selected.
- Removing the selected Link immediately selects `Heat pump` and sends `0`
  through `on_room_temperature:`, whether or not a Home Assistant selector exists.

**Forget All** also cancels pairing. Removing one Link cancels only that Link's
pending handshake. A failed removal preserves the bonds and pairing state.

Link readings normally arrive every 20 seconds and become stale after 90
seconds of silence. An offline Link remains selected; the controller does not
substitute another room's reading.

### One temperature per Serin Link

To display every Link's readings in Home Assistant, add `links:` under
`link_sensor:`:

```yaml
  room_temperature_source:
  link_sensor:
    temperature:                 # shared entity for the selected Link
      name: "Serin Link Temperature"
    links:                       # temperature and humidity for each slot
```

A bare `links:` key generates temperature and humidity entities for each
`max_links:` slot. Use `link_devices:` to group them under separate devices,
or declare custom names with one row per slot:

```yaml
  link_sensor:
    links:
      - temperature: { name: "Bedroom Temperature" }
        humidity:    { name: "Bedroom Humidity" }
      - temperature: { name: "Office Temperature" }
```

Either sensor may be omitted from a custom row. These entities do not affect
the selected control source. They become unknown after 90 seconds without a
reading, or when a slot changes occupants, until the new occupant reports.

### Seeing the bond table

Use `diagnostics:` for controller-wide status and optional custom slot names:

```yaml
  diagnostics:
    connected:       # ON when any bonded Serin Link is communicating
    bonded_count:    { name: "Bonded Serin Links" }
    pairing_status:  { name: "Pairing" }       # idle/listening/confirming/paired/
                                               # timeout/cancelled/full/pin-mismatch/storage-error
    pairing_seconds: { name: "Pairing Time Left" }  # 1 Hz countdown, 0 when closed
    # links: [...]   # optional custom rows; count must match max_links
```

All entries are optional. Include `mac_address:` on custom rows to identify
the current slot occupant. Unoccupied rows show empty strings, OFF, or unknown.
The bond table is also logged at startup. Pairing status `full` remains visible
after the window expires; without the status entity, it appears only in logs.

### ESPHome transport recovery

Set `power_save_mode: NONE` under `wifi:` to keep the radio awake from startup.
For connection problems, enable `DEBUG` logging for `serin_link` and follow the
[transport recovery guide](docs/esphome-transport-recovery.md).

### Screen on/off from Home Assistant

`screen:` controls all paired displays. ON keeps the glance display visible
and wakes a sleeping Link. OFF allows sleep after about 30 seconds without
input, or immediately if already idle; touch still wakes the display. The
switch restores its saved state and defaults to ON:

```yaml
serin_link:
  id: serin
  climate_id: hvac
  max_links: 1
  screen:          # "Serin Link Screen" switch (default name)
```

A Home Assistant motion automation can turn this switch ON when occupied and
OFF after a delay. Configure that automation in Home Assistant, separately
from the ESPHome YAML.

With `max_links:` set, each diagnostics row includes a "Screen" binary sensor
for the actual display state. Older firmware without this feature ignores
the switch and leaves the status sensor off.

### Night brightness

`night:` adds a switch that limits display brightness on every paired Link
while the switch is ON:

```yaml
serin_link:
  night:           # "Serin Link Night" switch (default name)
```

It restores the saved state, defaulting to OFF. In Home Assistant, an
automation can follow `sun.sun` being `below_horizon`. In ESPHome, use
[`sun:` triggers](https://esphome.io/components/sun/#triggers): `on_sunset` turns
the switch ON and `on_sunrise` turns it OFF. Set `elevation: -6°` for civil
twilight. Reconcile with the current sun position after startup or reconnection
to account for missed transitions; ESPHome needs synchronized time first.

### Grouping each Serin Link as its own device

Generated entities belong to the controller's Home Assistant device by
default. Use `link_devices:` to group each slot under a separate sub-device:

```yaml
esphome:
  name: serin-master
  devices:
    - id: link1
      name: "Serin Link 1"
    - id: link2
      name: "Serin Link 2"

serin_link:
  max_links: 2
  link_devices: [link1, link2]   # one per slot, in slot order
```

For explicitly declared `links:` rows, set `device_id:` on each entity.
`link_devices:` applies only to generated rows.

### Pairing and forgetting from automations

`pair_button:` provides the network-accessible button; `window:` defaults to
60 seconds. For a physical button, use the example below and select your
board's pin. Hold for 2–10 seconds, then release:

```yaml
binary_sensor:
  - platform: gpio
    id: pair_btn          # unnamed sensors are internal to ESPHome
    pin:
      number: GPIO41      # AtomS3 Lite front button; adjust for your board
      inverted: true
      mode: { input: true, pullup: true }
    filters:
      - delayed_off: 10ms
    on_click:
      min_length: 2s
      max_length: 10s
      then:
        - serin_link.pair_start: { window: 60s }
```

Shorter and longer presses are ignored. To remove a bond:

```yaml
button:
  - platform: template
    name: "Forget Serin Link 1"
    on_press:
      - serin_link.forget_link: { slot: 0 }
```

Actions: `serin_link.pair_start` (`window:`, default 60 s, templatable),
`serin_link.pair_cancel`, `serin_link.forget_link`, and
`serin_link.forget_all_links`. `forget_link` takes **exactly one** of `slot:`
(a bond-table position, templatable) or `mac_address:` (one specific Serin Link,
resolved at compile time).

## Trust model

- Each peer has a **per-unit Ed25519 identity**. Signed pairing records the
  peer's key at the first bond (TOFU); a different key is refused until that
  bond is removed. Pairing requires a user-opened window, but no certificate
  authority or manufacturer certificate.
- Bonded traffic uses **LMK-encrypted unicast**, with a per-bond key derived
  through X25519 and HKDF-SHA256. Pairing requests and responses are plaintext
  broadcasts; key confirmation is encrypted.
- The ESP-NOW PMK is a public constant used inside the radio driver, not a
  trust anchor.
- Wi-Fi credential sharing requires `link_ota_credentials:` to be enabled.

See §3 of the [wire specification](docs/serin-link-wire-spec.md) for details.

## ESPHome component details

- Do not add ESPHome's built-in `espnow:` component. It cannot share the radio
  callback with `serin_link` and lacks link-layer encryption.
- `vane_v_select:` and `vane_h_select:` bind positional vanes to select
  entities. Option order defines positions; "auto" and "swing" are recognized
  separately. Unbound axes are omitted from the Link's controls.
- `hvac_link:` accepts a Boolean lambda for device connection health. For CN105,
  use `hvac_link: !lambda 'return id(hvac).isHeatpumpConnected();'`.
  Alternatively, bind a binary sensor with `hvac_link_sensor:`; the two are
  mutually exclusive. Without either, an unavailable room temperature reports
  the device offline, but a retained reading can conceal a lost connection.
- `zone_name:` defaults to the climate entity's name, then the device's
  friendly name.
- `fahrenheit_table:` accepts `auto` (default), `linear`, or `mitsubishi`.
  Auto selects the Mitsubishi scale when the visual step is 0.5 °C and the
  range is within 16–31 °C; otherwise it uses linear conversion. On the
  Mitsubishi scale, 72 °F represents 22.5 °C and setpoints use a 0.5 °C grid.
  CN105 compatibility modes such as `fahrenheit_compatibility: standard`
  require `fahrenheit_table: linear`. See the
  [CN105 modes](https://github.com/echavet/MitsubishiCN105ESPHome/blob/4b63f3b4fab5ec55e8c8b3e917440a6e829fe04a/components/cn105/climate.py#L162).
  The °C/°F display preference persists across restarts.
- Commands are combined after 300 ms without further input. Requested values
  appear immediately and remain until confirmed or a 10-second timeout expires.
- Optional telemetry bindings are `outside_temp_sensor`, `compressor_hz_sensor`,
  `stage_sensor`, `sub_mode_sensor`, `auto_sub_mode_sensor`, `battery_sensor`,
  `runtime_sensor`, `power_sensor`, and `energy_sensor`. Battery readings set
  the low-battery flag at 10% and clear it at 15%. Wi-Fi, firmware, and system
  information are always provided. See the [CN105 example](esphome/example_cn105.yaml).

### Letting the Serin Link update itself

Enable **Settings → About → Details → Update** on supported Links:

```yaml
serin_link:
  id: serin
  climate_id: hvac
  link_ota_credentials: true   # default false
```

This setting shares the controller's Wi-Fi SSID and PSK with a bonded Link
on request over encrypted unicast. It is disabled by default. The Link keeps
received credentials in RAM, clears them after use, and downloads and verifies
its own firmware after the user confirms the version.

- The network must allow DNS, HTTPS, and NTP. WPA-Enterprise (`eap:`) networks
  are rejected because the relay requires an SSID and PSK.
- ESP-NOW communication pauses during the update and restart. Saved bonds
  remain valid; pairing again is not required.

## Adapting any controller firmware

Provide three interfaces:

| Interface | Responsibilities |
| --- | --- |
| `sl2_port_t` | Datagram transport, peer configuration, clock, and persistent key-value storage. |
| `sl2_crypto_t` | Secure random generation, Ed25519 signatures, and X25519 key exchange. |
| `sl2_hvac_iface_t` | Device state, command application, capabilities, and optional telemetry. |

The crypto interface can use libsodium or Monocypher. SHA-256, HMAC, and HKDF
are included in the core. Initialize the link after Wi-Fi and the radio are
ready, and check the startup result. For example, in a startup function that
returns `bool`:

```c
sl2_link_init(&link, &port, &crypto, &hvac);
if (!sl2_link_start(&link)) {
    /* Report the failure and arrange a retry before processing packets. */
    return false;
}
```

After successful startup, drain the receive queue into `sl2_link_on_recv()`
and call the loop function regularly:

```c
sl2_link_loop(&link);
```

`sl2_link_on_recv()` and `sl2_link_loop()` must execute in the same context.
Use the queue in `sl2_rxq.h` when radio callbacks run in a separate task.
Keep the interface structures available for the lifetime of the link.

See §12 of the [wire specification](docs/serin-link-wire-spec.md) for the full
contract. Reference adapters are the ESPHome component and
[mitsubishi-cn105-homekit](https://github.com/akifbayram/mitsubishi-cn105-homekit).

## Tests

```
test/run.sh
RUN_TSAN=1 test/run.sh  # also check the two-task RX queue with ThreadSanitizer
```

The host suite requires gcc and g++. It covers protocol handling, pairing,
persistence, climate controls, source selection, transport recovery, and
receive-queue concurrency. It also checks Monocypher Ed25519/X25519 and
in-tree HKDF against standard test vectors. ThreadSanitizer requires a
supported host runtime; radio interoperability still needs hardware testing.

CI runs the host suite, source synchronization and schema checks, and compiles
`example_spike.yaml` with ESPHome 2026.6.5.
See [CONTRIBUTING.md](CONTRIBUTING.md) for development instructions.

## License

Apache-2.0 — see [LICENSE](LICENSE).
