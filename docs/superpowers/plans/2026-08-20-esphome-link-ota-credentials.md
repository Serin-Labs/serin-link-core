# ESPHome Link-OTA Credential Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a Serin Link paired with an ESPHome node update its own firmware, by teaching the `serin_link` component to answer the Link's `WIFI_REQ` with the node's STA credentials — opt-in, default off.

**Architecture:** Three small changes to one ESPHome component. A `link_ota_credentials:` boolean gates (a) an `SL2_FEAT_LINK_OTA_CREDS` bit in CAPS, which is the only thing the Link's Update pill is gated on, and (b) a `wifi_creds` hook that reads the live STA config out of the Wi-Fi driver. A config-time guard rejects the combination with WPA-Enterprise, which has no PSK to relay. No protocol change, no dial firmware change: bonded Links pick the bit up through the existing CAPS-fingerprint `caps_seq` bump.

**Tech Stack:** ESPHome external component (Python config schema + C++ on ESP-IDF), the vendored `libserinlink` C core, `esp_wifi` IDF API, bash+`esphome config` schema tests.

**Spec:** `docs/superpowers/specs/2026-08-20-esphome-link-ota-credentials-design.md`

## Global Constraints

- **Repo:** all work is in `/home/akifb/serin-link-core`. `serin-link` (the dial firmware) must not be modified — "no dial change" is a design guarantee, not an oversight.
- **Never `git push`.** Commit locally only. Pushing requires explicit approval per the user's standing instruction.
- **YAML key name:** `link_ota_credentials` — exactly this spelling, everywhere.
- **Default:** `False`. Credential handover is never on unless the config says so.
- **Feature bit:** `SL2_FEAT_LINK_OTA_CREDS` (already defined in `sl2_proto.h` as `1u << 7` — do not redefine it).
- **Product naming in user-facing text** (README, error messages, entity names): the dial is a **"Serin Link"**, the heat-pump controller is a **"Serin Controller"**. Code identifiers keep their existing names (`dial`, `link`, etc.).
- **Wire spec is NOT edited.** `docs/serin-link-wire-spec.md` §10 already describes this exact behavior ("an ESPHome adapter reads its own STA creds; a build may decline with the bit unset").
- **Test command:** `ESPHOME=~/.local/opt/esphome-venv/bin/esphome test/esphome_schema.sh` — there is no `esphome` on `PATH`.
- **C core tests:** `test/run.sh` (must keep passing; this plan does not change C core files).

---

### Task 1: The `link_ota_credentials:` config key and the EAP guard

Config surface plus its validation, with both schema tests. This is a self-contained deliverable: after it, `esphome config` accepts the key and rejects the dangerous combination, even though the key does nothing at runtime yet. It is worth its own review gate because the EAP guard is the part that can be wrong in a way a compile never catches.

**Files:**
- Modify: `esphome/components/serin_link/__init__.py` (constant near line 115; schema entry near line 520; `FINAL_VALIDATE_SCHEMA` near line 690; `to_code` near line 824)
- Test: `test/esphome/pass_link_ota_creds.yaml` (create)
- Test: `test/esphome/fail_link_ota_creds_eap.yaml` (create)
- Test: `test/esphome_schema.sh` (modify: two dict entries)

**Interfaces:**
- Consumes: nothing.
- Produces: `CONF_LINK_OTA_CREDENTIALS = "link_ota_credentials"` (Python constant), and a `to_code` call to `var.set_link_ota_credentials(True)` — Task 2 defines that C++ setter. Until Task 2 lands, the generated C++ will not compile; that is expected and is why the tests in this task are `esphome config` (validation only), never `esphome compile`.

- [ ] **Step 1: Write the failing tests**

Create `test/esphome/pass_link_ota_creds.yaml`:

```yaml
# link_ota_credentials: — the node hands its STA creds to a bonded Serin Link
# so the Link's own Update flow can join the network and fetch its firmware.
# Opt-in: without this key the CAPS feature bit stays clear and the Link hides
# its update path entirely.
esphome:
  name: t-linkota

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

wifi:
  ssid: "test"
  password: "testpass123"

logger:

external_components:
  - source:
      type: local
      path: ../../esphome/components
    components: [serin_link]

serin_link:
  id: serin
  link_ota_credentials: true
```

Create `test/esphome/fail_link_ota_creds_eap.yaml`:

