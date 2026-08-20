# ESPHome Link-OTA credential relay — design

**Date:** 2026-08-20
**Repos touched:** `serin-link-core` only. `serin-link` (dial) is deliberately
untouched — that is a feature of the design, not an accident.

## Problem

A Serin Link paired with the native Serin Controller updates itself from
Settings → About → Details → Update: it asks the bonded controller for STA
credentials over the encrypted link (`WIFI_REQ`/`WIFI_RESP`), joins, fetches
the Serin-hosted manifest, and installs an RSA-encrypted, SHA-256-re-verified
image. A Link paired with an ESPHome node has no update path at all: the
component leaves `hvac_.wifi_creds = nullptr` (`serin_link.cpp:1198`) and
never sets `SL2_FEAT_LINK_OTA_CREDS`, so the dial — which gates its Update
pill on exactly that CAPS bit (`ui_about.c`) — hides the entry entirely. USB
is the only way to update such a dial today.

Everything downstream of the credentials is already controller-agnostic. The
manifest URL is compiled into the dial; the image is encrypted against a key
embedded in the dial and signed; the written slot is hash-checked before the
boot switch. The node supplies a network and nothing else — it never touches
the trust chain and relays nothing it could tamper with.

## Decision

Implement the credential relay in the ESPHome component, **opt-in via YAML,
default off**:

```yaml
serin_link:
  id: serin
  climate_id: hvac
  link_ota_credentials: true   # default false: hand STA creds to bonded Links
```

Chosen over an HA-switch runtime gate (worse discoverability at the wall for
a twice-in-a-device's-life event) and over a node-relayed image push (new
protocol surface, ~1.5 MB over ESP-NOW, dial firmware changes). Both remain
possible later; neither blocks on this.

### No dial change, no re-pair

The dial reads `SL2_FEAT_LINK_OTA_CREDS` out of CAPS at runtime, and the
component already fingerprints CAPS content and bumps `caps_seq` when it
changes across a reboot/reflash. So the sequence for a fielded dial is: user
bumps their `external_components` pin, adds the YAML key, reflashes the node
— the fingerprint differs, `caps_seq` bumps, every bonded Link re-pulls CAPS
and grows the Update pill. No dial reflash, no re-pair.

## Component changes

### 1. Config surface (`__init__.py`)

- `CONF_LINK_OTA_CREDENTIALS = "link_ota_credentials"`,
  `cv.Optional(..., default=False): cv.boolean` on the base schema.
- Codegen: `cg.add(var.set_link_ota_credentials(True))` only when true.

### 2. EAP guard (final validate)

If `link_ota_credentials: true` and the `wifi:` config contains **any**
`eap:` network, reject at config time with a message that names the key and
explains itself (the harness requires rejection *reasons*, not just
rejections):

> `link_ota_credentials:` relays an SSID+PSK to the Serin Link, and a
> WPA-Enterprise network has no PSK to relay — the Link cannot join it.
> Remove `link_ota_credentials:` or the `eap:` network.

Strict on *any* EAP network, not "all networks EAP": ESPHome normalizes a
single `ssid:` into the `networks:` list, and at runtime the node may be
associated to the EAP network, at which point the relay would hand out dead
credentials. Mixed PSK/EAP configs are rare; strictness beats a runtime
maybe. Wire it alongside `_no_builtin_espnow` in `FINAL_VALIDATE_SCHEMA`
(reading the `wifi:` block via the existing `fv.full_config.get()` idiom).

Open networks (non-empty SSID, empty password) are legitimate and relayed
as-is — the dial joins open networks fine.

### 3. Runtime hook (`serin_link.h` / `serin_link.cpp`)

- `bool link_ota_credentials_{false};` +
  `void set_link_ota_credentials(bool v)` (matches the existing setter idiom).
- `t_get_caps` (`serin_link.cpp:612` region): OR in
  `SL2_FEAT_LINK_OTA_CREDS` when the flag is set. The CAPS fingerprint covers
  the rest.
- Replace the `nullptr` with a `h_wifi_creds`-style static trampoline:

