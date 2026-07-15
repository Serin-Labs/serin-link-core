# Serin Link — wire specification

**Status:** normative for `SL2_PROTO_VERSION 2`; matches
`include/serin_link/sl2_proto.h` (the header is the byte-level ground truth —
every packed struct there carries a `sizeof` static assert). Wire version 1
was a pre-release draft that never shipped; version 2 is the first released
protocol, and `SL2_PROTO_MIN_COMPAT 1` simply marks the floor of the version
space.

Design goals, in priority order:

1. **Firmware-agnostic and heat-pump-agnostic.** No vendor bytes on the wire;
   the controller owns vendor mapping. The dial renders what `CAPS` declares.
2. **TOFU + key pinning, no CA.** Per-unit Ed25519 identity on both ends, signed
   handshake, pinned on first bond. No brand secret anywhere.
3. **Future-proof within reason.** Semantic superset of ESPHome `ClimateTraits` /
   HomeKit thermostat features (dual setpoint, humidity, presets, action);
   additive-growth discipline everywhere; u16 masks where u8 would plausibly
   fill; TLV registry with vendor space; core is transport-agnostic bytes.
4. **Proven transport mechanics:** ESP-NOW, LMK-encrypted unicast, probe
   cadence and pull-gating, channel sweep/memory, MAC-ACK liveness, tolerant
   MIN_LEN decode.

---

## 1. Transport

ESP-NOW on `WIFI_IF_STA`, ≤250 B payloads, little-endian packed structs (encode ==
memcpy). Pairing packets are broadcast plaintext; all bonded traffic is unicast
with `esp_now_peer_info_t.encrypt = true` and the per-bond LMK. `esp_now_set_pmk()`
is called with a **documented public constant** `"serin-link-open"` padded to 16 B
— it only wraps LMKs locally (per Espressif docs it never goes on air and does not
protect payloads); it is NOT a secret and NOT the pairing trust anchor.

Channel model: the controller follows its AP; the dial sweeps 1–13, remembers
`last_channel` per bond, self-heals on confirmed ACKs.

**Transport-agnostic note:** nothing below assumes ESP-NOW beyond (a) a ≤250 B
MTU and (b) 6-byte peer addresses, both abstracted behind the `libserinlink` port
(§12). A future BLE/Thread/UDP carrier reuses the packet layer unchanged; only
pairing discovery (channel sweep) is carrier-specific and lives in the port.

Header shared by every packet:

```c
uint8_t type;      /* SL2_PKT_* */
uint8_t version;   /* SL2_PROTO_VERSION */
```

```c
#define SL2_PROTO_VERSION    2
#define SL2_PROTO_MIN_COMPAT 1
```

**Growth discipline (normative):** changes are additive only — claim a spare
flag bit, then a `reserved[]` byte, then append after `reserved[]` — and bump
`VERSION` alone; layouts already shipped are wire-frozen. Receivers accept any
packet at least `*_MIN_LEN` long (the floor-era prefix) and decode by
zero-filling the destination struct then prefix-copying the received bytes
(`sl2_decode_pkt`), so short packets from older peers and long packets from
newer peers both parse. Unknown enum values, mask bits, and TLV types are
ignored, never errors.

## 2. Packet types

| # | Name | Dir | Encrypted | Purpose |
|---|------|-----|-----------|---------|
| 1 | `STATE` | ctrl→dial | yes | ~1 Hz semantic status |
| 2 | `CMD` | dial→ctrl | yes | masked semantic write |
| 3 | `PROBE` | dial→ctrl | yes | keepalive + pull gates |
| 4 | `PAIR_REQ` | dial→bcast | no (signed) | pairing request |
| 5 | `PAIR_RESP` | ctrl→bcast | no (signed) | pairing response |
| 6 | `CAPS` | ctrl→dial | yes | capability descriptor + zone name |
| 7 | `INFO` | ctrl→dial | yes | TLV telemetry stream (pull-only) |
| 8 | `WIFI_REQ` | dial→ctrl | yes | Link OTA: request STA creds |
| 9 | `WIFI_RESP` | ctrl→dial | yes | Link OTA: creds reply |
| 10 | `WIFI_SETUP` | dial→ctrl | yes | raise setup hotspot (change network) |
| 11 | `DIAL_INFO` | dial→ctrl | yes | dial identity (model/fw/caps_seq) for the controller's UI |

Types 12–127 are reserved for core growth; 128–255 are reserved for experiments
(never shipped semantics).

## 3. Pairing: signed X25519 + TOFU pinning

Both ends hold a **long-lived Ed25519 identity keypair**, generated on first boot
and persisted (controller: its KV store; dial: NVS). No CA, no certs, no badge —
the signature proves *continuity of identity*, not brand provenance. Each side
pins the other's identity pubkey at first bond; substitution later fails loudly.

