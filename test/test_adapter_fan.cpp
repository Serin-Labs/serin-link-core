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

int main() {
  try {
    fan_modes_follow_climate_traits();
    std::puts("adapter fan: PASS: six climate trait combinations");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "adapter fan: FAIL: %s\n", error.what());
    return 1;
  }
}
