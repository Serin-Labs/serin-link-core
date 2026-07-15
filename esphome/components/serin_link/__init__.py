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
from esphome.components import climate, select
from esphome.components.esp32 import add_idf_component
from esphome.const import CONF_ID

CODEOWNERS = ["@Serin-Labs"]
DEPENDENCIES = ["wifi", "esp32"]
# climate core is always compiled in (the adapter references its types even
# when no entity is bound — spike mode); an actual entity is still optional.
# select is auto-loaded so the optional vane bindings always compile
AUTO_LOAD = ["climate", "select"]

serin_link_ns = cg.esphome_ns.namespace("serin_link")
SerinLinkComponent = serin_link_ns.class_("SerinLinkComponent", cg.Component)

CONF_ZONE_NAME = "zone_name"
CONF_CLIMATE_ID = "climate_id"
CONF_HVAC_LINK = "hvac_link"
CONF_VANE_V_SELECT = "vane_v_select"
CONF_VANE_H_SELECT = "vane_h_select"

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
    # Ed25519 + X25519 + HMAC-SHA256 all come from libsodium (mbedTLS has no
    # EdDSA). Registry component; pin per ESPHome/IDF release as needed.
    add_idf_component(name="espressif/libsodium", ref="1.0.20~4")