```yaml
# WPA-Enterprise has no PSK to relay, so a Serin Link handed these
# "credentials" could never join. Reject at config time rather than let the
# Link grow an Update button that dies at "WiFi failed".
esphome:
  name: t-linkotaeap

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

wifi:
  networks:
    - ssid: "corp"
      eap:
        username: "user"
        password: "testpass123"

logger:

external_components:
  - source:
      type: local
      path: ../../esphome/components
    components: [serin_link]

serin_link:
  id: serin
  link_ota_credentials: true
```

In `test/esphome_schema.sh`, add to the `MUST_CONTAIN` dict (after the `pass_night.yaml` line):

```bash
  [pass_link_ota_creds.yaml]='link_ota_credentials: true'   # key survives validation
```

And to the `MUST_REJECT_WITH` dict (after the `fail_screen_row_without_switch.yaml` entry):

```bash
  # not 'link_ota_credentials': the dumped config echo contains that literal
  [fail_link_ota_creds_eap.yaml]='no PSK to relay'
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd /home/akifb/serin-link-core && ESPHOME=~/.local/opt/esphome-venv/bin/esphome test/esphome_schema.sh`

Expected: FAIL on both new files. `pass_link_ota_creds.yaml` fails to validate (ESPHome reports `[link_ota_credentials] is an invalid option for [serin_link]`). `fail_link_ota_creds_eap.yaml` also fails to validate — but the harness prints `FAIL ... rejected, but not for the expected reason ('no PSK to relay')`, because right now it is rejected for the unknown key, not for the guard. Both messages are the point: a rejection for the wrong reason is a failing test here.

- [ ] **Step 3: Add the constant and the schema entry**

In `esphome/components/serin_link/__init__.py`, next to `CONF_NIGHT = "night"` (line ~115):

```python
CONF_LINK_OTA_CREDENTIALS = "link_ota_credentials"
```

In `_BASE_SCHEMA`, immediately after the `cv.Optional(CONF_NIGHT): _night_schema,` line:

```python
        # link_ota_credentials: — answer a bonded Serin Link's WIFI_REQ with
        # this node's STA credentials, so the Link's own update flow can join
        # the network and pull its firmware. Off by default: it hands the
        # Wi-Fi PSK to the Link (encrypted unicast, RAM-only both ends,
        # zeroized after use), which is a decision each config makes for
        # itself. Unset = CAPS omits SL2_FEAT_LINK_OTA_CREDS and the Link
        # hides its update path.
        cv.Optional(CONF_LINK_OTA_CREDENTIALS, default=False): cv.boolean,
```

- [ ] **Step 4: Add the EAP guard**

Still in `__init__.py`, replace the existing `_no_builtin_espnow` / `FINAL_VALIDATE_SCHEMA` block (around line 679-691) with:

```python
def _no_builtin_espnow(config):
    full = fv.full_config.get()
    if "espnow" in full:
        raise cv.Invalid(
            "serin_link owns the ESP-NOW radio (encrypted peers, recv callback); "
            "remove the `espnow:` component — it cannot coexist and does not "
            "support ESP-NOW link-layer encryption."
        )
    return config


def _ota_creds_need_a_psk(config):
    """WPA-Enterprise has no PSK, so there is nothing a Serin Link could join
    with. Reject the pair at config time instead of shipping an Update button
    that always dies at "WiFi failed". Strict on ANY eap: network, not "every
    network is eap": ESPHome folds a bare `ssid:` into `networks:`, and the
    node may associate to the EAP one at runtime — at which point the relay
    would hand out credentials that cannot work."""
    if not config.get(CONF_LINK_OTA_CREDENTIALS):
        return config
    wifi_conf = fv.full_config.get().get("wifi") or {}
    if any("eap" in net for net in (wifi_conf.get("networks") or [])):
        raise cv.Invalid(
            "`link_ota_credentials:` relays an SSID and PSK to the Serin Link, "
            "and a WPA-Enterprise (`eap:`) network has no PSK to relay — the "
            "Link could never join it. Remove `link_ota_credentials:`, or give "
            "the node a WPA2-PSK network."
        )
    return config


def _final_validate(config):
    _no_builtin_espnow(config)
    _ota_creds_need_a_psk(config)
    return config


FINAL_VALIDATE_SCHEMA = _final_validate
```

- [ ] **Step 5: Emit the setter in `to_code`**

In `to_code`, immediately after the `if CONF_NIGHT in config:` block (line ~822-824):

```python
    if config[CONF_LINK_OTA_CREDENTIALS]:
        cg.add(var.set_link_ota_credentials(True))
```

