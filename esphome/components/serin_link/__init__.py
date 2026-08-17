"""serin_link — Serin Link controller for ESPHome.

Bind any `climate` entity via climate_id and a Serin Link can pair with this
node: CAPS derive from the entity's ClimateTraits, STATE from its published
state, and its commands route through a ClimateCall. Without climate_id the
component runs a canned device (the coexistence spike).

Owns the ESP-NOW radio (encrypted peers + broadcast pairing), so it is
mutually exclusive with ESPHome's built-in `espnow:` component, which neither
implements ESP-NOW link-layer encryption nor shares the recv callback.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.core import HexInt
from esphome.components import (
    binary_sensor,
    button,
    climate,
    select,
    sensor,
    switch,
    text_sensor,
)
from esphome.const import (
    CONF_DEVICE_ID,
    CONF_HUMIDITY,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_NAME,
    CONF_TEMPERATURE,
    CONF_TRIGGER_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_SECOND,
)

CODEOWNERS = ["@Serin-Labs"]
DEPENDENCIES = ["wifi", "esp32"]
# climate core is always compiled in (the adapter references its types even
# when no entity is bound — spike mode); an actual entity is still optional.
# select/sensor/text_sensor are auto-loaded so the optional bindings always
# compile.
AUTO_LOAD = ["binary_sensor", "button", "climate", "select", "sensor", "switch", "text_sensor"]

serin_link_ns = cg.esphome_ns.namespace("serin_link")
SerinLinkComponent = serin_link_ns.class_("SerinLinkComponent", cg.Component)

PrimaryLinkSelect = serin_link_ns.class_(
    "PrimaryLinkSelect", select.Select, cg.Parented.template(SerinLinkComponent)
)
PairLinkButton = serin_link_ns.class_(
    "PairLinkButton", button.Button, cg.Parented.template(SerinLinkComponent)
)
ScreenSwitch = serin_link_ns.class_("ScreenSwitch", switch.Switch)
NightSwitch = serin_link_ns.class_("NightSwitch", switch.Switch)

PairStartAction = serin_link_ns.class_("PairStartAction", automation.Action)
PairCancelAction = serin_link_ns.class_("PairCancelAction", automation.Action)
ForgetDialAction = serin_link_ns.class_("ForgetDialAction", automation.Action)
ForgetAllDialsAction = serin_link_ns.class_("ForgetAllDialsAction", automation.Action)

CONF_ZONE_NAME = "zone_name"
CONF_CLIMATE_ID = "climate_id"
CONF_HVAC_LINK = "hvac_link"
CONF_HVAC_LINK_SENSOR = "hvac_link_sensor"
CONF_MAX_LINKS = "max_links"
CONF_LINK_DEVICES = "link_devices"
CONF_PAIR_BUTTON = "pair_button"
CONF_CONNECTED = "connected"
CONF_ON_ROOM_TEMPERATURE = "on_room_temperature"
CONF_VANE_V_SELECT = "vane_v_select"
CONF_VANE_H_SELECT = "vane_h_select"
CONF_CMD_DEBOUNCE = "cmd_debounce"
CONF_OUTSIDE_TEMP_SENSOR = "outside_temp_sensor"
CONF_COMPRESSOR_HZ_SENSOR = "compressor_hz_sensor"
CONF_STAGE_SENSOR = "stage_sensor"
CONF_SUB_MODE_SENSOR = "sub_mode_sensor"
CONF_AUTO_SUB_MODE_SENSOR = "auto_sub_mode_sensor"
CONF_BATTERY_SENSOR = "battery_sensor"
CONF_BATTERY_LOW_THRESHOLD = "battery_low_threshold"
CONF_RUNTIME_SENSOR = "runtime_sensor"
CONF_POWER_SENSOR = "power_sensor"
CONF_ENERGY_SENSOR = "energy_sensor"

CONF_LINK_SENSOR = "link_sensor"
CONF_LINK_MAC = "link_mac"
CONF_DIAL_MAC = "dial_mac"        # deprecated alias for link_mac (shipped v0.1.3-beta.3)
CONF_STALE_AFTER = "stale_after"
CONF_PRIMARY_LINK = "primary_link"
CONF_PRIMARY_SELECT = "primary_select"
CONF_SLOTS = "slots"


CONF_DIAGNOSTICS = "diagnostics"
CONF_BONDED_COUNT = "bonded_count"
CONF_PAIRING_STATUS = "pairing_status"
CONF_PAIRING_SECONDS = "pairing_seconds"
CONF_LINKS = "links"
CONF_LINKED = "linked"
CONF_LAST_SEEN = "last_seen"
CONF_FIRMWARE = "firmware"
CONF_SLOT = "slot"
CONF_WINDOW = "window"
CONF_SCREEN = "screen"
CONF_NIGHT = "night"

# Mirrors SL2_MAX_DIALS in sl2_bond.h. If that #define ever changes, change
# this with it: the C side silently ignores rows past the limit, so the error
# has to be raised here.
SL2_MAX_DIALS = 4

# The dropdown's options. Index 0 is "no pin"; index N is bond slot N-1. The
# C++ keys off the INDEX (PrimaryLinkSelect::control(size_t)), so these labels
# can be reworded freely — but their ORDER is the contract.
#
# The LENGTH is per-config: ESPHome fixes a select's options when the entity is
# built and Home Assistant caches them from the initial entity listing, so the
# list cannot grow or shrink as Serin Links come and go. Offering all four slots
# to an install that will only ever bond two means offering two choices that get
# refused — so the count comes from `slots:`, defaulting to however many
# `links:` rows the diagnostics block declares.
PRIMARY_AUTO_LABEL = "Auto (last reporting)"


def _removed(message):
    """Tombstone for a key that shipped and was then removed.

    A key someone's working config used deserves a sentence about where its
    job went, not ESPHome's generic "[x] is an invalid option". Raises only
    when the key is PRESENT.
    """

    def validator(_value):
        raise cv.Invalid(message)

    return validator


_PRIMARY_SELECT_SCHEMA = select.select_schema(
    PrimaryLinkSelect,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(
    {
        cv.Optional(CONF_SLOTS): _removed(
            "`slots:` was removed in v0.1.3-beta.11 — the dropdown always "
            "sizes itself from `max_links:` (or the declared rows). A "
            "dropdown longer than the install only offered selections that "
            "had to be refused. Just delete the key."
        ),
    }
)


def primary_select_options(slots):
    return [PRIMARY_AUTO_LABEL] + [f"Serin Link {i + 1}" for i in range(slots)]

# link_sensor: links: — per-slot temperature/humidity, one row per bond slot.
# The arbitrated pair below shows ONE reading (the primary Link's); these rows
# show every bonded Link's, non-primary included — two Links in two rooms means
# two temperatures, and arbitration only decides which one FEEDS the heat
# pump, not which one you may see. Rows are bond SLOTS (the compaction caveat
# on LINK_ROW_SCHEMA applies): when a forget shifts the table, a row goes
# unknown until its new occupant reports. Like the diagnostics rows, bare
# `links:` generates temperature+humidity per slot from max_links: /
# link_devices:; a hand-written list is for custom naming and must agree with
# max_links: on the count.
LINK_SENSOR_ROW_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            # Temperature resolves to 0.01 C on the wire; humidity does too,
            # but the sensor is +/-1.8 %RH so a second decimal would be pure
            # noise in HA.
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


def _sensor_links(value):
    # Bare `links:` parses as None — kept as None here, expanded into
    # generated rows (or rejected, if there is no max_links: to size from) by
    # _expand_max_links, which is the only place that can see max_links:.
    if value is None:
        return None
    return cv.All(cv.ensure_list(LINK_SENSOR_ROW_SCHEMA), _max_link_rows)(value)


# link_sensor: — entities fed by the SERIN LINK's own sensor, owned by this
# component (the bindings above point at entities the user already has; here
# the Serin Link is the data source, so there is nothing to bind to). Presence
# of the block is the opt-in and is what sets SL2_FEAT_LINK_SENSOR: without it
# the Serin Link never transmits and its Settings never grow a room-source cycle,
# so an existing config upgrades with no behavior change. Every child is
# optional — `link_sensor: {}` means "accept the Serin Link as a room source,
# create no HA entities".
LINK_SENSOR_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            # Temperature resolves to 0.01 C on the wire; humidity does too,
            # but the sensor is +/-1.8 %RH so a second decimal would be pure
            # noise in HA.
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Which Serin Link is currently feeding. Diagnostic: it only matters
        # when two Links bonded to one controller both offer a sensor, where
        # the value alternates between them (last reporting Link wins).
        cv.Optional(CONF_LINK_MAC): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # primary_select: — which bonded Serin Link feeds the entities above,
        # as a Home Assistant dropdown. Unset, whichever Link reported last
        # owns them, which with two Links in two rooms makes the entity — and
        # any heat pump fed from it — alternate between rooms. Set, only the
        # pinned Link's readings are used; other Links' room-source EDITS are
        # still honored (a Link re-sends an unacknowledged edit at ~3 Hz
        # forever, wire spec §10d) — arbitration decides whose reading is
        # used, never whether an edit is accepted. The choice is persisted by
        # MAC, so it survives a reboot and follows its Link across the slot
        # shuffle a forget causes. Bare `primary_select:` gets a default
        # name, like pair_button:.
        cv.Optional(CONF_PRIMARY_SELECT): lambda value: _PRIMARY_SELECT_SCHEMA(
            {CONF_NAME: "Primary Serin Link", **(value or {})}
            if value is None or isinstance(value, dict)
            else value
        ),
        # links: — per-slot rows (see LINK_SENSOR_ROW_SCHEMA above). Presence
        # of the key is the opt-in, so existing configs grow no entities.
        cv.Optional(CONF_LINKS): _sensor_links,
        cv.Optional(CONF_STALE_AFTER): _removed(
            "`stale_after:` was removed in v0.1.3-beta.11 — freshness "
            "derives from the Serin Link's fixed 20 s report cadence (a wire "
            "constant, not a preference): readings go unknown after 90 s of "
            "silence, three missed reports plus slack. Just delete the key."
        ),
        cv.Optional(CONF_PRIMARY_LINK): _removed(
            "`primary_link:` was removed in v0.1.3-beta.11 — use "
            "`primary_select:`, which does the same job at runtime, persists "
            "the choice by MAC across reboots and slot shuffles, and reverts "
            "safely when the pinned Serin Link is forgotten. Add "
            "`internal: true` to it if you wanted a pin without an HA entity."
        ),
        cv.Optional(CONF_DIAL_MAC): _removed(
            f"`{CONF_DIAL_MAC}:` was renamed to `{CONF_LINK_MAC}:` in "
            f"v0.1.3-beta.5 (deprecated with a warning since) and removed in "
            f"v0.1.3-beta.11."
        ),
        # on_room_temperature: — fires with x (°C) when a usable reading
        # arrives: non-NaN, from the arbitrated primary Link, and only while
        # the Serin Link is the SELECTED room source, so cycling back to
        # Internal on it stops the feed with no condition in the YAML. This
        # replaces the guarded on_value recipe the examples used to carry —
        # the typical body is one line:
        #   - lambda: 'id(hvac).set_remote_temperature(x);'
        cv.Optional(CONF_ON_ROOM_TEMPERATURE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(
                    automation.Trigger.template(cg.float_)
                ),
            }
        ),
    }
)


def _link_sensor_schema(value):
    # `link_sensor:` with no sub-keys parses as None, not {} — map it before
    # the dict schema runs so the bare form (every child optional, meaning
    # "accept the Serin Link as a room source, create no HA entities")
    # validates the same as the explicit `link_sensor: {}`.
    return LINK_SENSOR_SCHEMA(value if value is not None else {})


# diagnostics: — read-only visibility into the controller's bond table. Like
# link_sensor: these entities are owned by the component (there is nothing
# pre-existing to bind to), presence of the block is the opt-in, and every
# child is optional, so an install that omits it creates no entities and
# behaves exactly as before.
#
# A `links:` ROW IS A BOND SLOT, NOT A SERIN LINK. sl2_link_forget_dial() (the
# core keeps the wire spec's "dial" vocabulary) compacts the table, so
# forgetting slot 0 shifts every later Link down one and "Serin Link 2
# firmware" starts describing what used to be Link 3. Declare mac_address: on
# every row — it is what makes that shift visible in HA instead of silently
# relabelling. Rows past the number of bonded Links publish ""/off/NAN.
LINK_ROW_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_MAC_ADDRESS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LINKED): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LAST_SEEN): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_DURATION,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # The Link's actual panel state (DIAL_SENSOR report): on = lit, dim
        # and the glance face included; off = display asleep OR not reported.
        # Requires the screen: switch (enforced in _expand_max_links) —
        # without it the controller never declares SL2_FEAT_SCREEN and no
        # Link ever reports.
        cv.Optional(CONF_SCREEN): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)

def _max_link_rows(value):
    # cv.Length's stock message is "length of value must be at most 4", which
    # says nothing about WHY 4. The limit is the controller's bond table.
    if len(value) > SL2_MAX_DIALS:
        raise cv.Invalid(
            f"at most {SL2_MAX_DIALS} Serin Link rows — that is SL2_MAX_DIALS, the "
            f"size of the controller's bond table, so a {SL2_MAX_DIALS + 1}th "
            f"row could never be filled (got {len(value)})"
        )
    return value


# pair_button: — a component-owned Pair button, replacing the template-button
# + lambda recipe every example carried. Bare `pair_button:` gets a default
# name; window: is the pairing window it opens.
PAIR_BUTTON_SCHEMA = button.button_schema(PairLinkButton).extend(
    {
        cv.Optional(CONF_WINDOW, default="60s"): cv.positive_time_period_milliseconds,
    }
)


def _pair_button_schema(value):
    # Bare `pair_button:` (None) and dicts get the default name; anything
    # else falls through for the schema to reject with a real error.
    if value is None or isinstance(value, dict):
        value = {CONF_NAME: "Pair Serin Link", **(value or {})}
    return PAIR_BUTTON_SCHEMA(value)


# screen: — the presence gate for every paired Link's display, as one HA
# switch. ON = someone in the room (Links never sleep darker than the glance
# face); OFF = room empty (Links sleep at their dim threshold; local touch
# still wakes them). Optimistic and restored (default ON, so a controller
# that HA never touches behaves exactly as before the switch existed); the
# state fans out in STATE's flags2 via the normal change detection.
SCREEN_SCHEMA = switch.switch_schema(
    ScreenSwitch,
    default_restore_mode="RESTORE_DEFAULT_ON",
)


def _screen_schema(value):
    if value is None or isinstance(value, dict):
        value = {CONF_NAME: "Serin Link Screen", **(value or {})}
    return SCREEN_SCHEMA(value)


# night: — the sun-down gate for every paired Link's display, as one HA
# switch. ON = sun below civil twilight (Links cap their wake brightness);
# OFF = daytime. Drive it from sun.sun / a schedule / any automation.
# Restored OFF, so a controller HA never touches behaves exactly as before
# the switch existed.
NIGHT_SCHEMA = switch.switch_schema(
    NightSwitch,
    default_restore_mode="RESTORE_DEFAULT_OFF",
)


def _night_schema(value):
    if value is None or isinstance(value, dict):
        value = {CONF_NAME: "Serin Link Night", **(value or {})}
    return NIGHT_SCHEMA(value)


# diagnostics: connected: — "is any bonded Serin Link alive", the component-
# owned form of the template binary_sensor over any_dial_live() the examples
# used to declare. Not per-slot (the links: rows carry that); this is the one
# a dashboard card wants.
CONNECTED_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_CONNECTIVITY,
)


def _connected_schema(value):
    if value is None or isinstance(value, dict):
        value = {CONF_NAME: "Serin Link Connected", **(value or {})}
    return CONNECTED_SCHEMA(value)


DIAGNOSTICS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CONNECTED): _connected_schema,
        cv.Optional(CONF_BONDED_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PAIRING_STATUS): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_PAIRING_SECONDS): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND,
            accuracy_decimals=0,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LINKS): cv.All(
            cv.ensure_list(LINK_ROW_SCHEMA),
            _max_link_rows,
        ),
    }
)


def _diagnostics_schema(value):
    # `diagnostics:` with no sub-keys parses as None, not {} — same treatment
    # as _link_sensor_schema above.
    return DIAGNOSTICS_SCHEMA(value if value is not None else {})


# esp-idf framework required (raw nvs_*, esp_now encrypted peers);
# it is ESPHome's ESP32 default. (cv.only_with_esp_idf was removed in 2026.x.)
_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SerinLinkComponent),
        cv.Optional(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
        cv.Optional(CONF_ZONE_NAME, default=""): cv.string,
        # max_links: — how many Serin Links this install holds, stated ONCE.
        # It generates the per-slot diagnostics entities (so a hand-written
        # links: list is only for custom naming) and sizes the primary-Link
        # dropdown. See _expand_max_links for what a mismatch rejects.
        cv.Optional(CONF_MAX_LINKS): cv.int_range(min=1, max=SL2_MAX_DIALS),
        # link_devices: — optional HA sub-device grouping for the generated
        # slot entities: one esphome:-devices: id per slot, in slot order.
        # The generated entities then carry short names ("MAC") scoped to
        # their sub-device instead of "Serin Link N MAC" flat on the node.
        cv.Optional(CONF_LINK_DEVICES): cv.ensure_list(cv.string_strict),
        # Device-link health for the STATE hvac_link flag (drives the Link's
        # offline face). A generic climate entity exists whether or not the
        # device behind it answers, so bind the platform's own signal here,
        # e.g. cn105: `hvac_link: !lambda 'return id(hvac).isHeatpumpConnected();'`
        # Unset: NaN room temp on an entity that claims one = link down.
        cv.Optional(CONF_HVAC_LINK): cv.returning_lambda,
        # hvac_link_sensor: — the same signal from a binary_sensor the config
        # already has, for platforms that expose their link health as an
        # entity. Mutually exclusive with hvac_link: (enforced below): both
        # answer "is the device behind the climate entity actually talking".
        cv.Optional(CONF_HVAC_LINK_SENSOR): cv.use_id(binary_sensor.BinarySensor),
        # Vane axes for platforms that expose vanes as select entities (e.g.
        # cn105's vertical_vane_select / horizontal_vane_select): the option
        # list defines the wire positions IN ORDER; options named "auto" or
        # "swing" (case-insensitive) become the wire AUTO/SWING codes.
        cv.Optional(CONF_VANE_V_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_VANE_H_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_CMD_DEBOUNCE): _removed(
            "`cmd_debounce:` was removed in v0.1.3-beta.11 — the 300 ms "
            "burst-merge window is internal tuning, matched to the Serin "
            "Link's detent rate and the state-overlay timeout, not a "
            "preference. Just delete the key."
        ),
        # Telemetry bindings -> INFO TLVs + CAPS feature bits (Serin Link telemetry
        # pages). All optional; unbound = TLV omitted, feature bit unset, the
        # the Link hides the row. Any platform's entities work — for echavet's
        # cn105 give its sensor blocks ids and bind them here.
        cv.Optional(CONF_OUTSIDE_TEMP_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_COMPRESSOR_HZ_SENSOR): cv.use_id(sensor.Sensor),
        # stage/sub-mode are text on cn105; strings map to wire codes via the
        # Mitsubishi value tables in sl2_info.h (wire spec §9, COMPRESSOR TLV)
        cv.Optional(CONF_STAGE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_SUB_MODE_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Optional(CONF_AUTO_SUB_MODE_SENSOR): cv.use_id(text_sensor.TextSensor),
        # battery_sensor: percent (0-100) from ANY sensor (BLE, HA import, …);
        # also drives the STATE low-battery flag (fixed at 10%, +5% clear
        # hysteresis).
        cv.Optional(CONF_BATTERY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BATTERY_LOW_THRESHOLD): _removed(
            "`battery_low_threshold:` was removed in v0.1.3-beta.11 — the "
            "low-battery flag is fixed at 10% with +5% clear hysteresis. "
            "Just delete the key."
        ),
        cv.Optional(CONF_RUNTIME_SENSOR): cv.use_id(sensor.Sensor),
        # ENERGY TLV: power_sensor in W, energy_sensor in kWh (sent as Wh);
        # either alone is fine — the other half rides as n/a.
        cv.Optional(CONF_POWER_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_PAIR_BUTTON): _pair_button_schema,
        cv.Optional(CONF_SCREEN): _screen_schema,
        cv.Optional(CONF_NIGHT): _night_schema,
        cv.Optional(CONF_LINK_SENSOR): _link_sensor_schema,
        cv.Optional(CONF_DIAGNOSTICS): _diagnostics_schema,
    }
).extend(cv.COMPONENT_SCHEMA)


# The four entities every bond slot gets, with their in-sub-device names.
_ROW_ENTITIES = (
    (CONF_MAC_ADDRESS, "MAC"),
    (CONF_LINKED, "Connected"),
    (CONF_LAST_SEEN, "Last Seen"),
    (CONF_FIRMWARE, "Firmware"),
)

# What a generated link_sensor: links: row holds, with in-sub-device names.
_SENSOR_ROW_ENTITIES = (
    (CONF_TEMPERATURE, "Temperature"),
    (CONF_HUMIDITY, "Humidity"),
)


def _expand_sensor_links(config, n, devs):
    """Expand (or check) link_sensor: links: against max_links/link_devices.

    Runs even with no max_links: — a bare `links:` must be rejected then (there
    is nothing to size it from), while a hand-written list stands on its own.
    """
    ls = config.get(CONF_LINK_SENSOR)
    if ls is None or CONF_LINKS not in ls:
        return config
    rows = ls[CONF_LINKS]
    if rows is None:
        if n is None:
            raise cv.Invalid(
                f"bare `{CONF_LINKS}:` under `{CONF_LINK_SENSOR}:` needs "
                f"`{CONF_MAX_LINKS}:` (or `{CONF_LINK_DEVICES}:`) to know how "
                f"many slots to generate — or write the rows out"
            )
        generated = []
        for i in range(n):
            row = {}
            for key, label in _SENSOR_ROW_ENTITIES:
                row[key] = (
                    {CONF_NAME: label, CONF_DEVICE_ID: devs[i]}
                    if devs is not None
                    else {CONF_NAME: f"Serin Link {i + 1} {label}"}
                )
            generated.append(LINK_SENSOR_ROW_SCHEMA(row))
        rows = generated
    elif n is not None and len(rows) != n:
        # Unlike the diagnostics rows, a hand-written list here MAY coexist
        # with link_devices: (which keeps serving the generated diagnostics
        # rows); scoping these rows is then per-entity device_id:.
        raise cv.Invalid(
            f"`{CONF_LINK_SENSOR}: {CONF_LINKS}:` declares {len(rows)} rows "
            f"but `{CONF_MAX_LINKS}:` is {n} — they describe the same slots "
            f"and must agree (drop the list unless you need custom names)"
        )
    else:
        return config
    config = dict(config)
    ls = dict(ls)
    ls[CONF_LINKS] = rows
    config[CONF_LINK_SENSOR] = ls
    return config


def _expand_max_links(config):
    """Cross-key checks, then expand max_links into diagnostics.links rows.

    Expansion happens HERE, at validation time, so to_code sees exactly what
    a hand-written config would produce — the generated rows go through
    LINK_ROW_SCHEMA like user-typed ones (auto IDs, device_id resolution),
    and the primary-select slot default (len of the rows) needs no new rule.
    """
    if CONF_HVAC_LINK in config and CONF_HVAC_LINK_SENSOR in config:
        raise cv.Invalid(
            f"`{CONF_HVAC_LINK}:` and `{CONF_HVAC_LINK_SENSOR}:` both bind "
            f"device-link health — set one, not both"
        )
    if CONF_SCREEN not in config:
        # Checked HERE (not in LINK_ROW_SCHEMA, which cannot see its siblings)
        # and BEFORE the max_links early-return, so a bare hand-written links:
        # list is covered too.
        for row in ((config.get(CONF_DIAGNOSTICS) or {}).get(CONF_LINKS) or ()):
            if CONF_SCREEN in row:
                raise cv.Invalid(
                    f"a `{CONF_LINKS}:` row's `{CONF_SCREEN}:` status entity "
                    f"requires `{CONF_SCREEN}:` on serin_link itself — without "
                    f"the switch the controller never declares SL2_FEAT_SCREEN, "
                    f"no Link ever reports, and the row would read off forever"
                )
    n = config.get(CONF_MAX_LINKS)
    devs = config.get(CONF_LINK_DEVICES)
    if devs is not None:
        if n is None:
            n = len(devs)
            if n > SL2_MAX_DIALS:
                raise cv.Invalid(
                    f"`{CONF_LINK_DEVICES}:` names {n} devices but the bond "
                    f"table holds at most {SL2_MAX_DIALS} Serin Links"
                )
        elif len(devs) != n:
            raise cv.Invalid(
                f"`{CONF_LINK_DEVICES}:` names {len(devs)} devices but "
                f"`{CONF_MAX_LINKS}:` is {n} — one device per slot, in slot "
                f"order"
            )
    config = _expand_sensor_links(config, n, devs)
    if n is None:
        return config
    config = dict(config)
    diag = dict(config.get(CONF_DIAGNOSTICS) or {})
    rows = diag.get(CONF_LINKS)
    if rows is not None:
        # A hand-written links: list is for custom naming, not a second place
        # to state the count — the two must agree, and per-row device_id
        # belongs on those rows rather than in link_devices:.
        if devs is not None:
            raise cv.Invalid(
                f"`{CONF_LINK_DEVICES}:` only applies to generated rows; "
                f"with an explicit `{CONF_LINKS}:` list, put `device_id:` on "
                f"each row's entities instead"
            )
        if len(rows) != n:
            raise cv.Invalid(
                f"`{CONF_LINKS}:` declares {len(rows)} rows but "
                f"`{CONF_MAX_LINKS}:` is {n} — they describe the same slots "
                f"and must agree (drop the list unless you need custom names)"
            )
        return config
    generated = []
    # The Screen status row only exists alongside the screen: switch — it is
    # the switch that makes Links report, and generating it unconditionally
    # would sprout a new HA entity under every existing max_links user on
    # upgrade.
    row_entities = (
        _ROW_ENTITIES + ((CONF_SCREEN, "Screen"),)
        if CONF_SCREEN in config
        else _ROW_ENTITIES
    )
    for i in range(n):
        row = {}
        for key, label in row_entities:
            ent = (
                {CONF_NAME: label, CONF_DEVICE_ID: devs[i]}
                if devs is not None
                else {CONF_NAME: f"Serin Link {i + 1} {label}"}
            )
            row[key] = ent
        generated.append(LINK_ROW_SCHEMA(row))
    diag[CONF_LINKS] = generated
    config[CONF_DIAGNOSTICS] = diag
    return config


CONFIG_SCHEMA = cv.All(_BASE_SCHEMA, _expand_max_links)


def _no_builtin_espnow(config):
    full = fv.full_config.get()
    if "espnow" in full:
        raise cv.Invalid(
            "serin_link owns the ESP-NOW radio (encrypted peers, recv callback); "
            "remove the `espnow:` component — it cannot coexist and does not "
            "support ESP-NOW link-layer encryption."
        )
    return config


FINAL_VALIDATE_SCHEMA = _no_builtin_espnow


# ── automation actions ────────────────────────────────────────────────────
# So configs stop reaching for raw lambdas to open a pairing window or drop a
# Serin Link. forget_link is the one that needed a decision: `slot:` is a
# bond-table POSITION (templatable, but it shifts when another Link is
# forgotten) and `mac_address:` is one specific Link, resolved at compile time
# — which is what
# keeps runtime MAC string parsing out of the component entirely.


@automation.register_action(
    "serin_link.pair_start",
    PairStartAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(SerinLinkComponent),
            cv.Optional(
                CONF_WINDOW, default="60s"
            ): cv.templatable(cv.positive_time_period_milliseconds),
        }
    ),
    # play() calls straight through to the component and returns — nothing is
    # deferred to a callback, timer, or loop().
    synchronous=True,
)
async def pair_start_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    # Both branches must go through cg.templatable(): since ESPHome 2026.4 a
    # TEMPLATABLE_VALUE field is TemplatableFn-backed and static_asserts if
    # handed a raw constant, so the static case needs the wrapper too (to_exp
    # converts the TimePeriod to milliseconds).
    templ = await cg.templatable(
        config[CONF_WINDOW], args, cg.uint32, to_exp=lambda v: v.total_milliseconds
    )
    cg.add(var.set_window(templ))
    return var


@automation.register_action(
    "serin_link.pair_cancel",
    PairCancelAction,
    cv.Schema({cv.GenerateID(): cv.use_id(SerinLinkComponent)}),
    synchronous=True,
)
async def pair_cancel_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def _exactly_one_target(config):
    has_slot = CONF_SLOT in config
    has_mac = CONF_MAC_ADDRESS in config
    if has_slot == has_mac:
        raise cv.Invalid(
            "serin_link.forget_link needs exactly one of `slot:` or "
            "`mac_address:` — `slot:` is a bond-table position (which shifts "
            "when an earlier Serin Link is forgotten), `mac_address:` is one "
            "specific Link."
        )
    return config


@automation.register_action(
    "serin_link.forget_link",
    ForgetDialAction,
    cv.All(
        cv.Schema(
            {
                cv.GenerateID(): cv.use_id(SerinLinkComponent),
                cv.Optional(CONF_SLOT): cv.templatable(
                    cv.int_range(min=0, max=SL2_MAX_DIALS - 1)
                ),
                cv.Optional(CONF_MAC_ADDRESS): cv.mac_address,
            }
        ),
        _exactly_one_target,
    ),
    synchronous=True,
)
async def forget_dial_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if CONF_SLOT in config:
        templ = await cg.templatable(config[CONF_SLOT], args, cg.int_)
        cg.add(var.set_slot(templ))
    else:
        # Same idiom as wifi's set_bssid: a list of HexInt renders as a braced
        # initializer, which binds to a const std::array<uint8_t,6>&.
        cg.add(var.set_mac([HexInt(i) for i in config[CONF_MAC_ADDRESS].parts]))
    return var


@automation.register_action(
    "serin_link.forget_all_links",
    ForgetAllDialsAction,
    cv.Schema({cv.GenerateID(): cv.use_id(SerinLinkComponent)}),
    synchronous=True,
)
async def forget_all_dials_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_zone_name(config[CONF_ZONE_NAME]))
    if CONF_CLIMATE_ID in config:
        entity = await cg.get_variable(config[CONF_CLIMATE_ID])
        cg.add(var.set_climate(entity))
    if CONF_HVAC_LINK in config:
        lam = await cg.process_lambda(
            config[CONF_HVAC_LINK], [], return_type=cg.bool_
        )
        cg.add(var.set_hvac_link_lambda(lam))
    if CONF_HVAC_LINK_SENSOR in config:
        sens = await cg.get_variable(config[CONF_HVAC_LINK_SENSOR])
        cg.add(var.set_hvac_link_sensor(sens))
    if CONF_PAIR_BUTTON in config:
        pb = config[CONF_PAIR_BUTTON]
        btn = await button.new_button(pb)
        await cg.register_parented(btn, var)
        cg.add(btn.set_window(pb[CONF_WINDOW].total_milliseconds))
    if CONF_SCREEN in config:
        sw = await switch.new_switch(config[CONF_SCREEN])
        cg.add(var.set_screen_switch(sw))
    if CONF_NIGHT in config:
        sw = await switch.new_switch(config[CONF_NIGHT])
        cg.add(var.set_night_switch(sw))
    if CONF_VANE_V_SELECT in config:
        sel = await cg.get_variable(config[CONF_VANE_V_SELECT])
        cg.add(var.set_vane_v_select(sel))
    if CONF_VANE_H_SELECT in config:
        sel = await cg.get_variable(config[CONF_VANE_H_SELECT])
        cg.add(var.set_vane_h_select(sel))
    for key, setter in (
        (CONF_OUTSIDE_TEMP_SENSOR, var.set_outside_temp_sensor),
        (CONF_COMPRESSOR_HZ_SENSOR, var.set_compressor_hz_sensor),
        (CONF_STAGE_SENSOR, var.set_stage_sensor),
        (CONF_SUB_MODE_SENSOR, var.set_sub_mode_sensor),
        (CONF_AUTO_SUB_MODE_SENSOR, var.set_auto_sub_mode_sensor),
        (CONF_BATTERY_SENSOR, var.set_battery_sensor),
        (CONF_RUNTIME_SENSOR, var.set_runtime_sensor),
        (CONF_POWER_SENSOR, var.set_power_sensor),
        (CONF_ENERGY_SENSOR, var.set_energy_sensor),
    ):
        if key in config:
            ent = await cg.get_variable(config[key])
            cg.add(setter(ent))
    if CONF_LINK_SENSOR in config:
        ls = config[CONF_LINK_SENSOR]
        cg.add(var.set_link_sensor_enabled())
        if CONF_PRIMARY_SELECT in ls:
            ps = ls[CONF_PRIMARY_SELECT]
            rows = (config.get(CONF_DIAGNOSTICS) or {}).get(CONF_LINKS) or []
            slots = len(rows) or len(ls.get(CONF_LINKS) or []) or SL2_MAX_DIALS
            sel = await select.new_select(ps, options=primary_select_options(slots))
            await cg.register_parented(sel, var)
            cg.add(var.set_primary_select(sel))
        if CONF_TEMPERATURE in ls:
            cg.add(var.set_dial_temp_sensor(await sensor.new_sensor(ls[CONF_TEMPERATURE])))
        if CONF_HUMIDITY in ls:
            cg.add(var.set_dial_hum_sensor(await sensor.new_sensor(ls[CONF_HUMIDITY])))
        if CONF_LINK_MAC in ls:
            cg.add(
                var.set_dial_mac_sensor(
                    await text_sensor.new_text_sensor(ls[CONF_LINK_MAC])
                )
            )
        for idx, row in enumerate(ls.get(CONF_LINKS) or []):
            temp = (
                await sensor.new_sensor(row[CONF_TEMPERATURE])
                if CONF_TEMPERATURE in row
                else cg.nullptr
            )
            hum = (
                await sensor.new_sensor(row[CONF_HUMIDITY])
                if CONF_HUMIDITY in row
                else cg.nullptr
            )
            cg.add(var.add_sensor_row(idx, temp, hum))
        for conf in ls.get(CONF_ON_ROOM_TEMPERATURE, []):
            trig = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
            cg.add(var.add_room_temp_trigger(trig))
            await automation.build_automation(trig, [(cg.float_, "x")], conf)
    if CONF_DIAGNOSTICS in config:
        diag = config[CONF_DIAGNOSTICS]
        if CONF_CONNECTED in diag:
            cg.add(
                var.set_connected_sensor(
                    await binary_sensor.new_binary_sensor(diag[CONF_CONNECTED])
                )
            )
        if CONF_BONDED_COUNT in diag:
            cg.add(
                var.set_bonded_count_sensor(
                    await sensor.new_sensor(diag[CONF_BONDED_COUNT])
                )
            )
        if CONF_PAIRING_STATUS in diag:
            cg.add(
                var.set_pairing_status_sensor(
                    await text_sensor.new_text_sensor(diag[CONF_PAIRING_STATUS])
                )
            )
        if CONF_PAIRING_SECONDS in diag:
            cg.add(
                var.set_pairing_seconds_sensor(
                    await sensor.new_sensor(diag[CONF_PAIRING_SECONDS])
                )
            )
        for idx, row in enumerate(diag.get(CONF_LINKS, [])):
            mac = (
                await text_sensor.new_text_sensor(row[CONF_MAC_ADDRESS])
                if CONF_MAC_ADDRESS in row
                else cg.nullptr
            )
            linked = (
                await binary_sensor.new_binary_sensor(row[CONF_LINKED])
                if CONF_LINKED in row
                else cg.nullptr
            )
            last_seen = (
                await sensor.new_sensor(row[CONF_LAST_SEEN])
                if CONF_LAST_SEEN in row
                else cg.nullptr
            )
            firmware = (
                await text_sensor.new_text_sensor(row[CONF_FIRMWARE])
                if CONF_FIRMWARE in row
                else cg.nullptr
            )
            cg.add(var.add_dial_row(idx, mac, linked, last_seen, firmware))
            if CONF_SCREEN in row:
                scr = await binary_sensor.new_binary_sensor(row[CONF_SCREEN])
                cg.add(var.set_dial_screen_sensor(idx, scr))
    # No IDF managed component is declared for crypto: Ed25519 + X25519 are
    # vendored (Monocypher, see the comment over the crypto hooks in
    # serin_link.cpp), and HMAC-SHA256 is pinned in-tree in sl2_sha256.h.
    #
    # Depending on espressif/libsodium is not an option here, however it is
    # keyed. `api: encryption:` pulls esphome/noise-c, which ESPHome converts
    # into an IDF component named `libsodium`; sharing that key means ESPHome's
    # entry silently overwrites ours (its curated subset has no crypto_sign_*,
    # so signing link-errors), and using a different key trips the component
    # manager's name-without-namespace collision check outright:
    #
    #   Requirement espressif__libsodium and requirement libsodium are both
    #   added as "project_managed_components". Can't decide which one to pick.