```c
static bool t_wifi_creds(void *ctx, char ssid[33], char psk[65]) {
  wifi_config_t wc;
  if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
  if (wc.sta.ssid[0] == 0) return false;          /* nothing stored */
  memcpy(ssid, wc.sta.ssid, 32);  ssid[32] = 0;   /* driver fields are   */
  memcpy(psk,  wc.sta.password, 64); psk[64] = 0; /* not NUL-terminated  */
  memset(&wc, 0, sizeof wc);                      /* no PSK copy behind  */
  return true;
}
```

  Installed only when `link_ota_credentials_` is set (a nullptr hook makes
  the core answer `ok=0`, the correct degraded behavior if the bit and the
  hook ever disagree).

`esp_wifi_get_config(WIFI_IF_STA)` — the live driver config — is chosen over
walking ESPHome's `wifi:` model on purpose: it is whatever network the node
*actually joined* (correct under multi-network roaming and for credentials
saved via the captive portal, which never appear in YAML), and it keeps the
component decoupled from WiFiComponent internals. The hook runs on the main
loop task like every other `hvac_` hook; `esp_wifi_get_config` is safe there.
The core already zeroizes the `WIFI_RESP` struct after send, and the dial
holds the creds in RAM only and scrubs them after the join.

## What the update path looks like end to end

Unchanged from the native controller, which is the point: user presses
Update on the dial → dial sends `WIFI_REQ` (encrypted unicast; the dial only
accepts `WIFI_RESP` during its few-second wait) → node answers with the STA
creds → dial pauses ESP-NOW, joins the home network, SNTP, fetches
`https://raw.githubusercontent.com/Serin-Labs/serin-cn105/.../manifest.json`,
confirms with the user, downloads/decrypts/verifies, reboots, rejoins the
link. During the download the node's `connected`/diagnostics rows for that
Link go down and self-heal after reboot (bonds are NVS-persistent).

## Documentation

- **README**: a `### Letting the Serin Link update itself` section under the
  ESPHome details — the key, the one-button UX on the dial, and the honest
  caveats: PSK goes to a signed-pair-bonded Link only, RAM-only both ends,
  zeroized after use, encrypted unicast (same posture as the native
  controller); the Link needs internet + SNTP reachability to
  `raw.githubusercontent.com` (a no-internet IoT VLAN fails at "Update check
  failed"); the Link drops off the ESP-NOW link for the duration; EAP is
  rejected at config time.
- **README trust model**: one bullet stating the relay exists, is opt-in,
  and what it hands to whom.
- **`example_package.yaml`**: a commented-out `link_ota_credentials: true`
  line. `packages/cn105.yaml` does **not** enable it — credential handover
  stays an explicit per-config decision.
- **Wire spec**: no change — §10 already anticipates exactly this ("an
  ESPHome adapter reads its own STA creds; a build may decline with the bit
  unset").

## Tests

- `test/esphome/pass_link_ota_creds.yaml` — key + PSK `wifi:` network,
  must validate.
- `test/esphome/fail_link_ota_creds_eap.yaml` — key + `eap:` network,
  `MUST_REJECT_WITH` a phrase unique to the guard (e.g. `no PSK to relay`;
  the config-echo caveat in the harness rules out matching on the key name).
- C core: untouched — `sl2_link.c`'s `WIFI_REQ` → `wifi_creds` → `WIFI_RESP`
  path already exists and ships in the native controller.

## Bench validation

Node: `~/heatpump-master.yaml` (already on serin-link-core). Dial: bench
dial #3 (`10:51:db:8e:ea:b0`, build_viewe15, feeds HA).

1. Add the key, `esphome run` the node (OTA).
2. Confirm the Update pill appears on the dial's About → Details **without
   re-pairing** (proves the fingerprint-driven `caps_seq` bump).
3. Run the update check against the live manifest through to the CONFIRM
   face, then **cancel**: dial #3 currently carries an uncommitted
   thermal-cal build (286 cc offset) that a real install would wipe. The
   downloaded-image path itself is unchanged dial code, already proven
   against the native controller.
4. Confirm the dial rejoins the link and HA rows recover after the cancel.

## Release

Next beta tag (`v0.1.4-beta.2`); README quickstart pin bumped alongside.

## Out of scope (deliberately)

- HA-switch runtime gate for the FEAT bit (shape B) — can layer on later.
- HA-initiated install push (shape C) — new protocol surface, dial changes,
  and a UX question about remotely blanking a wall dial; revisit after A is
  in the field.
- `WIFI_SETUP` / change-network hosting on ESPHome nodes — unrelated flow.
- Ethernet-only nodes — already excluded by `DEPENDENCIES = ["wifi"]`.