Direct subscript, not `.get()`: the key has a schema default, so it is always present.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cd /home/akifb/serin-link-core && ESPHOME=~/.local/opt/esphome-venv/bin/esphome test/esphome_schema.sh`

Expected: PASS for every file, including `PASS pass_link_ota_creds.yaml` and `PASS fail_link_ota_creds_eap.yaml (rejected as expected)`. Exit code 0.

The `MUST_CONTAIN` marker's casing was checked against this ESPHome version (2026.7.4): booleans dump lowercase, `link_ota_credentials: true`. Note that the schema default now puts `link_ota_credentials: false` in *every* config dump — harmless, since the marker asserts `true`.

- [ ] **Step 7: Commit**

```bash
cd /home/akifb/serin-link-core
git add esphome/components/serin_link/__init__.py test/esphome/pass_link_ota_creds.yaml test/esphome/fail_link_ota_creds_eap.yaml test/esphome_schema.sh
git commit -m "feat(esphome): link_ota_credentials: config key + WPA-Enterprise guard"
```

---

### Task 2: The runtime credential relay

The C++ half: the setter Task 1 already calls, the CAPS feature bit, and the `wifi_creds` hook. After this task the feature actually works.

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h` (setter near line 190; member near line 242)
- Modify: `esphome/components/serin_link/serin_link.cpp` (CAPS bits near line 612-622; trampoline near line 1009; hook install at line 1198)

**Interfaces:**
- Consumes: `var.set_link_ota_credentials(True)` from Task 1's `to_code`.
- Produces: `void SerinLinkComponent::set_link_ota_credentials(bool v)`, `bool SerinLinkComponent::hvac_wifi_creds(char ssid[33], char psk[65])`, and the file-static trampoline `static bool t_wifi_creds(void *ctx, char ssid[33], char psk[65])`. Nothing later in this plan consumes them.

- [ ] **Step 1: Add the setter and the member**

In `serin_link.h`, directly after `void set_night_switch(switch_::Switch *s) { night_switch_ = s; }` (line ~190):

```cpp
  /* Answer a bonded Serin Link's WIFI_REQ with this node's STA credentials
   * (`link_ota_credentials:`), so the Link's own update flow can join the
   * network. Off by default — see hvac_wifi_creds. */
  void set_link_ota_credentials(bool v) { link_ota_credentials_ = v; }
```

In the same header, add the declaration next to the other HVAC-iface backing methods (after `bool hvac_get_caps(struct sl2_caps_pkt *out);`, line ~193):

```cpp
  bool hvac_wifi_creds(char ssid[33], char psk[65]);
```

And in the `protected:` members, next to `bool link_sensor_cfg_{false};` (line ~242):

```cpp
  bool link_ota_credentials_{false};
```

- [ ] **Step 2: Declare the CAPS feature bit**

In `serin_link.cpp`, inside `SerinLinkComponent::hvac_get_caps`, after the `SL2_FEAT_ENERGY` block (line ~621-622):

```cpp
  if (link_ota_credentials_) out->features |= SL2_FEAT_LINK_OTA_CREDS;
```

This is what the Serin Link gates its Update entry on, and `setup()`'s CAPS fingerprint hashes this packet — so adding the key to a config and reflashing bumps `caps_seq` on its own and every bonded Link re-pulls. No re-pair, no dial reflash.

- [ ] **Step 3: Implement the hook**

In `serin_link.cpp`, add the member function immediately after `SerinLinkComponent::hvac_get_caps` ends (before the next function definition):

```cpp
/* WIFI_REQ -> WIFI_RESP: hand a bonded Serin Link this node's STA
 * credentials for its firmware update (`link_ota_credentials:`).
 *
 * Read from the DRIVER, not from ESPHome's `wifi:` model: this is whatever
 * network the node actually joined, which is the one the Link needs — correct
 * under multi-network roaming and for credentials saved through the captive
 * portal, neither of which appears in YAML. The core zeroizes the response
 * packet after sending it; the Link holds the creds in RAM for one attempt
 * and scrubs them after the join. */
bool SerinLinkComponent::hvac_wifi_creds(char ssid[33], char psk[65]) {
  wifi_config_t wc;
  if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
  if (wc.sta.ssid[0] == 0) return false;   /* nothing stored yet */
  /* The driver's fields are fixed-width and NOT guaranteed NUL-terminated;
   * copy the full width and terminate past it. */
  memcpy(ssid, wc.sta.ssid, sizeof(wc.sta.ssid));
  ssid[sizeof(wc.sta.ssid)] = '\0';
  memcpy(psk, wc.sta.password, sizeof(wc.sta.password));
  psk[sizeof(wc.sta.password)] = '\0';
  memset(&wc, 0, sizeof wc);               /* no PSK copy left on our stack */
  return true;
}
```

