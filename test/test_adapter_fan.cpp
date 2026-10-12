#include "adapter/fixture.h"
#include <cstdio>

using namespace adapter_host;
using namespace esphome::climate;

struct FanAdapter : SerinLinkComponent {
  void flush() { apply_pending_(); }
};

static void fan_modes_follow_climate_traits() {
  const struct {
    std::vector<ClimateFanMode> supported;
    uint8_t steps;
    bool automatic;
  } cases[] = {
    {{CLIMATE_FAN_ON, CLIMATE_FAN_LOW, CLIMATE_FAN_HIGH}, 2, false},
    {{CLIMATE_FAN_AUTO, CLIMATE_FAN_LOW, CLIMATE_FAN_HIGH}, 2, true},
    {{CLIMATE_FAN_AUTO, CLIMATE_FAN_ON, CLIMATE_FAN_LOW, CLIMATE_FAN_HIGH}, 2, true},
    {{CLIMATE_FAN_LOW, CLIMATE_FAN_HIGH}, 2, false},
    {{CLIMATE_FAN_ON}, 0, false},
    {{}, 0, false},
  };
  for (const auto &test : cases) {
    reset();
    FanAdapter component;
    Climate climate;
    climate.traits.fan_modes = test.supported;
    component.set_climate(&climate);
    component.setup();
    require(!component.failed, "fan adapter setup failed");
    sl2_caps_pkt caps{};
    require(component.hvac_get_caps(&caps), "fan caps unavailable");
    require(caps.fan_steps == test.steps, "fan steps do not match controllable speed modes");
    require((caps.fan_flags & SL2_FAN_HAS_AUTO) == (test.automatic ? SL2_FAN_HAS_AUTO : 0),
            "Auto advertised without climate Auto support");

    sl2_cmd_pkt command{};
    for (uint8_t fan : {uint8_t{50}, uint8_t{100}, uint8_t{SL2_FAN_AUTO}}) {
      command.fan = fan;
      const auto before = climate.fan_mode;
      const bool accepted = test.steps != 0 && (fan != SL2_FAN_AUTO || test.automatic);
      require(component.hvac_apply(SL2_CM_FAN, &command) == accepted,
              "fan API accepted an unsupported command or rejected an advertised one");
      component.flush();
      if (accepted) {
        const auto expected = fan == SL2_FAN_AUTO ? CLIMATE_FAN_AUTO :
                              fan == 50 ? CLIMATE_FAN_LOW : CLIMATE_FAN_HIGH;
        require(climate.fan_mode == expected, "fan command was not accepted by climate traits");
        sl2_hvac_state_t state{};
        require(component.hvac_get_state(&state) && state.fan == fan,
                "accepted fan mode did not round-trip through STATE");
      } else {
        require(climate.fan_mode == before, "unsupported fan command changed climate state");
      }
      require(climate.rejected_fan_calls == 0, "adapter sent a mode rejected by ClimateCall validation");
    }
  }
}

/* cn105 maps CLIMATE_FAN_MEDIUM to Mitsubishi speed 2 and CLIMATE_FAN_MIDDLE
 * to speed 3, so a rising dial detent must walk QUIET..HIGH in that order. */
static void cn105_fan_detents_rise_monotonically() {
  reset();
  FanAdapter component;
  Climate climate;
  climate.traits.fan_modes = {CLIMATE_FAN_AUTO, CLIMATE_FAN_QUIET, CLIMATE_FAN_LOW,
                              CLIMATE_FAN_MEDIUM, CLIMATE_FAN_MIDDLE, CLIMATE_FAN_HIGH};
  component.set_climate(&climate);
  component.setup();
  require(!component.failed, "fan adapter setup failed");
  sl2_caps_pkt caps{};
  require(component.hvac_get_caps(&caps) && caps.fan_steps == 5,
          "cn105 fan set must expose five speed detents");

  const ClimateFanMode rising[] = {CLIMATE_FAN_QUIET, CLIMATE_FAN_LOW, CLIMATE_FAN_MEDIUM,
                                   CLIMATE_FAN_MIDDLE, CLIMATE_FAN_HIGH};
  sl2_cmd_pkt command{};
  for (size_t i = 0; i < 5; ++i) {
    command.fan = static_cast<uint8_t>((i + 1) * 20);
    require(component.hvac_apply(SL2_CM_FAN, &command), "fan detent rejected");
    component.flush();
    require(climate.fan_mode == rising[i], "rising dial detents did not map to rising cn105 speeds");
    sl2_hvac_state_t state{};
    require(component.hvac_get_state(&state) && state.fan == command.fan,
            "fan detent did not round-trip through STATE");
  }

  sl2_hvac_state_t medium{}, middle{};
  climate.fan_mode = CLIMATE_FAN_MEDIUM;
  require(component.hvac_get_state(&medium), "STATE unavailable");
  climate.fan_mode = CLIMATE_FAN_MIDDLE;
  require(component.hvac_get_state(&middle), "STATE unavailable");
  require(medium.fan < middle.fan, "STATE reports MEDIUM at or above MIDDLE");
}

int main() {
  try {
    fan_modes_follow_climate_traits();
    std::puts("adapter fan: PASS: six climate trait combinations");
    cn105_fan_detents_rise_monotonically();
    std::puts("adapter fan: PASS: cn105 MEDIUM below MIDDLE");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "adapter fan: FAIL: %s\n", error.what());
    return 1;
  }
}
