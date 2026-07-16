"""serin_link — Serin Link controller for ESPHome.

Bind any `climate` entity via climate_id and a Serin dial can pair with this
node: CAPS derive from the entity's ClimateTraits, STATE from its published
state, and dial commands route through a ClimateCall. Without climate_id the
component runs a canned device (the coexistence spike).

Owns the ESP-NOW radio (encrypted peers + broadcast pairing), so it is
mutually exclusive with ESPHome's built-in `espnow:` component, which neither
implements ESP-NOW link-layer encryption nor shares the recv callback.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import climate, select, sensor, text_sensor
from esphome.components.esp32 import add_idf_component
from esphome.const import CONF_ID

CODEOWNERS = ["@Serin-Labs"]
DEPENDENCIES = ["wifi", "esp32"]
# climate core is always compiled in (the adapter references its types even
# when no entity is bound — spike mode); an actual entity is still optional.
# select/sensor/text_sensor are auto-loaded so the optional bindings always
# compile.
AUTO_LOAD = ["climate", "select", "sensor", "text_sensor"]

serin_link_ns = cg.esphome_ns.namespace("serin_link")
SerinLinkComponent = serin_link_ns.class_("SerinLinkComponent", cg.Component)

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

# esp-idf framework required (raw nvs_*, esp_now encrypted peers, libsodium);
# it is ESPHome's ESP32 default. (cv.only_with_esp_idf was removed in 2026.x.)
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SerinLinkComponent),
        cv.Optional(CONF_CLIMATE_ID): cv.use_id(climate.Climate),
        cv.Optional(CONF_ZONE_NAME, default=""): cv.string,
        # Device-link health for the STATE hvac_link flag (drives the dial's
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
        # Trailing quiet window before a burst of dial edits is applied to the
        # climate entity as a single ClimateCall (0s = apply each CMD
        # immediately). The STATE echoed to dials reflects the commanded
        # values either way (optimistic overlay until the entity confirms).
        cv.Optional(
            CONF_CMD_DEBOUNCE, default="300ms"
        ): cv.positive_time_period_milliseconds,
        # Telemetry bindings -> INFO TLVs + CAPS feature bits (dial telemetry
        # pages). All optional; unbound = TLV omitted, feature bit unset, the
        # dial hides the row. Any platform's entities work — for echavet's
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
    # Ed25519 + X25519 + HMAC-SHA256 all come from libsodium (mbedTLS has no
    # EdDSA). Caret range, not an exact pin: arduino-on-IDF builds carry
    # arduino-esp32's own `espressif/libsodium ^1.0.21` dependency, and an
    # exact pin here makes IDF version solving fail for the whole project.
    # setup() registers an esp_random-backed RNG before sodium_init(), so any
    # 1.x resolution is safe.
    add_idf_component(name="espressif/libsodium", ref="^1.0.20")