`wc.sta.ssid` is 32 bytes and `wc.sta.password` 64, matching the `char[33]` / `char[65]` the hook is handed — the terminator lands exactly at index 32 / 64. An open network yields an empty password, which is correct and relayed as-is.

- [ ] **Step 4: Add the trampoline and install the hook**

In `serin_link.cpp`, after `t_room_sensor` (line ~1009-1012), add:

```cpp
static bool t_wifi_creds(void *ctx, char ssid[33], char psk[65]) {
  return static_cast<SerinLinkComponent *>(ctx)->hvac_wifi_creds(ssid, psk);
}
```

Then replace line 1198 —

```cpp
  hvac_.wifi_creds = nullptr;  /* Link-OTA creds relay: future work */
```

— with:

```cpp
  /* Installed only when the config opted in: a null hook makes the core
   * answer ok=0, which is the right degraded behavior if the hook and the
   * CAPS bit ever disagree. */
  hvac_.wifi_creds = link_ota_credentials_ ? t_wifi_creds : nullptr;
```

`setup()` runs after `to_code`'s setter calls, so `link_ota_credentials_` is already correct at this point.

- [ ] **Step 5: Verify it compiles**

Run:

```bash
cd /home/akifb/serin-link-core/test/esphome
~/.local/opt/esphome-venv/bin/esphome compile pass_link_ota_creds.yaml
```

Expected: a successful build ending in `Successfully compiled program`. This is the first real compile of the new C++ — Task 1's schema tests only validated config.

Do not pipe this through `| tail`: it masks the exit code, so a failed build reads as a pass.

If the ESP-IDF toolchain is not installed in this environment and the compile cannot run, say so explicitly and stop — do not report the task as verified. The bench step in Task 4 is not a substitute for this compile.

- [ ] **Step 6: Re-run the schema tests**

Run: `cd /home/akifb/serin-link-core && ESPHOME=~/.local/opt/esphome-venv/bin/esphome test/esphome_schema.sh`

Expected: every file PASS, exit 0. (The compile in Step 5 leaves a `.esphome/` build dir under `test/esphome/`; confirm it is gitignored with `git status --short` before committing, and do not `git add` it.)

- [ ] **Step 7: Confirm the C core still passes**

Run: `cd /home/akifb/serin-link-core && test/run.sh`

Expected: all tests pass. This task changes no C core file, so a failure here means something else is wrong — investigate before committing.

- [ ] **Step 8: Commit**

```bash
cd /home/akifb/serin-link-core
git add esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "feat(esphome): relay STA credentials for Serin Link firmware updates"
```

---

### Task 3: Documentation

The feature is invisible without it, and it hands out a Wi-Fi password — the caveats are part of the deliverable, not a follow-up.

**Files:**
- Modify: `README.md` (new section under "ESPHome component details"; one bullet in "Trust model" near line 372)
- Modify: `esphome/example_package.yaml` (commented-out key)

**Interfaces:**
- Consumes: the `link_ota_credentials:` key from Task 1.
- Produces: nothing consumed by later tasks.

- [ ] **Step 1: Write the README section**

In `README.md`, add a new `###` section under the "ESPHome component details" area, following the style of the existing `### Screen on/off from Home Assistant` section:

````markdown
### Letting the Serin Link update itself

A Serin Link installs its own firmware: Settings → About → Details → Update.
It has no Wi-Fi credentials of its own, so it asks the controller it is
bonded to. Opt in and the Link grows that update path:

```yaml
serin_link:
  id: serin
  climate_id: hvac
  link_ota_credentials: true   # default false
```

The node answers with the network it is actually joined to. The Link pauses
its ESP-NOW link, joins, fetches Serin's signed firmware manifest over HTTPS,
asks the user to confirm the version, then downloads, decrypts, and
hash-verifies the image before switching boot partitions — the node supplies
a network and nothing else, and never touches the firmware or its trust
chain.

Bonded Links pick this up on their own: the capability fingerprint bumps
`caps_seq` when you reflash the node, every Link re-pulls, and the update
path appears. No re-pairing, and no firmware update needed on the Link to
enable Link firmware updates.