```c
struct sl2_pair_req_pkt {          /* dial -> broadcast, 137 B */
    uint8_t type, version;
    uint8_t src_mac[6];            /* dial STA MAC */
    uint8_t eph_pub[32];           /* fresh X25519 ephemeral, per attempt */
    uint8_t id_pub[32];            /* dial's long-lived Ed25519 identity */
    uint8_t sig[64];               /* Ed25519(dial_id_priv, req_transcript) */
    uint8_t reserved[1];
};
struct sl2_pair_resp_pkt {         /* ctrl -> broadcast, 137 B */
    uint8_t type, version;
    uint8_t src_mac[6];            /* controller STA MAC */
    uint8_t eph_pub[32];           /* fresh X25519 ephemeral */
    uint8_t id_pub[32];            /* controller's Ed25519 identity */
    uint8_t sig[64];               /* Ed25519(ctrl_id_priv, resp_transcript) */
    uint8_t channel;               /* controller's live channel (1..13; 0 =
                                    * unknown). Broadcasts bleed into adjacent
                                    * channels at close range, so the dial's
                                    * sweep can catch this RESP one channel
                                    * off — unicast then never ACKs. The dial
                                    * retunes here before its confirm probes.
                                    * SIGNED (in the transcript): a relayed
                                    * copy with a doctored channel fails
                                    * verification. */
};
```

Transcripts (domain-separated; all fields as sent; layouts wire-frozen —
`SL2_REQ_TRANSCRIPT_LEN` 80 B, `SL2_RESP_TRANSCRIPT_LEN` 114 B):

```
req_transcript  = "SLv2-req"  || type || version || src_mac || eph_pub || id_pub
resp_transcript = "SLv2-resp" || type || version || src_mac || eph_pub || id_pub
                  || dial_eph_pub   /* binds the response to THIS request */
                  || channel        /* so the retune hint can't be forged */
```

Key derivation:

```
shared = X25519(own_eph_priv, peer_eph_pub)
lmk    = HKDF-SHA256(shared, salt = dial_eph_pub || ctrl_eph_pub,
                     info = "serin-link-v2-lmk")[:16]
```

The HKDF is pinned in-tree (`sl2_sha256.h`) rather than delegated to a platform
crypto hook: two builds of the same crypto library once disagreed on streaming
HMAC-SHA256 and silently derived mismatched LMKs. A fixed implementation both
ends compile is the only portable guarantee.

Flow:

1. Dial: fresh ephemeral, broadcast `PAIR_REQ`, sweep channels 1–13, ~45 s window.
2. Controller (pairing window open, button-gated 60 s): verify `sig` against the
   packet's own `id_pub` (proof of possession). **Pinning check:** if a bond for
   this dial MAC exists with a different pinned `id_pub`, refuse and log. Fresh
   ephemeral, derive LMK, broadcast `PAIR_RESP`. Reply to every REQ heard.
3. Dial: verify `sig` (proof of possession) and the `dial_eph_pub` binding.
   **Pinning check:** same rule against its bond table. First valid RESP wins.
4. Dial derives LMK, installs the encrypted peer, retunes to the RESP's signed
   `channel` (falling back to a MAC-ACK channel hunt if probes go unACKed),
   sends PROBEs until the first STATE confirms the encrypted path, then
   persists the bond and reboots into the bonded link.

Threat model, stated honestly: first contact during the open pairing window is
TOFU — an attacker present in radio range at that moment can be pinned instead of
the real device (Zigbee permit-join posture; window is button-gated and short).
After first bond, pinning means re-pairs/replacements with a different key are
refused until the user explicitly forgets the zone. Replayed `PAIR_REQ`s are
harmless: the replayer lacks the ephemeral private key and can never derive the
LMK. Replay of *data-plane* ciphertexts (CMD/WIFI_SETUP captured and re-sent
after a controller reboot) is a real gap in the raw transport — closed by the
epoch echo, section 3b.

### 3a. Receiver hardening (normative for both ends)

Two rules the crypto alone does not give you:

- **dst-MAC gate.** ESP-NOW's encrypted-peer filter does not cover forged
  *plaintext broadcasts* carrying a bonded src MAC. Every packet except the
  legitimately-broadcast pairing pair (`PAIR_REQ` on the controller,
  `PAIR_RESP` on the dial) MUST be dropped unless `des_addr` equals the
  receiver's own STA MAC. Without this, a spoofed broadcast is accepted as
  STATE/CAPS/INFO (display spoofing) or WIFI_RESP (Wi-Fi credential injection
  during Link OTA).
