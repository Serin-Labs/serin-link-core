# Room Source: Retire Auto, Add External Sources — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the "Auto (last reporting)" room source from both controllers, rename the built-in entry to "Heat pump", and let ESPHome YAML bind external sensors (for example a Home Assistant temperature) into the controller-owned room-source catalog with zero guard lambdas.

**Architecture:** The ESPHome component keeps one persisted 64-bit `selected_source_id_` and projects it into three shapes: heat pump, pinned Link MAC, or external-source index. External sources are ESPHome `sensor::Sensor` pointers registered from codegen; the component subscribes to their state callbacks and feeds the single existing `on_room_temperature` trigger. The HomeKit controller drops its Auto catalog entry and resolves an unpinned Link selection at boot. No wire packet changes; one additive id namespace and helper in the shared header.

**Tech Stack:** C11 (shared core + host tests with gcc), C++17 ESPHome component (ESP-IDF), Python ESPHome codegen/schema, ESP-IDF v5.5 for the HomeKit build.

**Spec:** `docs/superpowers/specs/2026-09-05-room-source-no-auto-external-sources-design.md`

## Global Constraints

- Two repos: `~/serin-link-core` (tasks 1–8) and `~/mitsubishi-cn105-homekit` (tasks 9–10). Commit in whichever repo the task touches. Never `git push`.
- Do not add a `Claude-Session:` trailer or any Claude attribution to commit messages.
- `sl2_proto.h` exists as three byte-identical copies that must stay identical: `~/serin-link-core/include/serin_link/sl2_proto.h`, `~/serin-link-core/esphome/components/serin_link/sl2_proto.h`, `~/mitsubishi-cn105-homekit/main/sl2_proto.h`. (The dial's copy at `~/serin-link/main/sl2_proto.h` already diverges and is out of scope; do not touch it.)
- `SL2_ROOM_SOURCE_LINK_AUTO_ID` and `SL2_ROOM_SOURCE_NS_LINK_AUTO` stay defined. No packet layout, TLV, feature bit, or `SL2_PROTO_VERSION` change.
- Built-in entry label is exactly `Heat pump`. Link labels in ESPHome are exactly `Serin Link N` (1-based). Catalog names must fit `SL2_ROOM_SOURCE_NAME_LEN` (24 including NUL, so 23 characters).
- New YAML keys are all optional. `on_room_temperature` stays under `link_sensor:` with signature `x: float` (°C); it fires `0` when Heat pump is selected.
- Stale window is the existing `dial_stale_ms_` (90000 ms), not configurable.
- Host tests: `cd ~/serin-link-core/test && ./run.sh`. Schema tests: `cd ~/serin-link-core/test && ESPHOME=~/.local/opt/esphome-venv/bin/esphome ./esphome_schema.sh`.
- Max external sources: 7 (`SL2_ROOM_CATALOG_MAX` on the dial is 12; 12 − 1 heat pump − 4 `SL2_MAX_DIALS` = 7).

---

## File Structure

**serin-link-core**

- `include/serin_link/sl2_proto.h` (+ the two identical copies): add `SL2_ROOM_SOURCE_NS_EXTERNAL`, `sl2_room_source_name_id()`, retire comments on the Auto constants.
- `test/test_sl2_proto.c`: test for the name-id helper.
- `test/test_sl2_link.c`: host controller hooks lose Auto; a SET for Auto is answered BAD_SOURCE.
- `esphome/components/serin_link/__init__.py`: `sources:` schema, label resolution, retire `primary_select:`, option lists, codegen.
- `esphome/components/serin_link/serin_link.h` / `.cpp`: remove primary_select code, add external sources, new catalog/select mapping, status, triggers, migration, logging, dump_config.
- `test/esphome/*.yaml` + `test/esphome_schema.sh`: fixtures.
- `README.md`, `esphome/example_cn105.yaml`, `esphome/example_spike.yaml`, `esphome/packages/cn105.yaml`: docs and examples.

**mitsubishi-cn105-homekit**

- `main/sl2_proto.h`: synced copy.
- `main/espnow_link.cpp`: catalog without Auto, "Heat pump" label, v3 LINK edit pins sender, boot reconcile.
- `main/settings.cpp`: `room_source_id_derived` never yields Auto.

---

### Task 1: Shared header — external-source namespace and name-id helper

**Files:**
- Modify: `include/serin_link/sl2_proto.h:421-455`
- Copy to: `esphome/components/serin_link/sl2_proto.h`, `~/mitsubishi-cn105-homekit/main/sl2_proto.h`
- Test: `test/test_sl2_proto.c`

**Interfaces:**
- Produces: `#define SL2_ROOM_SOURCE_NS_EXTERNAL 4u`; `static inline uint64_t sl2_room_source_name_id(uint8_t ns, const char *name)` — FNV-1a 64 over the UTF-8 bytes of `name`, low 56 bits kept, `ns` in the top byte. Known vector: `sl2_room_source_name_id(4, "Home Assistant") == UINT64_C(0x043d164670e68fd6)`.

- [ ] **Step 1: Write the failing test**

Add to `test/test_sl2_proto.c`, after `test_room_source_mac_id`:

```c
/* External (YAML-declared) sources have no MAC. Their id is a 56-bit FNV-1a of
 * the display name under namespace 4 — stable across reflashes and list
 * reorderings, and the same function on the Python side (codegen detects
 * collisions) and the C side (the wire), so this pins the algorithm. */
static void test_room_source_name_id(void) {
    uint64_t ha = sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant");
    assert(ha == UINT64_C(0x043d164670e68fd6));
    assert((uint8_t)(ha >> 56) == SL2_ROOM_SOURCE_NS_EXTERNAL);
    uint64_t hall = sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Hallway");
    assert(hall == UINT64_C(0x04dddd7232305c79));
    assert(hall != ha);
    /* Deterministic, and never one of the well-known or retired ids. */
    assert(sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant") == ha);
    assert(ha != SL2_ROOM_SOURCE_INTERNAL_ID && ha != SL2_ROOM_SOURCE_AVERAGE_ID &&
           ha != SL2_ROOM_SOURCE_LINK_AUTO_ID);
    /* Not a MAC id in any MAC namespace. */
    uint8_t back[6];
    assert(!sl2_room_source_id_mac(ha, SL2_ROOM_SOURCE_NS_LINK, back));
    assert(!sl2_room_source_id_mac(ha, SL2_ROOM_SOURCE_NS_SENSOR, back));
    printf("room source name id ok\n");
}
```

Register it in `main()` right after `test_room_source_mac_id();`:

```c
    test_room_source_name_id();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd ~/serin-link-core/test && gcc -std=c11 -Wall -Wextra -Werror -I../include test_sl2_proto.c -o /tmp/test_sl2_proto -lm`
Expected: compile error `implicit declaration of function 'sl2_room_source_name_id'` and `'SL2_ROOM_SOURCE_NS_EXTERNAL' undeclared`.

- [ ] **Step 3: Implement in the header**

In `include/serin_link/sl2_proto.h`, replace the block comment and defines at lines 421–433 with:

```c
/* ── controller-defined room-temperature source catalog (v4) ───────── */
/* Well-known ids occupy namespace 0. Namespaces 1 and 2 are a u8 tag in the
 * high byte with a 6-byte MAC below it. Namespace 4 is a u8 tag with a 56-bit
 * FNV-1a of a display name below it (YAML-declared external sources — no
 * MAC to derive from). Ids 2..0xFF in namespace 0 and namespaces 5..255 are
 * reserved for core growth. Not every controller produces every id — the
 * ESPHome adapter emits Internal/External/Link, the HomeKit adopter also
 * emits Average and NS_SENSOR entries.
 *
 * Namespace 3 (automatic "last reporting Link wins") is RETIRED as of
 * 2026-09-05: no controller lists it and a ROOM_SOURCE_SET naming it is
 * answered SL2_ROOM_SET_BAD_SOURCE. The constants stay so every vendored copy
 * of this header remains identical and old stores can be recognised and
 * migrated. */
#define SL2_ROOM_SOURCE_INTERNAL_ID UINT64_C(0)
#define SL2_ROOM_SOURCE_AVERAGE_ID UINT64_C(1)
#define SL2_ROOM_SOURCE_NS_LINK 1u
#define SL2_ROOM_SOURCE_NS_SENSOR 2u
#define SL2_ROOM_SOURCE_NS_LINK_AUTO 3u   /* retired, see above */
#define SL2_ROOM_SOURCE_NS_EXTERNAL 4u
#define SL2_ROOM_SOURCE_LINK_AUTO_ID \
    ((uint64_t) SL2_ROOM_SOURCE_NS_LINK_AUTO << 56)   /* retired, see above */
```

After `sl2_room_source_id_mac()` (ends line 454), add:

```c
/* Id for a source that has a NAME but no MAC (YAML-declared external
 * sources). FNV-1a 64 over the UTF-8 bytes, low 56 bits kept, namespace in
 * the top byte. Codegen computes the same function to reject colliding
 * names before they reach the wire; renaming a source therefore changes its
 * id, which the controller treats exactly like removing it. */
static inline uint64_t sl2_room_source_name_id(uint8_t ns, const char *name) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (const unsigned char *p = (const unsigned char *) name; *p; p++) {
        h ^= *p;
        h *= UINT64_C(0x100000001b3);
    }
    return ((uint64_t) ns << 56) | (h & (UINT64_C(1) << 56) - 1);
}
```

Also update the `enum sl2_room_source_kind` comment: append `/* AUTO retired 2026-09-05; value kept */` after `SL2_ROOM_KIND_AUTO = 3,`.

- [ ] **Step 4: Sync the two other copies and verify identical**

```bash
cd ~/serin-link-core
cp include/serin_link/sl2_proto.h esphome/components/serin_link/sl2_proto.h
cp include/serin_link/sl2_proto.h ~/mitsubishi-cn105-homekit/main/sl2_proto.h
diff -q include/serin_link/sl2_proto.h esphome/components/serin_link/sl2_proto.h
diff -q include/serin_link/sl2_proto.h ~/mitsubishi-cn105-homekit/main/sl2_proto.h
```
Expected: no output from either `diff`.

- [ ] **Step 5: Run the host tests**

Run: `cd ~/serin-link-core/test && ./run.sh`
Expected: all binaries run; output includes `room source name id ok`. (Note: `-Wextra -Werror` — if gcc warns about `& (…) - 1` precedence, parenthesise as `(h & ((UINT64_C(1) << 56) - 1))`.)

- [ ] **Step 6: Commit (serin-link-core only; the HomeKit copy is committed in Task 9)**

```bash
cd ~/serin-link-core
git add include/serin_link/sl2_proto.h esphome/components/serin_link/sl2_proto.h test/test_sl2_proto.c
git commit -m "proto: external room-source namespace + name-id helper; retire Auto ids"
```

---

### Task 2: Host controller hooks drop Auto; a SET for Auto is rejected

**Files:**
- Modify: `test/test_sl2_link.c:189-240, 351-450`

**Interfaces:**
- Consumes: nothing new; `src/sl2_link.c` is unchanged (the policy lives in the hooks).

- [ ] **Step 1: Update the fake catalog and set hook**

In `test/test_sl2_link.c` replace the catalog construction inside `h_room_catalog` (the `catalog[0]`, `catalog[1]` and loop) with:

```c
    catalog[0] = (struct sl2_room_source_entry){
        SL2_ROOM_SOURCE_INTERNAL_ID, SL2_ROOM_KIND_INTERNAL,
        SL2_ROOM_SOURCE_F_SELECTABLE, "Heat pump" };
    catalog[1] = (struct sl2_room_source_entry){
        sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant"),
        SL2_ROOM_KIND_SENSOR, SL2_ROOM_SOURCE_F_SELECTABLE, "Home Assistant" };
    for (int i = 2; i < H_ROOM_CATALOG_N; i++) {
        const uint8_t mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t)i };
        catalog[i].id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
        catalog[i].kind = SL2_ROOM_KIND_LINK;
        catalog[i].flags = SL2_ROOM_SOURCE_F_SELECTABLE;
        snprintf(catalog[i].name, SL2_ROOM_SOURCE_NAME_LEN, "Serin Link %d", i - 1);
    }
```

Replace `h_room_set` with:

```c
#define H_ROOM_HA_ID sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, "Home Assistant")
static uint8_t h_room_set(void *c, uint32_t revision, uint64_t source_id) {
    (void)c;
    n_room_sets++;
    if (revision != h_room_revision) return SL2_ROOM_SET_STALE_CATALOG;
    /* The retired automatic id is deliberately NOT accepted. */
    if (source_id != SL2_ROOM_SOURCE_INTERNAL_ID && source_id != H_ROOM_HA_ID)
        return SL2_ROOM_SET_BAD_SOURCE;
    h_room_source = source_id;
    return SL2_ROOM_SET_OK;
}
```

- [ ] **Step 2: Update `test_room_source_catalog_and_set`**

Replace `assert(strcmp(r.entries[1].name, "Auto (last reporting)") == 0);` with:

```c
    assert(strcmp(r.entries[0].name, "Heat pump") == 0);
    assert(r.entries[1].id == H_ROOM_HA_ID && r.entries[1].kind == SL2_ROOM_KIND_SENSOR);
```

In the first SET block change `.source_id = SL2_ROOM_SOURCE_LINK_AUTO_ID,` to `.source_id = H_ROOM_HA_ID,` and the two following asserts to `a.source_id == H_ROOM_HA_ID`. In the "below the floor" block change `h_room_source == SL2_ROOM_SOURCE_LINK_AUTO_ID` to `h_room_source == H_ROOM_HA_ID`. In the stale block change `assert(a.source_id == SL2_ROOM_SOURCE_LINK_AUTO_ID);` to `assert(a.source_id == H_ROOM_HA_ID);`.

Then append, before the final `printf`, a retired-id case:

```c
    /* The retired automatic id: current revision, well-formed, and refused.
     * The ack still carries the authoritative (unchanged) selection. */
    set.request_id = 10;
    set.revision = h_room_revision;
    set.source_id = SL2_ROOM_SOURCE_LINK_AUTO_ID;
    sl2_link_on_recv(&l, d.mac, F.own, (const uint8_t *)&set, sizeof set);
    sl2_link_loop(&l);
    si = last_send_of(SL2_PKT_ROOM_SOURCE_ACK);
    sl2_decode_pkt(&a, sizeof a, F.sent[si].data, (int)F.sent[si].len);
    assert(a.request_id == 10 && a.result == SL2_ROOM_SET_BAD_SOURCE);
    assert(a.source_id == H_ROOM_HA_ID);
```

- [ ] **Step 3: Run the host tests**

Run: `cd ~/serin-link-core/test && ./run.sh`
Expected: `room source catalog + set ok` printed; no assertion failures.

- [ ] **Step 4: Commit**

```bash
cd ~/serin-link-core
git add test/test_sl2_link.c
git commit -m "test(link): controller hooks without Auto; retired id is BAD_SOURCE"
```

---

### Task 3: Python schema — `sources:`, retire `primary_select:`, option lists

**Files:**
- Modify: `esphome/components/serin_link/__init__.py` (constants ~79-137, `_PRIMARY_SELECT_SCHEMA` ~154-166, `primary_select_options`/`room_source_options` ~187-205, link_sensor schema ~276-292, `_validate` ~648-660, `to_code` ~933-975)
- Create: `test/esphome/pass_room_sources.yaml`, `test/esphome/fail_room_sources_duplicate_name.yaml`, `test/esphome/fail_room_sources_no_link_sensor.yaml`, `test/esphome/fail_primary_select_removed.yaml`
- Delete: `test/esphome/fail_room_source_with_primary_select.yaml`
- Modify: `test/esphome/pass_room_temperature_source.yaml` (comment), `test/esphome_schema.sh`

**Interfaces:**
- Produces (codegen calls the C++ that Task 5 defines): `var.add_room_source(<std::string label>, <sensor::Sensor*>)`. Option list order: `["Heat pump"] + [labels in YAML order] + ["Serin Link 1".."Serin Link N"]`.
- Removes: `PrimaryLinkSelect`, `set_primary_select`, `primary_select_options`, `PRIMARY_AUTO_LABEL`.

- [ ] **Step 1: Write the schema fixtures (failing)**

`test/esphome/pass_room_sources.yaml`:

```yaml
# room_temperature_source: sources: — external readings (any ESPHome sensor)
# offered in the same controller-owned dropdown/catalog as the Serin Links.
# One inherits its label from the sensor's name; one overrides it.
esphome:
  name: t-room-sources

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

sensor:
  - platform: template
    id: ha_room_temp
    name: "Home Assistant"
  - platform: template
    id: hall_temp
    name: "Hallway Template"

serin_link:
  id: serin
  max_links: 2
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
  room_temperature_source:
    sources:
      - sensor: ha_room_temp
      - sensor: hall_temp
        name: "Hallway"
```

`test/esphome/fail_room_sources_duplicate_name.yaml` (same preamble; `serin_link:` block):

```yaml
sensor:
  - platform: template
    id: a_temp
    name: "Home Assistant"
  - platform: template
    id: b_temp
    name: "Other"

serin_link:
  id: serin
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
  room_temperature_source:
    sources:
      - sensor: a_temp
      - sensor: b_temp
        name: "Home Assistant"
```

`test/esphome/fail_room_sources_no_link_sensor.yaml` (same preamble):

```yaml
sensor:
  - platform: template
    id: a_temp
    name: "Home Assistant"

serin_link:
  id: serin
  room_temperature_source:
    sources:
      - sensor: a_temp
```

`test/esphome/fail_primary_select_removed.yaml` (same preamble):

```yaml
serin_link:
  id: serin
  link_sensor:
    temperature:
      name: "Serin Link Temperature"
    primary_select:
```

Delete `test/esphome/fail_room_source_with_primary_select.yaml`. In `pass_room_temperature_source.yaml` change the header comment's "Options are Internal, Auto (last reporting), and one row per bond slot" to "Options are Heat pump, any `sources:`, and one row per bond slot".

In `test/esphome_schema.sh`: add to `MUST_CONTAIN`:

```bash
  [pass_room_sources.yaml]='Room Temperature Source'
```
In `MUST_REJECT_WITH` remove the `fail_room_source_with_primary_select.yaml` line and add:

```bash
  [fail_room_sources_duplicate_name.yaml]='same label'
  [fail_room_sources_no_link_sensor.yaml]='requires `link_sensor:`'
  [fail_primary_select_removed.yaml]='room_temperature_source: sources'
```

- [ ] **Step 2: Run schema tests to verify they fail**

Run: `cd ~/serin-link-core/test && ESPHOME=~/.local/opt/esphome-venv/bin/esphome ./esphome_schema.sh`
Expected: `FAIL pass_room_sources.yaml: did not validate` (unknown key `sources`), `FAIL fail_primary_select_removed.yaml: validated but should be rejected`.

- [ ] **Step 3: Implement the schema**

In `__init__.py`:

(a) Constants — after `CONF_ROOM_TEMPERATURE_SOURCE = "room_temperature_source"` add:

```python
CONF_SOURCES = "sources"
CONF_SENSOR = "sensor"
# Dial-side SL2_ROOM_CATALOG_MAX is 12: minus Heat pump, minus SL2_MAX_DIALS.
MAX_ROOM_SOURCES = 7
HEAT_PUMP_LABEL = "Heat pump"
# Must match SL2_ROOM_SOURCE_NAME_LEN - 1 in sl2_proto.h.
ROOM_SOURCE_NAME_MAX = 23
```

Delete `PRIMARY_AUTO_LABEL` and its comment block, delete `_PRIMARY_SELECT_SCHEMA`, delete `primary_select_options`. Delete the `PrimaryLinkSelect = serin_link_ns.class_(...)` declaration.

(b) Replace `room_source_options`:

```python
def room_source_labels(rts_conf):
    """Labels for `sources:` in YAML order — resolved once here so the HA
    option list, the collision check and the C++ registration all see the
    same strings."""
    return [_room_source_label(s) for s in (rts_conf or {}).get(CONF_SOURCES) or []]


def room_source_options(config):
    """Dropdown order is the contract with RoomSourceSelect::control(size_t):
    0 = Heat pump, then each external source, then one row per bond slot."""
    return (
        [HEAT_PUMP_LABEL]
        + room_source_labels(config.get(CONF_ROOM_TEMPERATURE_SOURCE))
        + link_slot_labels(slot_count(config))
    )


def _room_source_label(src):
    """A source's catalog label: `name:` if given, else the bound sensor's own
    name, found in the top-level `sensor:` list. Sensors declared elsewhere
    (inside another component) need an explicit `name:`."""
    if CONF_NAME in src:
        return src[CONF_NAME]
    wanted = src[CONF_SENSOR].id
    for ent in CORE.config.get(CONF_SENSOR) or []:
        if CONF_ID in ent and ent[CONF_ID].id == wanted and CONF_NAME in ent:
            return str(ent[CONF_NAME])
    raise cv.Invalid(
        f"room source `{wanted}` has no resolvable name — add `name:` to the "
        f"`sources:` entry"
    )


def _fnv1a56(label):
    """Mirror of sl2_room_source_name_id() — same bytes, same result."""
    h = 0xCBF29CE484222325
    for b in label.encode("utf-8"):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h & ((1 << 56) - 1)
```

Add `from esphome.core import CORE` and `CONF_ID` to the imports if not already present (check the existing `from esphome.const import (...)` list).

(c) Source entry schema and `_room_source_schema`:

```python
ROOM_SOURCE_ENTRY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=ROOM_SOURCE_NAME_MAX)),
    }
)

ROOM_SOURCE_SELECT_SCHEMA = select.select_schema(
    RoomSourceSelect, entity_category=ENTITY_CATEGORY_CONFIG
).extend(
    {
        # sources: — external readings offered alongside the Serin Links. Each
        # is any ESPHome sensor (typically `platform: homeassistant`); the
        # component subscribes to it, lists it on the dial and in this
        # dropdown, reports it stale after 90 s of silence, and feeds it to
        # on_room_temperature: while it is the selection. No YAML guards.
        cv.Optional(CONF_SOURCES): cv.All(
            cv.ensure_list(ROOM_SOURCE_ENTRY_SCHEMA),
            cv.Length(max=MAX_ROOM_SOURCES),
        ),
    }
)
```

(d) In the `link_sensor` schema, replace the `cv.Optional(CONF_PRIMARY_SELECT): lambda ...` entry (and its comment) with a tombstone:

```python
        cv.Optional(CONF_PRIMARY_SELECT): _removed(
            "`primary_select:` was removed in v0.1.5 along with the automatic "
            "\"last reporting\" room source: every Serin Link selection now pins "
            "one Link. Use `room_temperature_source:` (add `sources:` to offer "
            "an external reading such as a Home Assistant sensor)."
        ),
```

Update the `primary_link:` tombstone text to point at `room_temperature_source:` instead of `primary_select:`.

(e) In `_validate`, replace the `CONF_PRIMARY_SELECT in config[CONF_LINK_SENSOR]` branch with a label check (the `requires link_sensor:` branch stays):

```python
        labels = room_source_labels(config[CONF_ROOM_TEMPERATURE_SOURCE])
        seen = {}
        for label in labels:
            if len(label) > ROOM_SOURCE_NAME_MAX:
                raise cv.Invalid(
                    f"room source label `{label}` is longer than "
                    f"{ROOM_SOURCE_NAME_MAX} characters — set a shorter `name:`"
                )
            if label == HEAT_PUMP_LABEL or label.startswith("Serin Link"):
                raise cv.Invalid(
                    f"room source label `{label}` collides with a built-in entry"
                )
            key = _fnv1a56(label)
            if key in seen:
                raise cv.Invalid(
                    f"room sources `{seen[key]}` and `{label}` resolve to the "
                    f"same label/id — give one a distinct `name:`"
                )
            seen[key] = label
```

Note: `_validate` runs as `FINAL_VALIDATE_SCHEMA`/post-validation where `CORE.config` is populated; if it is a plain `cv.All` validator that runs before `CORE.config` exists, move this block into a `FINAL_VALIDATE_SCHEMA = cv.Schema(_validate_final, extra=cv.ALLOW_EXTRA)` using `esphome.final_validate.full_config.get()` in `_room_source_label` instead of `CORE.config`. Verify which by running the pass fixture; pick the one that resolves names.

(f) In `to_code`: delete the `if CONF_PRIMARY_SELECT in ls:` block. Replace the `room_temperature_source` block with:

```python
    if CONF_ROOM_TEMPERATURE_SOURCE in config:
        rts = config[CONF_ROOM_TEMPERATURE_SOURCE]
        sel = await select.new_select(rts, options=room_source_options(config))
        await cg.register_parented(sel, var)
        cg.add(var.set_room_source_select(sel))
        for src, label in zip(rts.get(CONF_SOURCES) or [], room_source_labels(rts)):
            sens = await cg.get_variable(src[CONF_SENSOR])
            cg.add(var.add_room_source(label, sens))
```

- [ ] **Step 4: Run schema tests to verify they pass**

Run: `cd ~/serin-link-core/test && ESPHOME=~/.local/opt/esphome-venv/bin/esphome ./esphome_schema.sh`
Expected: every `pass_*` prints PASS (including `pass_room_sources.yaml`), every `fail_*` prints PASS with the expected phrase. Note `esphome config` does not run `to_code`, so the C++ side is not required yet.

- [ ] **Step 5: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/__init__.py test/esphome test/esphome_schema.sh
git commit -m "esphome(schema): room_temperature_source sources:, retire primary_select"
```

---

### Task 4: C++ — remove primary_select and Auto; Heat pump label; v3 LINK pins sender

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h:100-125, 255-300, 385-400`
- Modify: `esphome/components/serin_link/serin_link.cpp` (`room_sensor_feed` ~843-866, `publish_dial_` ~963-969, `room_catalog_revision_` ~1101-1115, `room_catalog_entry_` ~1133-1161, `room_source_set` ~1215-1222, `room_source_select_control` ~1224-1245, `refresh_room_source_select_` ~1247-1268, primary_select functions 1089-1094 and 1270-1345, setup 1376-1415, loop ~1552-1558, dump_config ~1693-1702)

**Interfaces:**
- Produces: catalog order `0 = Heat pump, 1.. = bond slots` (Task 5 inserts external sources between). `room_source_apply_(uint64_t id)` unchanged signature. `has_primary_dial_` is true for every Link selection.
- Removes: `PrimaryLinkSelect`, `set_primary_select`, `primary_select_control`, `refresh_primary_select_`, `publish_primary_`, `primary_slot_`, `primary_select_`, `pub_primary_idx_`. Keeps `primary_pref_` (read-only, for the v3→v4 migration) and `last_primary_ms_` (the 1 Hz select refresh tick).

- [ ] **Step 1: Header edits**

In `serin_link.h`:
- Delete the `set_primary_select` declaration and its comment; delete `primary_select_control` and its comment.
- Delete members `select::Select *primary_select_{nullptr};`, `int pub_primary_idx_{-1};`, and the declarations of `refresh_primary_select_`, `primary_slot_`, `publish_primary_` with their comments. Keep `ESPPreferenceObject primary_pref_;` with a new comment: `/* Read once at boot for the v3->v4 migration; never written any more. */`
- Delete `class PrimaryLinkSelect ... ;`.
- Change the `room_src_is_link()` comment to: `/* True while a Serin Link (always a specific, pinned one) is the selected room source. */`

- [ ] **Step 2: Remove the primary_select implementation**

In `serin_link.cpp` delete these functions entirely: `publish_primary_` (~1089–1094), `primary_slot_` (~1271–1284), `primary_select_control` (~1286–1323), `refresh_primary_select_` (~1325–1345).

In `setup()`, keep the `primary_pref_` creation and load block (it feeds the migration) but change the comment to: `/* Stored v3 primary pin: [0]=set flag, [1..6]=MAC. Read-only since primary_select: was retired; consulted only by the first-boot-on-v4 fold below. */`

In `loop()` replace the block at ~1552–1558 with:

```cpp
  /* 1 Hz: republish the room-source dropdown from the CURRENT bond table and
   * drop a pin whose Serin Link has been forgotten. Quiet because
   * refresh_room_source_select_() gates on change. */
  if (room_source_select_ != nullptr && now - last_primary_ms_ >= 1000) {
    last_primary_ms_ = now;
    refresh_room_source_select_();
  }
```

In `dump_config()` replace the `primary Serin Link:` if/else with:

```cpp
    if (has_primary_dial_) {
      char mac[18];
      sl2_fmt_mac(primary_dial_, mac);
      ESP_LOGCONFIG(TAG, "    room source: Serin Link %s", mac);
    } else if (selected_source_id_ == SL2_ROOM_SOURCE_INTERNAL_ID) {
      ESP_LOGCONFIG(TAG, "    room source: Heat pump");
    }
```
(Task 5 adds the external branch.)

- [ ] **Step 3: Catalog and revision without Auto**

Replace `room_catalog_revision_`'s body:

```cpp
uint32_t SerinLinkComponent::room_catalog_revision_() const {
  uint32_t h = 2166136261u;
  auto mix_id = [&h](uint64_t id) {
    for (int i = 0; i < 8; i++) h = (h ^ (uint8_t)(id >> (i * 8))) * 16777619u;
  };
  mix_id(SL2_ROOM_SOURCE_INTERNAL_ID);
  const int n = sl2_link_dial_count(const_cast<sl2_link_t *>(&link_));
  for (int slot = 0; slot < n; slot++) {
    uint8_t mac[6];
    if (!sl2_link_dial_mac(const_cast<sl2_link_t *>(&link_), slot, mac)) continue;
    mix_id(sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac));
  }
  return h ? h : 1;
}
```

Replace `room_catalog_entry_`:

```cpp
/* One catalog entry by POSITION: 0 = Heat pump, 1+k = bond slot k. A pure
 * function of the index so room_catalog_page can fill the caller's window
 * directly. Returns false past the end. */
bool SerinLinkComponent::room_catalog_entry_(int idx,
                                             struct sl2_room_source_entry *e) const {
  const int bonds = sl2_link_dial_count(const_cast<sl2_link_t *>(&link_));
  if (idx < 0 || idx >= bonds + 1) return false;
  std::memset(e, 0, sizeof *e);
  e->flags = SL2_ROOM_SOURCE_F_SELECTABLE;
  if (idx == 0) {
    e->id = SL2_ROOM_SOURCE_INTERNAL_ID;
    e->kind = SL2_ROOM_KIND_INTERNAL;
    std::snprintf(e->name, SL2_ROOM_SOURCE_NAME_LEN, "Heat pump");
    return true;
  }
  const int slot = idx - 1;
  uint8_t mac[6];
  if (!sl2_link_dial_mac(const_cast<sl2_link_t *>(&link_), slot, mac)) return false;
  e->id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
  e->kind = SL2_ROOM_KIND_LINK;
  /* Numbered even when only one Link is bonded: the Python option list is
   * sized at BUILD time from max_links, so a live-count-dependent label would
   * name the same Link differently in Home Assistant and on the dial. */
  std::snprintf(e->name, SL2_ROOM_SOURCE_NAME_LEN, "Serin Link %d", slot + 1);
  return true;
}
```

Replace `room_source_set`:

```cpp
uint8_t SerinLinkComponent::room_source_set(uint32_t revision, uint64_t id) {
  if (revision != room_catalog_revision_()) return SL2_ROOM_SET_STALE_CATALOG;
  /* Only ids the catalog currently lists. The retired automatic id falls
   * through to BAD_SOURCE like any other unknown id. */
  if (id != SL2_ROOM_SOURCE_INTERNAL_ID && !room_source_slot_(id, nullptr))
    return SL2_ROOM_SET_BAD_SOURCE;
  room_source_apply_(id);
  char s[18];
  sl2_fmt_mac(primary_dial_, s);
  ESP_LOGI(TAG, "room source -> %s (set from Serin Link)",
           has_primary_dial_ ? s : "Heat pump");
  return SL2_ROOM_SET_OK;
}
```

Replace `room_source_select_control`:

```cpp
void SerinLinkComponent::room_source_select_control(size_t index) {
  uint64_t id = SL2_ROOM_SOURCE_INTERNAL_ID;
  if (index >= 1) {
    uint8_t mac[6];
    if (!sl2_link_dial_mac(&link_, static_cast<int>(index) - 1, mac)) {
      ESP_LOGW(TAG, "room source: Serin Link slot %u is empty — selection ignored",
               static_cast<unsigned>(index));
      refresh_room_source_select_();
      return;
    }
    id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
  }
  room_source_apply_(id);
}
```

In `refresh_room_source_select_` remove the `if (selected_source_id_ == SL2_ROOM_SOURCE_LINK_AUTO_ID) { idx = 1; } else` arm and change `idx = slot + 2;` to `idx = slot + 1;`. Change the fallback log text to `"... — reverting to Heat pump"`.

- [ ] **Step 4: v3 edit path pins the sender**

In `room_sensor_feed`, replace the `else { room_source_apply_(p->want_src == SL2_ROOMSRC_INTERNAL ? ... : SL2_ROOM_SOURCE_LINK_AUTO_ID); }` arm with:

```cpp
    } else if (p->want_src == SL2_ROOMSRC_INTERNAL || src_mac == nullptr) {
      room_source_apply_(SL2_ROOM_SOURCE_INTERNAL_ID);
    } else {
      /* A pre-catalog dial asking for "Link" means "use me": pin the sender.
       * The automatic id this used to map to is retired. */
      room_source_apply_(sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, src_mac));
    }
```

- [ ] **Step 5: First-boot fold no longer produces Auto**

In `setup()`, in the `else { /* First boot on v4 */ }` branch, replace the three-way assignment with:

```cpp
    /* First boot on v4: fold the two v3 blobs into the canonical id. An
     * unpinned Link choice cannot be resolved until the bond table is loaded
     * (sl2_link_init below) — leave the retired automatic id as a marker
     * that room_source_reconcile_() (Task 6) resolves right after. */
    if (selected_src_ != SL2_ROOMSRC_LINK)
      selected_source_id_ = SL2_ROOM_SOURCE_INTERNAL_ID;
    else if (has_primary_dial_)
      selected_source_id_ = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, primary_dial_);
    else
      selected_source_id_ = SL2_ROOM_SOURCE_LINK_AUTO_ID;
    room_source_id_pref_.save(&selected_source_id_);
```
(Behaviour is unchanged here on purpose; Task 6 adds the reconcile.)

- [ ] **Step 6: Compile**

Run: `cd ~/serin-link-core/test/esphome && ~/.local/opt/esphome-venv/bin/esphome compile pass_room_temperature_source.yaml 2>&1 | tail -30`
Expected: `Successfully compiled program` (first compile of the toolchain can take several minutes). Fix any reference to the deleted symbols the compiler reports.

- [ ] **Step 7: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "esphome: retire Auto and primary_select; Heat pump label; v3 Link edit pins sender"
```

---

### Task 5: C++ — external sources

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h` (public API near `set_room_source_select`; private members near `selected_source_id_`)
- Modify: `esphome/components/serin_link/serin_link.cpp` (`room_src_status_`, `publish_dial_`, `room_catalog_revision_`, `room_catalog_entry_`, `room_source_set`, `room_source_select_control`, `refresh_room_source_select_`, `room_source_apply_`, `room_source_project_`, `dump_config`)

**Interfaces:**
- Consumes: Task 3 codegen call `add_room_source(std::string, sensor::Sensor*)`; Task 1 `sl2_room_source_name_id`.
- Produces: `void add_room_source(const std::string &name, sensor::Sensor *s);` `int selected_ext_` (−1 = none) as the third projection; `void fire_room_temperature_(float t)`; `void room_source_changed_()`. Catalog/select order: `0 = Heat pump, 1..E = external, E+1+k = bond slot k`.

- [ ] **Step 1: Header additions**

Public, after `set_room_source_select`:

```cpp
  /* room_temperature_source: sources: — an external reading (any ESPHome
   * sensor) offered as a selectable room source. Registered in YAML order;
   * the position is the dropdown index (after Heat pump) and the catalog
   * position on the dial. The id is a hash of the label (sl2_proto.h). */
  void add_room_source(const std::string &name, sensor::Sensor *s);
```

Private, after `ESPPreferenceObject room_source_id_pref_;`:

```cpp
  struct ext_source_t {
    std::string name;
    sensor::Sensor *sensor;
    uint64_t id;
    float last{NAN};
    uint32_t last_ms{0};   /* millis() of the last non-NaN value; 0 = never */
  };
  std::vector<ext_source_t> ext_sources_;
  /* Third projection of selected_source_id_ (with has_primary_dial_ and
   * selected_src_): index into ext_sources_, or -1. */
  int selected_ext_{-1};
  int ext_index_(uint64_t id) const;
  void ext_state_(int idx, float v);
  /* on_room_temperature: fan-out. Every feed path goes through here. */
  void fire_room_temperature_(float t);
  /* After a selection change: push the new source's last known value (or 0
   * for Heat pump) so the heat pump does not wait for the next sample. */
  void room_source_changed_();
```

Add `#include <string>` / `<vector>` / `<cmath>` to the header if not already included.

- [ ] **Step 2: Registration and callbacks**

In `serin_link.cpp` add:

```cpp
void SerinLinkComponent::add_room_source(const std::string &name, sensor::Sensor *s) {
  ext_source_t e;
  e.name = name;
  e.sensor = s;
  e.id = sl2_room_source_name_id(SL2_ROOM_SOURCE_NS_EXTERNAL, name.c_str());
  const int idx = static_cast<int>(ext_sources_.size());
  ext_sources_.push_back(e);
  s->add_on_state_callback([this, idx](float v) { this->ext_state_(idx, v); });
}

int SerinLinkComponent::ext_index_(uint64_t id) const {
  for (size_t i = 0; i < ext_sources_.size(); i++)
    if (ext_sources_[i].id == id) return static_cast<int>(i);
  return -1;
}

void SerinLinkComponent::ext_state_(int idx, float v) {
  if (idx < 0 || idx >= static_cast<int>(ext_sources_.size())) return;
  if (std::isnan(v)) return;             /* NaN is "no reading", not a reading */
  ext_sources_[idx].last = v;
  ext_sources_[idx].last_ms = millis();
  if (selected_ext_ == idx) fire_room_temperature_(v);
}

void SerinLinkComponent::fire_room_temperature_(float t) {
  for (auto *trig : room_temp_triggers_) trig->trigger(t);
}

void SerinLinkComponent::room_source_changed_() {
  if (selected_ext_ >= 0) {
    const ext_source_t &e = ext_sources_[selected_ext_];
    if (e.last_ms != 0) fire_room_temperature_(e.last);
  } else if (has_primary_dial_) {
    if (dial_temp_ms_ != 0 && std::memcmp(dial_mac_, primary_dial_, 6) == 0)
      fire_room_temperature_(dial_temp_cc_ / 100.0f);
  } else {
    /* Heat pump: 0 is cn105's own "drop the remote temperature" value, and
     * takes effect immediately instead of after remote_temperature_timeout. */
    fire_room_temperature_(0.0f);
  }
}
```

In `publish_dial_` replace the final `if (room_src_is_link()) { ... trig->trigger(t); }` with:

```cpp
  if (room_src_is_link()) fire_room_temperature_(dial_temp_cc_ / 100.0f);
```

- [ ] **Step 3: Projection and apply**

Replace `room_source_project_`:

```cpp
void SerinLinkComponent::room_source_project_() {
  selected_ext_ = ext_index_(selected_source_id_);
  has_primary_dial_ = sl2_room_source_id_mac(selected_source_id_,
                                             SL2_ROOM_SOURCE_NS_LINK, primary_dial_);
  if (!has_primary_dial_) std::memset(primary_dial_, 0, sizeof primary_dial_);
  /* v3 coarse view for pre-catalog dials: an external source has no v3 word,
   * so it reads as Internal there. */
  selected_src_ = has_primary_dial_ ? SL2_ROOMSRC_LINK : SL2_ROOMSRC_INTERNAL;
}
```

Replace `room_source_apply_`:

```cpp
void SerinLinkComponent::room_source_apply_(uint64_t id) {
  const bool changed = id != selected_source_id_;
  selected_source_id_ = id;
  room_source_project_();
  room_source_id_pref_.save(&selected_source_id_);
  room_src_pref_.save(&selected_src_);
  refresh_room_source_select_();
  if (changed && started_) room_source_changed_();
}
```
(`started_` already exists; it is set once `setup()` completes, so boot-time applies do not fire the trigger.)

- [ ] **Step 4: Status**

Replace `room_src_status_`:

```cpp
uint8_t SerinLinkComponent::room_src_status_() const {
  if (selected_ext_ >= 0) {
    const ext_source_t &e = ext_sources_[selected_ext_];
    if (e.last_ms == 0) return SL2_ROOMST_UNAVAILABLE;
    return (millis() - e.last_ms >= dial_stale_ms_) ? SL2_ROOMST_STALE : SL2_ROOMST_OK;
  }
  switch (selected_src_) {
    case SL2_ROOMSRC_LINK:
      if (!dial_has_sensor_ || dial_temp_ms_ == 0) return SL2_ROOMST_UNAVAILABLE;
      return (millis() - dial_temp_ms_ >= dial_stale_ms_) ? SL2_ROOMST_STALE
                                                          : SL2_ROOMST_OK;
    case SL2_ROOMSRC_BLE:
      return SL2_ROOMST_UNAVAILABLE;
    default:
      return SL2_ROOMST_OK;   /* Heat pump: its own thermistor has no failure mode */
  }
}
```

- [ ] **Step 5: Catalog, revision, set, select with external entries**

`room_catalog_revision_`: after `mix_id(SL2_ROOM_SOURCE_INTERNAL_ID);` add:

```cpp
  for (const auto &e : ext_sources_) mix_id(e.id);
```

`room_catalog_entry_`: change the bound and insert the external arm:

```cpp
  const int ext = static_cast<int>(ext_sources_.size());
  const int bonds = sl2_link_dial_count(const_cast<sl2_link_t *>(&link_));
  if (idx < 0 || idx >= 1 + ext + bonds) return false;
  std::memset(e, 0, sizeof *e);
  e->flags = SL2_ROOM_SOURCE_F_SELECTABLE;
  if (idx == 0) { /* Heat pump — unchanged */ }
  if (idx <= ext) {
    const ext_source_t &x = ext_sources_[idx - 1];
    e->id = x.id;
    e->kind = SL2_ROOM_KIND_SENSOR;
    std::snprintf(e->name, SL2_ROOM_SOURCE_NAME_LEN, "%s", x.name.c_str());
    return true;
  }
  const int slot = idx - 1 - ext;
  /* ... Link arm unchanged ... */
```
Update the function's leading comment to `0 = Heat pump, 1..E = sources: in YAML order, E+1+k = bond slot k`.

`room_source_set`: the acceptance test becomes

```cpp
  if (id != SL2_ROOM_SOURCE_INTERNAL_ID && ext_index_(id) < 0 &&
      !room_source_slot_(id, nullptr))
    return SL2_ROOM_SET_BAD_SOURCE;
```
and the log line becomes:

```cpp
  ESP_LOGI(TAG, "room source -> %s (set from Serin Link)",
           selected_ext_ >= 0 ? ext_sources_[selected_ext_].name.c_str()
                              : has_primary_dial_ ? s : "Heat pump");
```

`room_source_select_control`:

```cpp
void SerinLinkComponent::room_source_select_control(size_t index) {
  const size_t ext = ext_sources_.size();
  uint64_t id = SL2_ROOM_SOURCE_INTERNAL_ID;
  if (index >= 1 && index <= ext) {
    id = ext_sources_[index - 1].id;
  } else if (index > ext) {
    uint8_t mac[6];
    const int slot = static_cast<int>(index - ext) - 1;
    if (!sl2_link_dial_mac(&link_, slot, mac)) {
      ESP_LOGW(TAG, "room source: Serin Link slot %d is empty — selection ignored", slot + 1);
      refresh_room_source_select_();
      return;
    }
    id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
  }
  room_source_apply_(id);
}
```

`refresh_room_source_select_`: compute the index as

```cpp
  int idx = 0, slot = -1;
  if (selected_ext_ >= 0) {
    idx = 1 + selected_ext_;
  } else if (room_source_slot_(selected_source_id_, &slot)) {
    idx = 1 + static_cast<int>(ext_sources_.size()) + slot;
  } else if (selected_source_id_ != SL2_ROOM_SOURCE_INTERNAL_ID) {
    /* fallback to Heat pump — unchanged apart from the log text */
  }
```

`dump_config`: add before the Heat pump branch:

```cpp
    } else if (selected_ext_ >= 0) {
      ESP_LOGCONFIG(TAG, "    room source: %s (external)", ext_sources_[selected_ext_].name.c_str());
```
and after the room-sensor block:

```cpp
    for (const auto &e : ext_sources_)
      ESP_LOGCONFIG(TAG, "    external room source: %s", e.name.c_str());
```

- [ ] **Step 6: Compile the sources fixture**

Run: `cd ~/serin-link-core/test/esphome && ~/.local/opt/esphome-venv/bin/esphome compile pass_room_sources.yaml 2>&1 | tail -30`
Expected: `Successfully compiled program`. Also confirm `grep -n "add_room_source" .esphome/build/t-room-sources/src/main.cpp` shows two calls with labels `"Home Assistant"` and `"Hallway"`.

- [ ] **Step 7: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "esphome: YAML-bound external room sources in the catalog, select and feed"
```

---

### Task 6: C++ — boot reconcile of stored selections

**Files:**
- Modify: `esphome/components/serin_link/serin_link.h` (private: `void room_source_reconcile_();`)
- Modify: `esphome/components/serin_link/serin_link.cpp` (`setup()` right after `sl2_link_init(...)` at ~1475)

**Interfaces:**
- Consumes: `room_source_apply_`, `ext_index_`, `room_source_slot_` from Tasks 4–5.
- Produces: `room_source_reconcile_()` — resolves the retired automatic id and any id absent from the current catalog.

- [ ] **Step 1: Implement**

Add to `serin_link.cpp`:

```cpp
/* Runs once after the bond table is loaded. Two stores need resolving:
 *  - the retired automatic id (written by pre-2026-09 builds, or by the
 *    first-boot fold above when a v3 Link choice carried no pin): becomes the
 *    sole bonded Link when exactly one is bonded, else Heat pump;
 *  - any id the current catalog does not list (external source removed or
 *    renamed in YAML, Link forgotten while powered off): Heat pump.
 * Boot-time, so room_source_apply_ does not fire on_room_temperature. */
void SerinLinkComponent::room_source_reconcile_() {
  uint64_t id = selected_source_id_;
  if (id == SL2_ROOM_SOURCE_LINK_AUTO_ID) {
    uint8_t mac[6];
    if (sl2_link_dial_count(&link_) == 1 && sl2_link_dial_mac(&link_, 0, mac)) {
      id = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
      ESP_LOGI(TAG, "room source: automatic is retired — pinned to the only bonded Serin Link");
    } else {
      id = SL2_ROOM_SOURCE_INTERNAL_ID;
      ESP_LOGI(TAG, "room source: automatic is retired — reverting to Heat pump");
    }
  } else if (id != SL2_ROOM_SOURCE_INTERNAL_ID && ext_index_(id) < 0 &&
             !room_source_slot_(id, nullptr)) {
    ESP_LOGW(TAG, "room source: stored selection is not in the catalog — reverting to Heat pump");
    id = SL2_ROOM_SOURCE_INTERNAL_ID;
  }
  if (id != selected_source_id_) {
    room_source_apply_(id);
  } else {
    room_source_project_();   /* selected_ext_ was unknown before sources were registered */
  }
}
```

In `setup()`, immediately after `sl2_link_init(&link_, &port_, &crypto_, &hvac_);` add:

```cpp
  room_source_reconcile_();
```

Also in `setup()`, the existing block `if (room_source_select_ != nullptr) { ... room_source_project_(); ... }` after loading `room_source_id_pref_`: drop the `room_source_select_ != nullptr` condition so projection always runs (the reconcile depends on it and sources may exist without a select entity only if someone sets `internal: true`; projection is harmless either way).

- [ ] **Step 2: Compile both fixtures**

Run: `cd ~/serin-link-core/test/esphome && ~/.local/opt/esphome-venv/bin/esphome compile pass_room_sources.yaml 2>&1 | tail -5 && ~/.local/opt/esphome-venv/bin/esphome compile pass_room_temperature_source.yaml 2>&1 | tail -5`
Expected: both `Successfully compiled program`.

- [ ] **Step 3: Commit**

```bash
cd ~/serin-link-core
git add esphome/components/serin_link/serin_link.h esphome/components/serin_link/serin_link.cpp
git commit -m "esphome: reconcile stored room source at boot (retired Auto, missing ids)"
```

---

### Task 7: Docs and examples

**Files:**
- Modify: `README.md:138-160`, `esphome/example_cn105.yaml:165-180`, `esphome/example_spike.yaml:45-55`, `esphome/packages/cn105.yaml` (the `on_value` guard around line 110-124 and the `remote_temp` sensor)

- [ ] **Step 1: README**

Replace the paragraph starting "The same confirmed selection is shown in Home Assistant…" through the `primary_select:` paragraph with:

```markdown
The same confirmed selection is shown in Home Assistant and on every paired
Serin Link. Options are `Heat pump` (the unit's own sensor), any external
sources you declare, and one `Serin Link N` entry per `max_links:` slot. A
Serin Link entry always means that specific Link; there is no automatic
"last reporting" mode. The selection is stored on the controller.

To offer a reading that lives outside the controller — a Home Assistant
temperature, for example — bind the sensor as a source. The component
subscribes to it, lists it on the dial, reports it stale after 90 s of
silence, and feeds it through `on_room_temperature:` while it is selected.
No guard lambdas are needed anywhere:

```yaml
sensor:
  - platform: homeassistant
    id: ha_room_temp
    name: "Home Assistant"
    entity_id: sensor.living_room_temperature

serin_link:
  link_sensor:
    on_room_temperature:
      - lambda: 'id(hvac).set_remote_temperature(x);'
  room_temperature_source:
    sources:
      - sensor: ha_room_temp          # label defaults to the sensor's name
```

`on_room_temperature:` receives the selected source's reading in °C, and `0`
when `Heat pump` is chosen — cn105's own "use the built-in sensor" value, so
the switch is immediate.

`link_sensor: primary_select:` has been removed; `room_temperature_source:`
replaces it.
```

- [ ] **Step 2: example_cn105.yaml**

Delete the `primary_select:` line and its comment block. Replace the `on_room_temperature:` comment with:

```yaml
    # Feed the heat pump the SELECTED room source's reading: a pinned Serin
    # Link, or any `sources:` entry below. Fires 0 when Heat pump is chosen.
```

Under `room_temperature_source:` (add the key if the example lacks it) add:

```yaml
  room_temperature_source:
    # Optional external readings. Each is any ESPHome sensor; its name is
    # the label on the dial and in the dropdown.
    sources:
      - sensor: ha_room_temp
```
and a matching `platform: homeassistant` sensor with `id: ha_room_temp` in the example's `sensor:` list, replacing any existing guarded `on_value` recipe that calls `set_remote_temperature`.

- [ ] **Step 3: example_spike.yaml and packages/cn105.yaml**

`example_spike.yaml`: delete the `primary_select:` block and its comment.

`packages/cn105.yaml`: remove the `on_value:` automation on the `remote_temp` homeassistant sensor (lines around 110–124) and its `room_src_is_link` guard; give that sensor an `id: ha_room_temp`; add `room_temperature_source: sources: [ { sensor: ha_room_temp } ]` under `serin_link:` and keep `on_room_temperature:` as the single feed. If the package uses a substitution for the HA entity, keep it.

- [ ] **Step 4: Validate the examples still parse**

Run: `cd ~/serin-link-core/esphome && for f in example_cn105.yaml example_spike.yaml; do ~/.local/opt/esphome-venv/bin/esphome config $f >/dev/null && echo "OK $f"; done`
Expected: `OK` for both (secrets may need a `secrets.yaml` stub in that directory; if so create one with placeholder `wifi_ssid`/`wifi_password` and do not commit it).

- [ ] **Step 5: Commit**

```bash
cd ~/serin-link-core
git add README.md esphome/example_cn105.yaml esphome/example_spike.yaml esphome/packages/cn105.yaml
git commit -m "docs: room source without Auto; external sources example; drop guard recipe"
```

---

### Task 8: serin-link-core full verification

- [ ] **Step 1: Host tests**

Run: `cd ~/serin-link-core/test && ./run.sh`
Expected: all pass.

- [ ] **Step 2: Schema tests**

Run: `cd ~/serin-link-core/test && ESPHOME=~/.local/opt/esphome-venv/bin/esphome ./esphome_schema.sh`
Expected: all PASS.

- [ ] **Step 3: Compile fixtures**

Run: `cd ~/serin-link-core/test/esphome && for f in pass_room_sources.yaml pass_room_temperature_source.yaml pass_link_sensor_links.yaml; do ~/.local/opt/esphome-venv/bin/esphome compile $f 2>&1 | tail -1; done`
Expected: three `Successfully compiled program` lines.

- [ ] **Step 4: Grep for leftovers**

Run: `cd ~/serin-link-core && grep -rn "LINK_AUTO\|last reporting\|primary_select\|room_src_is_link" --include=*.py --include=*.cpp --include=*.h --include=*.yaml --include=*.md . | grep -v "superpowers/\|sl2_proto.h\|_removed\|test_sl2_link.c\|test_sl2_proto.c"`
Expected: no output, except the `room_src_is_link()` declaration/uses inside `serin_link.h/.cpp` (it remains a valid internal predicate) and tombstone strings.

---

### Task 9: HomeKit controller — catalog and set without Auto

**Files:**
- Modify: `~/mitsubishi-cn105-homekit/main/sl2_proto.h` (already synced in Task 1)
- Modify: `~/mitsubishi-cn105-homekit/main/espnow_link.cpp:521-535, 672-695, 745-790`
- Modify: `~/mitsubishi-cn105-homekit/main/settings.cpp:475-490`

**Interfaces:**
- Produces: catalog `Heat pump`, Average (if selectable), BLE sensors, `Serin Link` / `Serin Link N`. `room_source_id_derived()` returns `SL2_ROOM_SOURCE_INTERNAL_ID` for an unpinned Link single (never the automatic id). v3 LINK edit pins `st.roomSourceId` to the sender.

- [ ] **Step 1: Catalog**

In `room_catalog_build` change `"Internal"` to `"Heat pump"` and delete the three lines:

```cpp
    /* "Automatic" only means something when there is a choice to make. */
    int links = sl2_link_dial_count(&s_link);
    if (links > 1)
        add(SL2_ROOM_SOURCE_LINK_AUTO_ID, SL2_ROOM_KIND_AUTO, "Serin Link (automatic)");
```
keeping `int links = sl2_link_dial_count(&s_link);` (the loop below needs it).

- [ ] **Step 2: Set path**

In `h_room_source_set` delete the line `else if (id == SL2_ROOM_SOURCE_LINK_AUTO_ID) single = ROOM_MEMBER_LINK;`. The MAC-pin loop below already handles every Link id, and an unknown id (including the retired one) falls to `SL2_ROOM_SET_BAD_SOURCE`.

- [ ] **Step 3: v3 edit pins the sender**

In `h_room_sensor` replace the body of the `if (is_edit && ...)` block with:

```cpp
        auto &st = settings.get();
        uint8_t single = room_single_from_legacy(p->want_src);
        uint64_t pin = single == ROOM_MEMBER_LINK && src_mac
                           ? sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, src_mac)
                           : st.roomSourceId;
        if (st.roomMode != 0 || st.roomSingle != single || st.roomSourceId != pin) {
            st.roomMode     = 0;
            st.roomSingle   = single;
            st.roomSourceId = pin;   // a pre-catalog "Link" edit means "use me"
            settings.save();         // re-derives roomSource; keeps a NS_LINK pin
            LOG_INFO("room source -> %u (set from dial)", (unsigned)p->want_src);
        }
```

- [ ] **Step 4: Derivation never yields Auto**

In `settings.cpp` `room_source_id_derived`, replace `return SL2_ROOM_SOURCE_LINK_AUTO_ID;` with `return SL2_ROOM_SOURCE_INTERNAL_ID;` and update the comment: `// Unpinned Link (pre-2026-09 store, or the pinned dial was forgotten): there is no automatic mode any more, so this resolves to the heat pump's own sensor until a Link is chosen again.` Note that `roomSingle` may still say `ROOM_MEMBER_LINK` here; Task 10's boot reconcile fixes the pair.

- [ ] **Step 5: Build**

Run: `cd ~/mitsubishi-cn105-homekit && source ~/esp/esp-idf/export.sh >/dev/null && idf.py build 2>&1 | tail -5`
Expected: `Project build complete.` (Use the build dir/sdkconfig this repo normally uses; check its README or `build/` before adding `-B`.)

- [ ] **Step 6: Commit**

```bash
cd ~/mitsubishi-cn105-homekit
git add main/sl2_proto.h main/espnow_link.cpp main/settings.cpp
git commit -m "room source: retire automatic Link entry; Heat pump label; v3 Link edit pins sender"
```

---

### Task 10: HomeKit controller — boot reconcile of an unpinned Link selection

**Files:**
- Modify: `~/mitsubishi-cn105-homekit/main/espnow_link.cpp` (after `sl2_link_init(...)` at ~1000)

**Interfaces:**
- Consumes: `settings.get()`, `sl2_link_dial_count`, `sl2_link_dial_mac`.

- [ ] **Step 1: Implement**

Add above the init function:

```cpp
/* Once the bond table is loaded: a Link selection with no pin (a store from
 * before the automatic mode was retired) becomes the sole bonded Link when
 * exactly one is bonded, otherwise Internal. A pin whose Link is gone is
 * already handled by the forget path. */
static void room_source_reconcile(void) {
    auto &st = settings.get();
    if (st.roomMode != 0 || st.roomSingle != ROOM_MEMBER_LINK) return;
    uint8_t pin[6];
    if (sl2_room_source_id_mac(st.roomSourceId, SL2_ROOM_SOURCE_NS_LINK, pin)) return;
    uint8_t mac[6];
    if (sl2_link_dial_count(&s_link) == 1 && sl2_link_dial_mac(&s_link, 0, mac)) {
        st.roomSourceId = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, mac);
        LOG_INFO("room source: automatic is retired — pinned to the only bonded dial");
    } else {
        st.roomSingle   = ROOM_MEMBER_INTERNAL;
        st.roomSourceId = SL2_ROOM_SOURCE_INTERNAL_ID;
        LOG_INFO("room source: automatic is retired — reverting to Internal");
    }
    settings.save();
}
```

Call `room_source_reconcile();` immediately after `sl2_link_init(&s_link, &s_port, &s_crypto, &s_hvac);`.

- [ ] **Step 2: Build**

Run: `cd ~/mitsubishi-cn105-homekit && source ~/esp/esp-idf/export.sh >/dev/null && idf.py build 2>&1 | tail -3`
Expected: `Project build complete.`

- [ ] **Step 3: Grep for leftovers**

Run: `cd ~/mitsubishi-cn105-homekit && grep -rn "LINK_AUTO\|automatic)" main/ | grep -v sl2_proto.h`
Expected: only the reconcile function's log strings.

- [ ] **Step 4: Commit**

```bash
cd ~/mitsubishi-cn105-homekit
git add main/espnow_link.cpp
git commit -m "room source: resolve unpinned Link selection at boot"
```

---

### Task 11: Bench validation (manual, user-run hardware)

**Setup:** controller "Heat Pump - Main" at http://192.168.30.165/, dial #4 (MAC …EB:64). The user's live YAML is not the stale `~/heatpump-main.yaml`; edit whichever config they actually flash.

- [ ] **Step 1: Point the controller at the branch and simplify the YAML**

In the live YAML: set `external_components` source to `github://Serin-Labs/serin-link-core@<commit from Task 7>` with `refresh: 0s` (or a local path); delete the `on_value:` block under the `Remote Temperature Sensor` homeassistant sensor and give it `id: ha_room_temp` and `name: "Home Assistant"`; add under `serin_link:`:

```yaml
  room_temperature_source:
    sources:
      - sensor: ha_room_temp
```
Keep `link_sensor: on_room_temperature:` as is. Flash.

- [ ] **Step 2: Verify the catalog**

On dial #4: Settings → Room Sensor → press. Expected list: `Heat pump`, `Home Assistant`, `Serin Link 1`. In HA the `Room Temperature Source` select shows the same three options.

- [ ] **Step 3: Verify each source feeds**

Pick `Serin Link 1` on the dial: within one status cycle the controller log's `[STATUS] [received]-> room C°` equals the dial's temperature rounded to 0.5. Pick `Home Assistant`: it equals the HA value (23.0 in the earlier log). Pick `Heat pump`: within one cycle the log shows the cn105 remote-temperature keep-alive stop and the room reading revert to the unit's thermistor (no 30-minute wait).

- [ ] **Step 4: Verify status and persistence**

With `Home Assistant` selected, disable the HA automation that publishes the sensor: after 90 s the dial's Settings row shows "Reading is stale". Reboot the controller: the selection survives and the log shows no "automatic is retired" line (the store was already canonical).

- [ ] **Step 5: Record results**

Note pass/fail per step in the PR description or the spec's testing section; any failure goes back to the corresponding task, not to a patch on top.