What you are agreeing to, plainly:

- **The Wi-Fi PSK goes to the Link.** Only to a Link that completed signed
  pairing and is in the bond table, only over LMK-encrypted unicast, only
  when it asks. It is held in RAM at both ends and zeroized after the join —
  never written to flash on the Link. This is the same posture as the Serin
  Controller firmware, made explicit here because on an ESPHome node it is
  your network and your decision.
- **WPA-Enterprise is rejected at config time.** An `eap:` network has no PSK
  to relay, so the pair of keys is refused rather than shipping an update
  button that always fails.
- **The Link needs internet.** It resolves and fetches over HTTPS and needs
  NTP for certificate validation; on an IoT VLAN with no route out the check
  fails at "Update check failed".
- **The Link drops off the link while it updates.** Its `connected` and
  per-slot diagnostics rows go down for the download and reboot, then
  recover — bonds are persistent, so nothing needs re-pairing.
````

- [ ] **Step 2: Add the trust-model bullet**

In `README.md`'s "Trust model" list (line ~372-388), add as the last bullet:

```markdown
- `link_ota_credentials:` (default **off**) is the one setting that sends a
  secret *outward*: it relays the node's Wi-Fi SSID and PSK to a bonded Serin
  Link, encrypted, on request, so the Link can fetch its own firmware. Off
  unless a config asks for it.
```

- [ ] **Step 3: Add the commented example**

In `esphome/example_package.yaml`, inside the `serin_link:` block, add:

```yaml
  # Let a bonded Serin Link fetch its own firmware — relays this node's Wi-Fi
  # credentials to it on request. See README, "Letting the Serin Link update
  # itself", for what that hands to whom.
  # link_ota_credentials: true
```

Leave `esphome/packages/cn105.yaml` alone: credential handover stays an explicit per-config decision, never something a package turns on for you.

- [ ] **Step 4: Verify the examples still validate**

Run:

```bash
cd /home/akifb/serin-link-core/test/esphome
for f in ../../esphome/example_*.yaml; do
  echo "== $f"; ~/.local/opt/esphome-venv/bin/esphome config "$f" >/dev/null || echo "BROKE $f"
done
```

Expected: no `BROKE` lines. (A commented-out key cannot break validation; this catches an indentation slip in the edit.)

If an example fails for a pre-existing reason unrelated to this change — e.g. it references a secret or a source pin that does not resolve locally — say so explicitly rather than treating it as a pass, and confirm it failed the same way before the edit with `git stash`.

- [ ] **Step 5: Commit**

```bash
cd /home/akifb/serin-link-core
git add README.md esphome/example_package.yaml
git commit -m "docs(esphome): link_ota_credentials — what it does and what it hands out"
```

---

### Task 4: Bench validation on real hardware

The schema tests prove the config surface and the compile proves the C++ builds; neither proves a Serin Link grows an Update pill. This task is the only evidence that the feature works, and it is a task rather than a footnote because it can fail on its own.

**Files:** none — this is verification. Any code fix it provokes belongs in the task that owns that code, as a follow-up commit.

**Interfaces:**
- Consumes: everything from Tasks 1-3.
- Produces: a pass/fail report. Nothing else depends on it.

**Hardware:** node = the user's `~/heatpump-master.yaml` (already running serin-link-core). Link = bench dial #3, MAC `10:51:db:8e:ea:b0`, `build_viewe15`, the one feeding HA.

> **This task needs the user.** It flashes a live node and drives a physical dial. Ask before starting it, and do not flash anything without a clear yes.

- [ ] **Step 1: Point the node's config at this working tree and enable the key**

In `~/heatpump-master.yaml`, temporarily switch the `external_components:` source from the released `github://Serin-Labs/serin-link-core@v0.1.3-beta.11` pin to the local path, and add the key:

```yaml
external_components:
  - source:
      type: local
      path: /home/akifb/serin-link-core/esphome/components
    components: [serin_link]

serin_link:
  # ... existing keys unchanged ...
  link_ota_credentials: true
```

Record what the `source:` block said before the edit, so Step 6 can put it back verbatim.

- [ ] **Step 2: Flash the node**

Run: `cd ~ && ~/.local/opt/esphome-venv/bin/esphome run heatpump-master.yaml`

Expected: compiles, uploads over the network, and the node comes back in HA. Do not pipe through `| tail`.

- [ ] **Step 3: Confirm the Update pill appears without re-pairing**

On dial #3, without touching pairing: **Settings → About → Details**.