- **Verify a snapshot, use the snapshot.** A `PAIR_RESP` landing buffer shared
  with the radio task MUST be single-writer (radio fills it only while empty)
  and the pairing task MUST copy it to a local before signature verification,
  then read only the copy. Re-reading the shared buffer after verification
  lets an attacker spamming forged RESPs swap unverified bytes into the
  LMK/pin/channel logic (TOCTOU).

### 3b. Data-plane replay guard (epoch echo)

ESP-NOW's CCMP packet-number window lives in the radio driver's RAM. After a
controller reboot (or peer re-add) it resets, so a **captured encrypted
ciphertext replayed later decrypts and delivers cleanly** — the LMK is
bond-lifetime. Without an application-layer freshness check, a saved CMD
("mode off", "heat to max") or WIFI_SETUP ("raise your setup hotspot") can be
replayed after any power blip. WIFI_SETUP is the sharp one: the setup AP uses
a documented password and its portal accepts credentials, so a replay is a
step toward re-provisioning the controller onto an attacker's network.

The guard:

- The controller draws a random nonzero u16 **epoch** each boot and carries it
  in every STATE. `0` is reserved for "no epoch support" (also the honest
  value if the RNG fails at boot — the guard turns off rather than locking
  dials out).
- The dial stores the latest epoch per zone and **echoes it in every CMD and
  WIFI_SETUP**. A dial must not send either packet before it holds a fresh
  STATE (the dial's sync window is already read-only, so this falls out).
- Enforcement **ratchets on per dial**: the first correct echo sets a
  persisted bond flag (`SL2_BOND_F_EPOCH`); from then on — across reboots —
  a zero or stale epoch from that dial is dropped. Until the ratchet is set,
  zero epochs are accepted so pre-epoch dial firmware keeps working (and the
  replay window honestly remains until the dial upgrades and sends its first
  echoed packet). Re-pairing resets the bond including the flag.
- On an epoch-mismatch drop the controller marks STATE pending for that dial,
  so a live dial that missed the reboot resyncs within the 250 ms floor and
  retries with the fresh value. A dropped packet does NOT count as liveness.

Scope, stated honestly: the epoch defends **cross-boot** replay. Same-boot
replay is left to the radio's PN window plus the commands' idempotent,
last-writer-wins semantics; PROBE/WIFI_REQ are unguarded because a replay of
either yields the attacker nothing (STATE/CAPS go to the dial's MAC; the
WIFI_RESP credential payload is encrypted to the dial).

### Multi-dial per controller

The controller keeps a **bond table**, not a single bond: up to
`SL2_MAX_DIALS 4` records `{dial_mac[6], lmk[16], dial_id_pub[32], flags}`
(flags bit 0 = the epoch-echo ratchet, section 3b). The pairing
window *adds* (or refreshes, same pinned key) without forgetting existing dials;
a full table refuses new pairs until one is forgotten. All bonded dials get the
STATE stream (unicast per peer, same change/heartbeat policy); CMD is accepted
from any bonded dial; the post-CMD STATE echo goes to **all** bonded dials so
every head converges within one send. Per-dial forget via the controller UI
(button/console/web). ESP-NOW's encrypted-peer budget (default 7) bounds
dials + the controller's other peers; 4 leaves headroom.

The dial side is unchanged — a dial still bonds up to 7 zones (controllers), and
a controller with several dials just sees several independent bonds. No wire
change; this is controller storage + fan-out policy.

Dial bond record (NVS, `fmt=3`):

```c
struct sl2_bond_rec {              /* 104 B; 7 zones ≤ 731 B blob */
    uint8_t  mac[6];
    uint8_t  lmk[16];
    uint8_t  peer_id_pub[32];      /* pinned Ed25519 identity */
    uint8_t  last_channel;
    char     name[32];             /* from CAPS */
    uint8_t  caps_seq;             /* last CAPS generation seen */
    uint8_t  reserved[16];
};
```

## 4. Semantic HVAC model

All vendor bytes stay off the wire. The controller translates to/from its
device protocol. The model is a deliberate superset of ESPHome `ClimateTraits`
+ HomeKit thermostat, so any adapter maps 1:1 or degrades by capability bit.

```c
enum sl2_mode {                    /* aligns with ESPHome ClimateMode / HomeKit */
    SL2_MODE_OFF = 0, SL2_MODE_HEAT = 1, SL2_MODE_COOL = 2,
    SL2_MODE_HEAT_COOL = 3,        /* dual setpoint: set_low/set_high */
    SL2_MODE_AUTO = 4,             /* single-setpoint vendor auto */
    SL2_MODE_DRY = 5, SL2_MODE_FAN_ONLY = 6,
    /* 7..15 reserved (modes mask is u16) */
};

enum sl2_action {                  /* what the equipment is DOING right now */
    SL2_ACT_UNKNOWN = 0, SL2_ACT_IDLE = 1, SL2_ACT_HEATING = 2,
    SL2_ACT_COOLING = 3, SL2_ACT_DRYING = 4, SL2_ACT_FAN = 5,
    SL2_ACT_DEFROST = 6, SL2_ACT_PREHEAT = 7, SL2_ACT_STANDBY = 8,
};

enum sl2_preset {                  /* ESPHome preset superset; 0 = none */
    SL2_PRESET_NONE = 0, SL2_PRESET_ECO = 1, SL2_PRESET_AWAY = 2,
    SL2_PRESET_BOOST = 3, SL2_PRESET_COMFORT = 4, SL2_PRESET_HOME = 5,
    SL2_PRESET_SLEEP = 6, SL2_PRESET_ACTIVITY = 7,
    /* 8..15 reserved (presets mask is u16) */
};
```

- **Power is not a field.** Off is a mode. The dial's power toggle sends
  `MODE_OFF` / restores the last non-off mode (dial remembers it locally).
- **Action, not an "operating" flag.** `is_operating`-style booleans are
  derived (`action >= HEATING`); vendor sub-states (defrost/preheat/standby)
  are first-class actions every vendor can map.
- **Fan:** `uint8_t`, `0 = auto`, `1–100 = percent`. The *controller*
  quantises percent to its own steps; `CAPS.fan_steps` tells the dial how many
  detents to render, and the dial sends the detent's canonical percent
  (`round(i * 100 / steps)`). Wire stays step-count-agnostic forever; a
  continuous-fan vendor needs no change.
- **Vanes:** per axis `uint8_t`: `0 = auto`, `1..n_pos` = physical positions
  (1 = most horizontal/left), `255 = swing`. Vendor specials (Mitsubishi
  wide-vane split etc.) are vendor TLVs, not core positions.
- **Temperatures:** deci-°C `int16_t` everywhere, always. °F is a *display*
  concern: `CAPS.ftab_id` selects the dial-side mapping table
  (`0` = linear round-trip, `1` = the Mitsubishi 61–88 °F table). New vendors
  add table ids; the dial ships the tables.
- **Humidity:** room + target reported in STATE, target controllable via
  CMD, gated by a `CAPS` capability bit. `0xFF` = invalid/unsupported anywhere.

## 5. STATE (26 B)

```c
struct sl2_state_pkt {
    uint8_t  type, version;
    uint8_t  caps_seq;      /* bumps when CAPS content changes -> dial re-pulls */
    uint8_t  flags;         /* SL2_SF_* */
    uint8_t  mode;          /* sl2_mode */
    uint8_t  action;        /* sl2_action */
    uint8_t  fan;           /* 0=auto, 1-100 */
    uint8_t  vane_v;        /* 0=auto, 1..n, 255=swing; 0 if axis absent */
    uint8_t  vane_h;
    uint8_t  preset;        /* sl2_preset; 0 = none */
    uint16_t fault;         /* 0 = normal; else vendor fault code (dial shows hex) */
    int16_t  room_dc;
    int16_t  set_dc;        /* single-setpoint modes */
    int16_t  set_low_dc;    /* HEAT_COOL band; 0x7FFF = n/a */
    int16_t  set_high_dc;   /* HEAT_COOL band; 0x7FFF = n/a */
    uint8_t  room_hum_pct;  /* 0-100; 0xFF = not reported */
    uint8_t  hum_set_pct;   /* 0-100; 0xFF = n/a */
    uint8_t  flags2;        /* SL2_SF2_* */
    uint8_t  reserved[1];
    uint16_t epoch;         /* per-boot replay token; 0 = no epoch support (3b) */
};
enum {  /* flags */
    SL2_SF_HVAC_LINK = 1u<<0,   /* controller <-> heat pump link up */
    SL2_SF_WIFI      = 1u<<1,
    SL2_SF_USE_F     = 1u<<2,   /* display pref, per controller */
    SL2_SF_WIFI_PROVISIONED = 1u<<3, /* has stored STA creds (dial Wi-Fi setup
                                      * face; always-on for YAML-provisioned fw) */
    SL2_SF_SETUP_AP  = 1u<<4,   /* recovery/setup hotspot currently active
                                 * (echoes a WIFI_SETUP request) */
    SL2_SF_WIFI_ERR  = 1u<<5,   /* last credential change failed to join (10b) */
    /* bits 6-7 spare */
};
enum {  /* flags2 */
    SL2_SF2_SENSOR_BATT_LOW = 1u<<0,
    /* bits 1-7 spare */
};
```

Send policy: on first-live, on change (≥250 ms min interval), heartbeat 10 s —
fanned out to every bonded dial. There is no HomeKit flag (HomeKit pairing
state is an INFO TLV) and no operating flag (subsumed by `action`); `fault` is
u16 so any vendor's fault-code space fits.

## 6. CMD (19 B)

```c
struct sl2_cmd_pkt {
    uint8_t  type, version;
    uint16_t mask;          /* SL2_CM_* */
    uint8_t  mode;
    uint8_t  fan;
    uint8_t  vane_v;
    uint8_t  vane_h;
    uint8_t  preset;
    int16_t  set_dc;
    int16_t  set_low_dc;
    int16_t  set_high_dc;
    uint8_t  hum_set_pct;
    uint8_t  use_f;         /* SL2_CM_UNITS: 0/1 display pref */
    uint16_t epoch;         /* echo of the latest STATE.epoch; 0 = legacy (3b) */
};
enum {
    SL2_CM_MODE     = 1u<<0,  SL2_CM_TEMP    = 1u<<1,
    SL2_CM_TEMP_BAND= 1u<<2,  SL2_CM_FAN     = 1u<<3,
    SL2_CM_VANEV    = 1u<<4,  SL2_CM_VANEH   = 1u<<5,
    SL2_CM_PRESET   = 1u<<6,  SL2_CM_HUM     = 1u<<7,
    SL2_CM_UNITS    = 1u<<8,
    /* bits 9-15 spare */
};
```

Apply-masked-fields-only; echo STATE to **all** bonded dials immediately after
applying. The dial debounces edits (250 ms) and holds its local value against
the echo for 3 s. Controllers MUST validate wire values before applying them
(range-clamp temperatures/humidity, ignore undeclared presets/modes — a CMD
field the controller didn't declare in CAPS is a no-op, and the dial shouldn't
offer it).

## 7. PROBE (4 B) and liveness

```c
struct sl2_probe_pkt {
    uint8_t type, version;
    uint8_t want;           /* SL2_WANT_* pull gates */
    uint8_t reserved[1];
};
enum {
    SL2_WANT_INFO  = 1u << 0,
    SL2_WANT_CAPS  = 1u << 1,
    SL2_WANT_STATE = 1u << 2,
    /* bits 3-7 spare */
};
```

Dial cadence: active zone 1 Hz, background zones every 4 s (staggered per
zone), cross-channel 30 s opportunistic. Liveness is MAC-ACK-based on the dial
(4 s active / 12 s background windows, 8 s no-STATE sync watchdog). The
controller serves `WANT_INFO`/`WANT_CAPS` pulls with a ~2 s per-type throttle
and considers a dial live for `SL2_DIAL_LIVE_MS` (12.5 s) after its last
probe — wide enough to survive one lost probe at the dial's slowest background
cadence, so a single dropped frame doesn't cut the STATE stream.

`SL2_WANT_STATE` is a dial-initiated STATE pull for zone switches and resyncs:
background keepalive probes keep the dial "live" on the controller, which
suppresses the first-contact STATE — without this bit, a dial that lacks a
fresh STATE could wait out the full 10 s heartbeat and misread the silence as
link loss. The controller serves the pull at the 250 ms min-interval floor and
keeps the bit latched until a STATE actually goes out, so loss-retry is simply
the dial's next probe. Unknown `want` bits are ignored: mixed protocol
versions degrade to heartbeat-paced behavior.

## 8. CAPS (68 B) — the capability descriptor

Pulled via `SL2_WANT_CAPS`; the dial requests it after pairing and whenever
`STATE.caps_seq` differs from the bond's stored `caps_seq`. Name changes, feature
changes, and firmware updates all just bump `caps_seq`.

```c
struct sl2_caps_pkt {
    uint8_t  type, version;
    uint8_t  caps_seq;
    uint8_t  caps_flags;     /* SL2_CF_* device-level capabilities */
    uint16_t modes;          /* bitmask: bit N = sl2_mode N supported (incl. OFF) */
    uint16_t presets;        /* bitmask: bit N = sl2_preset N (bit 0 unused) */
    uint8_t  fan_steps;      /* 0 = fan not controllable; N = discrete steps (excl. auto) */
    uint8_t  fan_flags;      /* bit0 = has auto */
    uint8_t  vane_v;         /* low nibble n_pos (0 = axis absent); bit4 auto; bit5 swing */
    uint8_t  vane_h;         /* same encoding */
    int16_t  set_min_dc;     /* setpoint range + step (also bounds the band) */
    int16_t  set_max_dc;
    uint8_t  set_step_dc;    /* deci-C: 5 = 0.5C, 10 = 1C */
    uint8_t  ftab_id;        /* 0 linear, 1 Mitsubishi 61-88F, ... */
    uint8_t  band_min_gap_dc;/* HEAT_COOL: min high-low separation, deci-C (0 = none) */
    uint8_t  hum_step_pct;   /* target-humidity step; 0 = n/a */
    uint16_t features;       /* SL2_FEAT_*: which INFO TLVs this controller emits */
    char     name[32];       /* zone name; empty = unnamed */
    uint8_t  reserved[14];   /* pads the packed struct to the stated 68 B */
};
enum {  /* caps_flags — control capabilities */
    SL2_CF_HUM_CTRL = 1u<<0,     /* accepts SL2_CM_HUM */
    /* bits 1-7 spare */
};
enum {  /* features — telemetry the controller can emit (INFO TLVs / creds) */
    SL2_FEAT_WIFI_INFO   = 1u<<0,  SL2_FEAT_HOMEKIT   = 1u<<1,
    SL2_FEAT_OUTSIDE_T   = 1u<<2,  SL2_FEAT_COMPRESSOR = 1u<<3,
    SL2_FEAT_SENSOR_BATT = 1u<<4,  SL2_FEAT_FW_INFO    = 1u<<5,
    SL2_FEAT_RUNTIME     = 1u<<6,  SL2_FEAT_LINK_OTA_CREDS = 1u<<7,
    SL2_FEAT_ENERGY      = 1u<<8,  SL2_FEAT_WIFI_SETUP = 1u<<9, /* accepts WIFI_SETUP */
    /* bits 10-15 spare */
};
```

The dial builds its UI from this: mode ring shows only set bits; fan page detents
per `fan_steps`; vane pages exist per axis; setpoint clamps/steps per range;
HEAT_COOL renders per the mode bit (enforcing `band_min_gap_dc`); humidity
control appears iff `SL2_CF_HUM_CTRL`; settings pages appear per `features`
bit. Nothing is vendor-assumed.

**Current Serin dial firmware limitations** (a capability the dial doesn't
render yet degrades gracefully; the wire carries it regardless):

- No dedicated HEAT_COOL band editor: the mode maps onto the dial's
  single-setpoint Auto, a band-mode STATE (`set_dc = 0x7FFF`) displays the
  band midpoint, and commits send `SL2_MODE_AUTO`.
- No humidity-control UI (`SL2_CF_HUM_CTRL` is ignored; humidity is
  display-only).

**Known modeling gap — asymmetric heat/cool ranges.** `set_min_dc`/
`set_max_dc` is one range for every mode. Real heat pumps sometimes have
distinct heating vs. cooling limits (e.g. heat 10–28 °C, cool 18–32 °C); a
dial rendering the union lets a user request a value the controller then
clamps, and the STATE echo snaps the dial back — legal (controllers MUST
clamp, section 6) but a mediocre UX. When an adapter for such a unit lands,
claim four `reserved[]` bytes for `heat_min_dc`/`heat_max_dc`/`cool_min_dc`/
`cool_max_dc` (0 = "use the shared range") — additive, no wire break. Not
done pre-emptively: both current substrates (ESPHome climate traits, HomeKit)
model a single visual range themselves.

## 9. INFO — TLV telemetry (pull-only)

One packet, a stream of TLVs, truncated to what fits 250 B; the controller
sends what it has, gated by `SL2_WANT_INFO`.

```c
struct sl2_info_pkt {
    uint8_t type, version;
    uint8_t reserved[2];
    /* then TLVs back-to-back until packet end: */
    /* uint8_t t; uint8_t l; uint8_t v[l]; */
};
```

Core TLV registry (0x01–0x7F core; 0x80–0xFF vendor-specific, dial ignores
unknown types in both ranges):

| t | Name | Payload |
|---|------|---------|
| 0x01 | `WIFI_INFO` | `int8 rssi; u8 channel; char ssid[]; char ip[]` (NUL-joined) |
| 0x02 | `HOMEKIT` | `u8 paired_count; char code[]; char payload[]` (NUL-joined) |
| 0x03 | `OUTSIDE_T` | `int16 outside_dc` |
| 0x04 | `COMPRESSOR` | `u8 hz; u8 stage; u8 vendor_sub_mode; u8 vendor_auto_sub` |
| 0x05 | `SENSOR_BATT` | `u8 pct` |
| 0x06 | `FW_INFO` | `char version[]; char build_date[]` (NUL-joined) |
| 0x07 | `RUNTIME` | `u32 hours` |
| 0x08 | `SYS` | `u32 uptime_s; u8 reset_reason` |
| 0x09 | `ENERGY` | `u16 input_w; u32 wh_total` (0xFFFF/0xFFFFFFFF = n/a) |

Rules: dial renders "—" for any TLV absent; `l` is authoritative (forward-compat:
longer payloads than the dial knows are prefix-read); a TLV never changes
meaning, new fields append, new facts get new types. `COMPRESSOR.stage/
vendor_sub_mode` carry Mitsubishi value tables — generic running-state already
lives in `STATE.action`, so other vendors simply omit this TLV or ship a
vendor TLV with their own semantics.

## 10. Link OTA credential relay

```c
struct sl2_wifi_req_pkt  { uint8_t type, version; uint8_t reserved[2]; };
struct sl2_wifi_resp_pkt {
    uint8_t type, version;
    uint8_t ok;             /* 0 = no creds stored */
    char    ssid[33];
    char    psk[65];
    uint8_t reserved[2];
};
```

Encrypted unicast only; dial keeps creds in RAM for one OTA attempt and zeroizes.
Controllers advertise availability via `SL2_FEAT_LINK_OTA_CREDS` (an ESPHome
adapter reads its own STA creds; a build may decline with the bit unset — the
dial then hides/greys its update path).

## 10b. Dial-initiated Wi-Fi setup (change network)

```c
struct sl2_wifi_setup_pkt { uint8_t type, version; uint8_t reserved[2]; };
```

Encrypted unicast, dial→ctrl: "raise your recovery/setup hotspot NOW" — the
dial's change-network flow needs the hotspot up while the controller's STA is
still healthily connected (a recovery flow would only raise it after a
disconnect timeout). No response packet: the controller reports the hotspot
via `SL2_SF_SETUP_AP` in STATE, and a flag flip re-sends STATE within the
250 ms floor. The dial re-fires ~1 Hz until it sees the flag (ESP-NOW is
lossy), so the request must be idempotent; the controller should also bound
the window (e.g. 10 min auto-close if the STA never drops — an abandoned
change attempt can't leave the AP up forever). Gated on `SL2_FEAT_WIFI_SETUP`
in CAPS: the dial hides the change-network affordance entirely when the bit
is absent. Handshake:

```
dial: WIFI_SETUP  ──►  ctrl raises AP (idempotent) ──► STATE: SF_SETUP_AP
dial shows join QR ──► phone joins AP, captive portal collects new creds
ctrl STA drops (STATE: !SF_WIFI) ──► reconnects on the new network
ctrl closes the AP after reconnect ──► STATE: !SF_SETUP_AP
dial: "Connected" finale
```

Completion detection (dial-side): the `!SF_WIFI` edge is a single
change-triggered STATE transmitted exactly while the controller's radio
re-associates (and a channel move deafens the dial until it re-locks), so
receivers must NOT rely on observing it. The lossless signal is `SF_SETUP_AP`
returning to 0 while `SF_WIFI` is set — level-encoded in every subsequent
STATE/heartbeat. The dial advances on either (outage observed via the flag or
its own link-loss detection, OR hotspot-seen-up → now-closed while connected).

**Normative for the completion signal:** because AP-closed-while-connected IS
the dial's success tell, a controller MUST NOT close the setup AP while the
STA is connected to anything other than the credentials applied *during this
window*. Concretely: a reconnect to the old network during the window (portal
scan / beacon-loss blip) must neither close the AP nor end the window, and a
failed change (wrong password) must leave the AP up for retry rather than
"fall back and close" — an adapter whose Wi-Fi stack auto-falls-back to prior
credentials and drops its AP would otherwise show the user a false
"Connected". (The reference controller keys this off "connected AND no join
pending", i.e. got-IP on the last-applied credentials.)

**Failure signal:** `SL2_SF_WIFI_ERR` (STATE flags bit 5). A controller
SHOULD latch it when a portal-applied credential change fails to join (retry
budget exhausted / window expired without got-IP on the new credentials), and
clear it on the next successful join or the next change window. It lets the
dial render "Connection failed — check the password" instead of an ambiguous
timeout. Dials must tolerate controllers that never set it (bit was spare
pre-epoch fw): the timeout path stays.

## 10c. DIAL_INFO (dial→ctrl, encrypted, push)

```c
struct sl2_dial_info_pkt {
    uint8_t type;            /* SL2_PKT_DIAL_INFO */
    uint8_t version;         /* SL2_PROTO_VERSION */
    uint8_t caps_seq;        /* the caps_seq the dial currently has applied */
    char    model[24];       /* branded model, NUL-terminated */
    char    fw[16];          /* firmware version, NUL-terminated */
};
#define SL2_DIAL_INFO_MIN_LEN 3   /* through caps_seq; model/fw may be absent */
```

The dial reports its own identity so the controller's UI can show which head
is paired (model + firmware version per bond-table entry). Sent on connect, on
caps_seq change, and ~every 60 s. `caps_seq` is the generation the dial has
*applied* — a controller can compare it against its own to render a "syncing"
hint. Controllers expose the latest values via `sl2_link_dial_view()`;
receiving any DIAL_INFO also refreshes the dial's liveness window.

## 11. Future-proofing inventory (what's deliberately left room for)

- **New modes/presets/actions:** u16 masks + reserved enum space; additive.
- **New fan semantics:** percent wire is step-agnostic; `fan_flags` has 7 spare
  bits (e.g. a future "quiet" modifier as a flag, not a step).
- **New telemetry:** TLV registry, vendor range, `features` bits 10–15 spare.
- **New control surfaces** (aux heat, purifier/night-mode switches): next
  `caps_flags` bits + CMD mask bits 9–15 + claimed `reserved[]` bytes; if they
  outgrow that, a new packet type (12+) without touching existing layouts.
- **Second temperature sensor / follow-me:** dial-side room sensing would ride a
  new dial→ctrl packet type; nothing in STATE precludes it.
- **Other carriers:** §1 note — packet layer is carrier-neutral; port owns
  discovery.
- **Bigger vane arrays / 4-way cassettes:** nibble caps positions at 15; beyond
  that, a `caps_flags` bit + appended fields. Accepted limit.
- **Explicitly out of scope forever (wire):** schedules/programs (the dial is a
  head, not a scheduler — that lives in HA/HomeKit), and °F on the wire.

## 12. libserinlink port surface

The controller-role core is dependency-free C (Apache-2.0). Adopters provide a
platform port and crypto hooks, and implement the HVAC interface:

```c
typedef struct sl2_port {
    void *ctx;
    /* transport (≤250 B MTU, 6-byte peer addresses) */
    bool (*send)(void *ctx, const uint8_t mac[6], const void *buf, size_t len);
    bool (*peer_add)(void *ctx, const uint8_t mac[6],
                     const uint8_t lmk[16], bool encrypt);
    void (*peer_del)(void *ctx, const uint8_t mac[6]);
    bool (*own_mac)(void *ctx, uint8_t out[6]);
    uint8_t (*get_channel)(void *ctx);       /* optional: NULL ok */
    /* time + storage */
    uint32_t (*now_ms)(void *ctx);           /* monotonic; wraps ok */
    bool (*kv_get)(void *ctx, const char *key, void *buf, size_t *len);
    bool (*kv_set)(void *ctx, const char *key, const void *buf, size_t len);
    void (*log)(void *ctx, int level, const char *msg);  /* optional */
} sl2_port_t;

typedef struct sl2_crypto {   /* bind libsodium or equivalent; see sl2_crypto.h */
    void *ctx;
    int (*rand_bytes)(void *ctx, uint8_t *buf, size_t len);
    int (*x25519_keypair)(void *ctx, uint8_t priv[32], uint8_t pub[32]);
    int (*x25519_shared)(void *ctx, const uint8_t priv[32],
                         const uint8_t peer_pub[32], uint8_t shared[32]);
    int (*ed25519_keypair)(void *ctx, uint8_t priv[64], uint8_t pub[32]);
    int (*ed25519_sign)(void *ctx, const uint8_t priv[64],
                        const uint8_t *msg, size_t msg_len, uint8_t sig[64]);
    int (*ed25519_verify)(void *ctx, const uint8_t pub[32],
                          const uint8_t *msg, size_t msg_len,
                          const uint8_t sig[64]);
} sl2_crypto_t;

typedef struct sl2_hvac_iface {
    void *ctx;
    bool (*get_state)(void *ctx, sl2_hvac_state_t *out);   /* semantic model */
    bool (*apply)(void *ctx, uint16_t mask, const struct sl2_cmd_pkt *cmd);
    bool (*get_caps)(void *ctx, struct sl2_caps_pkt *out);
    size_t (*fill_info_tlvs)(void *ctx, uint8_t *buf, size_t cap);
    bool (*wifi_creds)(void *ctx, char ssid[33], char psk[65]);  /* NULL = FEAT unset */
    bool (*wifi_setup)(void *ctx);                               /* NULL = FEAT unset */
} sl2_hvac_iface_t;
```

Driving it:

```c
sl2_link_init(&link, &port, &crypto, &hvac);
sl2_link_start(&link);          /* after Wi-Fi/radio is up */
/* every loop: drain your radio's rx callback into
 * sl2_link_on_recv(&link, src_mac, dst_mac, data, len), then: */
sl2_link_loop(&link);
```

The core owns: the pairing state machine, signature/pin checks, LMK derivation,
the multi-dial bond table (through `kv_*`), probe/STATE/CAPS/INFO scheduling +
fan-out, and tolerant decode. **Threading contract:** `sl2_link_on_recv()` and
`sl2_link_loop()` must run in the same context — queue radio callbacks through
the SPSC ring in `sl2_rxq.h` if they don't. Note there is deliberately no HKDF
crypto hook (see the key-derivation note in §3).

Reference adapters: the ESPHome `serin_link` component in this repo (binds any
`climate` entity — `ClimateTraits` → `sl2_caps_pkt` is nearly 1:1), and the
Serin Controller firmware (native CN105/HomeKit).
