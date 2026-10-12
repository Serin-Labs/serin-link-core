/* Host tests: the screen: and night: switches apply their restore_mode at
 * boot, before the serin_link component starts fanning STATE out. */
#include "adapter/fixture.h"
#include <cstdio>

using namespace adapter_host;
using esphome::serin_link::NightSwitch;
using esphome::serin_link::ScreenSwitch;

// App.setup() runs components in descending setup priority; mirror that.
static sl2_hvac_state_t boot(ScreenSwitch *screen, NightSwitch *night) {
  reset();
  SerinLinkComponent component;
  if (screen != nullptr) {
    component.set_screen_switch(screen);
    require(screen->get_setup_priority() > component.get_setup_priority(),
            "screen switch must restore before serin_link starts");
    screen->setup();
  }
  if (night != nullptr) {
    component.set_night_switch(night);
    require(night->get_setup_priority() > component.get_setup_priority(),
            "night switch must restore before serin_link starts");
    night->setup();
  }
  component.setup();
  require(!component.failed, "adapter setup failed");
  sl2_hvac_state_t state{};
  require(component.hvac_get_state(&state), "STATE unavailable");
  return state;
}

static void stored_on_screen_keeps_links_awake() {
  ScreenSwitch screen;
  screen.initial_state = true;
  const sl2_hvac_state_t state = boot(&screen, nullptr);
  require(screen.emitted == std::vector<bool>{true}, "stored ON screen switch did not publish ON");
  require(state.screen_ctl && !state.screen_off, "stored ON screen switch sent SCREEN_OFF");
}

static void stored_off_screen_sleeps_links() {
  ScreenSwitch screen;
  screen.initial_state = false;
  const sl2_hvac_state_t state = boot(&screen, nullptr);
  require(screen.emitted == std::vector<bool>{false}, "stored OFF screen switch did not publish OFF");
  require(state.screen_ctl && state.screen_off, "stored OFF screen switch did not send SCREEN_OFF");
}

static void disabled_restore_leaves_screen_unpublished() {
  ScreenSwitch screen;
  boot(&screen, nullptr);
  require(screen.emitted.empty(), "RESTORE_DISABLED screen switch published a state");
}

static void stored_night_state_is_restored() {
  for (bool stored : {true, false}) {
    NightSwitch night;
    night.initial_state = stored;
    const sl2_hvac_state_t state = boot(nullptr, &night);
    require(night.emitted == std::vector<bool>{stored}, "night switch did not publish its stored state");
    require(state.night_ctl && state.night == stored, "STATE night bit does not match the stored switch");
  }
}

int main() {
  const struct { const char *name; void (*run)(); } tests[] = {
    {"stored ON screen", stored_on_screen_keeps_links_awake},
    {"stored OFF screen", stored_off_screen_sleeps_links},
    {"restore disabled screen", disabled_restore_leaves_screen_unpublished},
    {"stored night", stored_night_state_is_restored},
  };
  unsigned failed = 0;
  for (const auto &test : tests) {
    try { test.run(); std::printf("adapter switch restore: PASS: %s\n", test.name); }
    catch (const std::exception &error) {
      std::fprintf(stderr, "adapter switch restore: FAIL: %s: %s\n", test.name, error.what());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
