"""serin_link — Serin Link controller for ESPHome.

Bind any `climate` entity via climate_id and a Serin Link can pair with this
node: CAPS derive from the entity's ClimateTraits, STATE from its published
state, and its commands route through a ClimateCall. Without climate_id the
component runs a canned device (the coexistence spike).

Owns the ESP-NOW radio (encrypted peers + broadcast pairing), so it is
mutually exclusive with ESPHome's built-in `espnow:` component, which neither
implements ESP-NOW link-layer encryption nor shares the recv callback.
"""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.core import HexInt
from esphome.components import binary_sensor, climate, select, sensor, text_sensor
from esphome.const import (
    CONF_HUMIDITY,
    CONF_ID,
    CONF_MAC_ADDRESS,
    CONF_TEMPERATURE,
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

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@Serin-Labs"]
DEPENDENCIES = ["wifi", "esp32"]
# climate core is always compiled in (the adapter references its types even
# when no entity is bound — spike mode); an actual entity is still optional.
# select/sensor/text_sensor are auto-loaded so the optional bindings always
# compile.
AUTO_LOAD = ["binary_sensor", "climate", "select", "sensor", "text_sensor"]

serin_link_ns = cg.esphome_ns.namespace("serin_link")
SerinLinkComponent = serin_link_ns.class_("SerinLinkComponent", cg.Component)

PrimaryLinkSelect = serin_link_ns.class_(
    "PrimaryLinkSelect", select.Select, cg.Parented.template(SerinLinkComponent)
)

PairStartAction = serin_link_ns.class_("PairStartAction", automation.Action)
PairCancelAction = serin_link_ns.class_("PairCancelAction", automation.Action)
ForgetDialAction = serin_link_ns.class_("ForgetDialAction", automation.Action)
ForgetAllDialsAction = serin_link_ns.class_("ForgetAllDialsAction", automation.Action)

CONF_ZONE_NAME = "zone_name"
CONF_CLIMATE_ID = "climate_id"
CONF_HVAC_LINK = "hvac_link"
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


def primary_select_options(slots):
    return [PRIMARY_AUTO_LABEL] + [f"Serin Link {i + 1}" for i in range(slots)]

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
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_HUMIDITY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Which Serin Link is currently feeding. Diagnostic: it only matters
        # when two Links bonded to one controller both offer a sensor, where
        # the value alternates between them (last reporting Link wins).
        cv.Optional(CONF_LINK_MAC): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # primary_link: — which bonded Serin Link feeds the entities above.
        # Unset (the default) keeps the historical behavior: whichever Link
        # reported last owns them, which with two Links in two rooms makes the
        # entity — and any heat pump fed from it — alternate between rooms.
        #
        # Set, only this Link's readings are used. Other Links are NOT
        # rejected: their room-source EDITS are still honored, because a Link
        # re-sends an unacknowledged edit at ~3 Hz forever (wire spec §10d has
        # no give-up rule). Arbitration decides whose reading is used, never
        # whether an edit is accepted.
        #
        # Get the MAC from diagnostics: links: [].mac_address, or the bond
        # table dump_config() prints at boot.
        cv.Optional(CONF_PRIMARY_LINK): cv.mac_address,
        # primary_select: — the same pin, chosen at runtime from a Home
        # Assistant dropdown instead of baked into the config. Mutually
        # exclusive with primary_link: (below), so there is never a precedence
        # question between a compile-time pin and a stored one. The chosen
        # Serin Link is persisted by MAC, so it survives a reboot and follows
        # its Link across the slot shuffle a forget causes.
        cv.Optional(CONF_PRIMARY_SELECT): select.select_schema(
            PrimaryLinkSelect,
            entity_category=ENTITY_CATEGORY_CONFIG,
        ).extend(
            {
                # How many Serin Links this install can hold, i.e. how many
                # entries the dropdown offers besides "Auto". Defaults to the
                # number of `links:` rows under diagnostics:, or all
                # SL2_MAX_DIALS slots when there is no diagnostics block.
                cv.Optional(CONF_SLOTS): cv.int_range(min=1, max=SL2_MAX_DIALS),
            }
        ),
        # 90s = three missed 20s Serin Link keepalives plus slack.
        cv.Optional(
            CONF_STALE_AFTER, default="90s"
        ): cv.positive_time_period_milliseconds,
    }
)