Expected: an **Update** pill is now present next to Back. Before this change it was absent — that difference, with no re-pair and no dial reflash, is the whole claim of the design (the CAPS fingerprint bumped `caps_seq`, the Link re-pulled, `SL2_FEAT_LINK_OTA_CREDS` arrived).

If the pill does not appear: check the node log for `serin_link` CAPS lines, confirm the dial shows the controller online, and power-cycle the dial once (a stale cached `caps_seq` is the first suspect). Report the result rather than working around it.

- [ ] **Step 4: Run the check as far as CONFIRM, then cancel**

Press Update. Expected sequence on the dial: `Checking` → the Link leaves the ESP-NOW link and joins Wi-Fi → either **"Up to date"** with the running version, or a **CONFIRM** face reading `v<current> -> v<manifest>`.

**Then cancel.** Do not install.

Dial #3 currently carries an uncommitted thermal-calibration build (286 cc offset, airflow term off) that a real install would overwrite, and that build is mid-experiment. Reaching CONFIRM proves the entire new path — creds relayed, network joined, NTP synced, HTTPS manifest fetched and parsed. Everything past CONFIRM is unchanged dial code already proven against the Serin Controller firmware.

Either outcome is a pass. "Up to date" means the bench dial is already at the manifest version; it still proves the relay, the join, and the fetch. A failure at **"Unit unreachable"** means the relay itself did not answer — that is the real regression to chase. **"WiFi failed"** means the credentials arrived but did not work; **"Update check failed"** means the network has no route to the manifest host.

- [ ] **Step 5: Confirm the Link recovers**

After cancelling, watch the dial return to its home face and HA's per-slot rows for Link #3 come back to connected.

Expected: recovery without any user action within roughly a minute. A Link that stays offline is a real finding — report it.

- [ ] **Step 6: Restore the node config**

Put `~/heatpump-master.yaml`'s `external_components:` block back to the released pin recorded in Step 1, and decide with the user whether `link_ota_credentials: true` stays. Re-flash with `~/.local/opt/esphome-venv/bin/esphome run heatpump-master.yaml`.

`~/heatpump-master.yaml` lives outside the repo and is the user's live config — leave it in the state they ask for, and say plainly which state that is.

- [ ] **Step 7: Report**

Write up what actually happened at each step — including which of the Step 4 outcomes occurred — and state plainly anything that was not verified. If the bench run could not happen at all (hardware unavailable, user declined), say the feature is unvalidated on hardware rather than implying it passed.

---

### Task 5: Release prep

**Files:**
- Modify: `README.md` (the `external_components:` pin in the quickstart, line ~35)

**Interfaces:**
- Consumes: Tasks 1-3 committed.
- Produces: nothing.

> Confirm the version number with the user before editing — the next tag may not be what this plan guessed, and a wrong pin in the quickstart is worse than none.

- [ ] **Step 1: Bump the quickstart pin**

In `README.md`, change the quickstart's `source: github://Serin-Labs/serin-link-core@v0.1.3-beta.11` to the agreed next tag (expected `v0.1.4-beta.2`). Check for other pinned references first:

```bash
cd /home/akifb/serin-link-core && grep -rn "v0\.1\.3-beta\.12" --include=*.md --include=*.yaml .
```

Update every hit that is a "use this version" instruction. Leave hits that are historical statements ("removed in v0.1.3-beta.11") alone — those are facts about the past, and the existing `_removed()` tombstones in `__init__.py` depend on them being accurate.

- [ ] **Step 2: Commit**

```bash
cd /home/akifb/serin-link-core
git add README.md
git commit -m "docs: bump quickstart pin to v0.1.4-beta.2"
```

- [ ] **Step 3: Stop**

Do not tag and do not push. Both need the user's explicit go-ahead.

---

## Notes for whoever executes this

- **Tasks 1 and 2 are one feature split across two review gates.** After Task 1 the repo is in a state where `esphome config` passes but `esphome compile` would fail, because `to_code` calls a setter that does not exist yet. That is intentional — Task 1's tests are validation-only for exactly this reason. Do not "fix" it by writing Task 2's C++ early; do not add a placeholder setter.
- **The dial firmware is not yours to touch.** If something looks like it needs a change in `/home/akifb/serin-link`, stop and report it — that would mean the design's central claim is wrong, and it is worth a conversation rather than a patch.
- **`test/run.sh` should pass unchanged throughout.** No task in this plan modifies a C core file.