def _link_sensor_schema(value):
    # `link_sensor:` with no sub-keys parses as None, not {} — map it before
    # the dict schema runs so the bare form (every child optional, meaning
    # "accept the Serin Link as a room source, create no HA entities")
    # validates the same as the explicit `link_sensor: {}`.
    if value is None:
        value = {}
    # dial_mac: shipped in v0.1.3-beta.3 and was renamed to link_mac: when the
    # component's user-facing vocabulary settled on "Serin Link". Accepted for
    # one release with a warning; cv.rename_key alone would do it silently.
    if isinstance(value, dict) and CONF_DIAL_MAC in value:
        if CONF_LINK_MAC in value:
            raise cv.Invalid(
                f"set either `{CONF_LINK_MAC}:` or the deprecated "
                f"`{CONF_DIAL_MAC}:`, not both"
            )
        _LOGGER.warning(
            "serin_link: `%s:` is deprecated, rename it to `%s:` "
            "(the component now calls the device a Serin Link, not a dial). "
            "The old key still works in this release.",
            CONF_DIAL_MAC,
            CONF_LINK_MAC,
        )
        value = cv.rename_key(CONF_DIAL_MAC, CONF_LINK_MAC)(value)
    # Static pin or runtime pin, never both: allowing both would mean deciding
    # whether the config or the stored selection wins, and whichever answer we
    # picked would surprise the other half of the users.
    if isinstance(value, dict) and CONF_PRIMARY_LINK in value and CONF_PRIMARY_SELECT in value:
        raise cv.Invalid(
            f"`{CONF_PRIMARY_LINK}:` and `{CONF_PRIMARY_SELECT}:` do the same "
            f"job and cannot both be set — `{CONF_PRIMARY_LINK}:` pins one "
            f"Serin Link at compile time, `{CONF_PRIMARY_SELECT}:` exposes the "
            f"choice as a Home Assistant dropdown that persists across reboots."
        )
    return LINK_SENSOR_SCHEMA(value)


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


DIAGNOSTICS_SCHEMA = cv.Schema(
    {
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
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SerinLinkComponent),
        cv.Optional(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
        cv.Optional(CONF_ZONE_NAME, default=""): cv.string,
        # Device-link health for the STATE hvac_link flag (drives the Link's
        # offline face). A generic climate entity exists whether or not the
        # device behind it answers, so bind the platform's own signal here,
        # e.g. cn105: `hvac_link: !lambda 'return id(hvac).isHeatpumpConnected();'`
        # Unset: NaN room temp on an entity that claims one = link down.
        cv.Optional(CONF_HVAC_LINK): cv.returning_lambda,
        # Vane axes for platforms that expose vanes as select entities (e.g.
        # cn105's vertical_vane_select / horizontal_vane_select): the option
        # list defines the wire positions IN ORDER; options named "auto" or
        # "swing" (case-insensitive) become the wire AUTO/SWING codes.
        cv.Optional(CONF_VANE_V_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_VANE_H_SELECT): cv.use_id(select.Select),
        # Trailing quiet window before a burst of Serin Link edits is applied to the
        # climate entity as a single ClimateCall (0s = apply each CMD
        # immediately). The STATE echoed to Links reflects the commanded
        # values either way (optimistic overlay until the entity confirms).
        cv.Optional(
            CONF_CMD_DEBOUNCE, default="300ms"
        ): cv.positive_time_period_milliseconds,
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
        # also drives the STATE low-battery flag with +5% clear hysteresis,
        # hence the 95% ceiling on the threshold.
        cv.Optional(CONF_BATTERY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_BATTERY_LOW_THRESHOLD, default="10%"): cv.All(
            cv.percentage, cv.Range(min=0.0, max=0.95)
        ),
        cv.Optional(CONF_RUNTIME_SENSOR): cv.use_id(sensor.Sensor),
        # ENERGY TLV: power_sensor in W, energy_sensor in kWh (sent as Wh);
        # either alone is fine — the other half rides as n/a.
        cv.Optional(CONF_POWER_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ENERGY_SENSOR): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_LINK_SENSOR): _link_sensor_schema,
        cv.Optional(CONF_DIAGNOSTICS): _diagnostics_schema,
    }
).extend(cv.COMPONENT_SCHEMA)


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
    cg.add(
        var.set_battery_low_threshold(
            int(round(config[CONF_BATTERY_LOW_THRESHOLD] * 100))
        )
    )
    cg.add(var.set_cmd_debounce(config[CONF_CMD_DEBOUNCE].total_milliseconds))
    if CONF_LINK_SENSOR in config:
        ls = config[CONF_LINK_SENSOR]
        cg.add(var.set_link_sensor_enabled())
        cg.add(var.set_dial_stale_after(ls[CONF_STALE_AFTER].total_milliseconds))
        if CONF_PRIMARY_SELECT in ls:
            ps = ls[CONF_PRIMARY_SELECT]
            slots = ps.get(CONF_SLOTS)
            if slots is None:
                rows = (config.get(CONF_DIAGNOSTICS) or {}).get(CONF_LINKS) or []
                slots = len(rows) or SL2_MAX_DIALS
            sel = await select.new_select(ps, options=primary_select_options(slots))
            await cg.register_parented(sel, var)
            cg.add(var.set_primary_select(sel))
        if CONF_PRIMARY_LINK in ls:
            # list of HexInt -> braced initializer -> const std::array<uint8_t,6>&
            cg.add(
                var.set_primary_dial([HexInt(i) for i in ls[CONF_PRIMARY_LINK].parts])
            )
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
    if CONF_DIAGNOSTICS in config:
        diag = config[CONF_DIAGNOSTICS]
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
