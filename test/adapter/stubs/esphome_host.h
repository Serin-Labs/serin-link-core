#pragma once
// Minimal host stand-ins for ESPHome and ESP-IDF boundaries. Adapter and core
// behavior comes from their production translation units, never copied here.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace adapter_host {
// Model platform boundaries; protocol and adapter behavior stays in the
// production translation units. Scheduling and unrelated climate fields are no-ops.
inline uint32_t now_ms = 1000;
inline bool fail_storage = false;
inline std::map<std::string, std::vector<uint8_t>> nvs;
inline std::map<uint32_t, std::vector<uint8_t>> preferences;
void reset_radio();
inline std::vector<std::string> logs;
inline bool wifi_connected = false;
inline bool api_connected = true;   // a native API client (Home Assistant) is connected
inline void reset() {
  reset_radio();
  logs.clear();
  wifi_connected = false;
  api_connected = true;
  now_ms = 1000;
  fail_storage = false;
  nvs.clear();
  preferences.clear();
}
template<typename... Args> void log(const char *, const char *format, Args... args) {
  if constexpr (sizeof...(Args) == 0) {
    logs.emplace_back(format);
  } else {
    char line[512];
    std::snprintf(line, sizeof(line), format, args...);
    logs.emplace_back(line);
  }
}
}
#define ESP_LOGI(...) adapter_host::log(__VA_ARGS__)
#define ESP_LOGW(...) adapter_host::log(__VA_ARGS__)
#define ESP_LOGV(...) adapter_host::log(__VA_ARGS__)
#define ESP_LOGE(...) adapter_host::log(__VA_ARGS__)
#define ESP_LOGD(...) adapter_host::log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) adapter_host::log(__VA_ARGS__)
#define ESPHOME_VERSION "host-test"

namespace esphome {
inline uint32_t millis() { return adapter_host::now_ms; }
namespace setup_priority { constexpr float DATA = 600; constexpr float LATE = -100; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  void set_timeout(const std::string &, uint32_t, std::function<void()>) {}
  void mark_failed() { failed = true; }
  void status_set_warning() { warning = true; }
  void status_clear_warning() { warning = false; }
  bool warning{false};
  bool failed{false};
};
struct ESPPreferenceObject {
  uint32_t key{0};
  template<typename T> bool save(const T *v) {
    if (adapter_host::fail_storage) return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(v);
    adapter_host::preferences[key] = {bytes, bytes + sizeof(T)};
    return true;
  }
  template<typename T> bool load(T *v) {
    auto it = adapter_host::preferences.find(key);
    if (it == adapter_host::preferences.end() || it->second.size() != sizeof(T)) return false;
    std::memcpy(v, it->second.data(), sizeof(T));
    return true;
  }
};
struct Preferences {
  template<typename T> ESPPreferenceObject make_preference(uint32_t key) { return {key}; }
};
inline Preferences host_preferences;
inline Preferences *global_preferences = &host_preferences;
template<typename T> class Trigger {
 public:
  std::vector<T> emitted;
  void trigger(T v) { emitted.push_back(v); }
};
template<typename... Ts> class Action { public: virtual void play(Ts...) = 0; };
template<typename T> class Parented {
 public: void set_parent(T *v) { parent_ = v; }
 protected: T *parent_{nullptr};
};
template<typename T> struct TemplateValue {
  template<typename... Ts> T value(Ts...) { return T{}; }
};
#define TEMPLATABLE_VALUE(type, name) TemplateValue<type> name##_;
namespace sensor {
class Sensor {
 public:
  float state{NAN};
  std::vector<float> emitted;
  std::vector<std::function<void(float)>> callbacks;
  bool has_state() const { return !emitted.empty(); }
  void publish_state(float v) {
    state = v;
    emitted.push_back(v);
    for (auto &cb : callbacks) cb(v);
  }
  void add_on_state_callback(std::function<void(float)> cb) { callbacks.push_back(std::move(cb)); }
};
}
namespace text_sensor {
class TextSensor {
 public:
  std::string state;
  std::vector<std::string> emitted;
  bool has_state() const { return !emitted.empty(); }
  void publish_state(const std::string &v) { state = v; emitted.push_back(v); }
};
}
namespace binary_sensor {
class BinarySensor {
 public:
  bool state{false};
  std::vector<bool> emitted;
  void publish_state(bool v) { state = v; emitted.push_back(v); }
};
}
namespace select {
struct SelectTraits {
  std::vector<std::string> options;
  const std::vector<std::string> &get_options() const { return options; }
};
struct Option { std::string value; std::string str() const { return value; } };
class Select {
 public:
  virtual ~Select() = default;
  SelectTraits traits;
  size_t index{0};
  std::vector<size_t> emitted;
  void publish_state(size_t v) { index = v; emitted.push_back(v); }
  Option current_option() const { return {index < traits.options.size() ? traits.options[index] : ""}; }
  struct Call { void set_option(const std::string &) {} void perform() {} };
  Call make_call() { return {}; }
 protected: virtual void control(size_t) {}
};
}
namespace switch_ {
class Switch {
 public:
  bool state{false};
  std::vector<bool> emitted;
  // What ESPHome's get_initial_state_with_restore_mode() would return: the
  // stored value or the restore_mode default; nullopt = RESTORE_DISABLED.
  std::optional<bool> initial_state;
  virtual ~Switch() = default;
  void publish_state(bool v) { state = v; emitted.push_back(v); }
  void turn_on() { write_state(true); }
  void turn_off() { write_state(false); }
  std::optional<bool> get_initial_state_with_restore_mode() { return initial_state; }
 protected: virtual void write_state(bool) {}
};
}
namespace button {
class Button {
 public: virtual ~Button() = default;
 protected: virtual void press_action() {}
};
}
namespace climate {
enum ClimateMode { CLIMATE_MODE_OFF, CLIMATE_MODE_HEAT, CLIMATE_MODE_COOL,
  CLIMATE_MODE_HEAT_COOL, CLIMATE_MODE_AUTO, CLIMATE_MODE_DRY, CLIMATE_MODE_FAN_ONLY };
enum ClimateAction { CLIMATE_ACTION_OFF, CLIMATE_ACTION_IDLE, CLIMATE_ACTION_COOLING,
  CLIMATE_ACTION_HEATING, CLIMATE_ACTION_DRYING, CLIMATE_ACTION_FAN };
enum ClimateFanMode { CLIMATE_FAN_AUTO, CLIMATE_FAN_ON, CLIMATE_FAN_QUIET,
  CLIMATE_FAN_LOW, CLIMATE_FAN_MIDDLE, CLIMATE_FAN_MEDIUM, CLIMATE_FAN_FOCUS,
  CLIMATE_FAN_DIFFUSE, CLIMATE_FAN_HIGH };
enum ClimatePreset { CLIMATE_PRESET_NONE, CLIMATE_PRESET_HOME, CLIMATE_PRESET_AWAY,
  CLIMATE_PRESET_BOOST, CLIMATE_PRESET_COMFORT, CLIMATE_PRESET_ECO,
  CLIMATE_PRESET_SLEEP, CLIMATE_PRESET_ACTIVITY };
enum ClimateFeature { CLIMATE_SUPPORTS_CURRENT_TEMPERATURE = 1,
  CLIMATE_SUPPORTS_ACTION = 2, CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE = 4,
  CLIMATE_SUPPORTS_CURRENT_HUMIDITY = 8, CLIMATE_SUPPORTS_TARGET_HUMIDITY = 16 };
struct ClimateTraits {
  std::vector<ClimateFanMode> fan_modes{CLIMATE_FAN_AUTO, CLIMATE_FAN_ON,
    CLIMATE_FAN_QUIET, CLIMATE_FAN_LOW, CLIMATE_FAN_MIDDLE, CLIMATE_FAN_MEDIUM,
    CLIMATE_FAN_FOCUS, CLIMATE_FAN_DIFFUSE, CLIMATE_FAN_HIGH};
  bool supports_fan_mode(ClimateFanMode mode) const {
    return std::find(fan_modes.begin(), fan_modes.end(), mode) != fan_modes.end();
  }
  bool supports_mode(ClimateMode) const { return true; }
  bool supports_preset(ClimatePreset) const { return true; }
  bool has_feature_flags(ClimateFeature) const { return false; }
  float get_visual_min_temperature() const { return 16; }
  float get_visual_max_temperature() const { return 30; }
  float get_visual_target_temperature_step() const { return 0.5f; }
};
class Climate;
class ClimateCall {
 public:
  explicit ClimateCall(Climate *parent) : parent_(parent) {}
  void set_mode(ClimateMode) {}
  void set_target_temperature(float) {}
  void set_target_temperature_low(float) {}
  void set_target_temperature_high(float) {}
  void set_target_humidity(float) {}
  void set_fan_mode(ClimateFanMode mode) { fan_mode_ = mode; }
  void set_preset(ClimatePreset) {}
  void perform();
 private:
  Climate *parent_;
  std::optional<ClimateFanMode> fan_mode_;
};
class Climate {
 public:
  ClimateMode mode{CLIMATE_MODE_OFF};
  ClimateAction action{CLIMATE_ACTION_IDLE};
  float current_temperature{NAN}, target_temperature{NAN};
  float target_temperature_low{NAN}, target_temperature_high{NAN};
  float current_humidity{NAN}, target_humidity{NAN};
  std::optional<ClimateFanMode> fan_mode;
  std::optional<ClimatePreset> preset;
  ClimateTraits traits;
  unsigned rejected_fan_calls{0};
  ClimateTraits get_traits() const { return traits; }
  ClimateCall make_call() { return ClimateCall(this); }
  std::string get_name() const { return "host climate"; }
};
inline void ClimateCall::perform() {
  if (!fan_mode_) return;
  // ESPHome ClimateCall::validate_ drops fan modes absent from the traits.
  // A test must observe accepted state, not merely a captured set_fan_mode().
  if (parent_->get_traits().supports_fan_mode(*fan_mode_)) parent_->fan_mode = fan_mode_;
  else ++parent_->rejected_fan_calls;
}
}
namespace network {
constexpr size_t IP_ADDRESS_BUFFER_SIZE = 40;
inline bool is_connected() { return adapter_host::wifi_connected; }
struct IPAddress {
  bool is_set() const { return false; }
  void str_to(char *out) const { out[0] = '\0'; }
};
}
namespace api {
class APIServer {
 public: bool is_connected() const { return adapter_host::api_connected; }
};
inline APIServer host_api_server;
inline APIServer *global_api_server = &host_api_server;
}
namespace wifi {
constexpr size_t SSID_BUFFER_SIZE = 33;
struct WiFiComponent {
  bool is_connected() const { return adapter_host::wifi_connected; }
  std::vector<network::IPAddress> wifi_sta_ip_addresses() { return {}; }
  int wifi_rssi() const { return -50; }
  const char *wifi_ssid_to(char *out) { out[0] = '\0'; return out; }
};
inline WiFiComponent host_wifi_component;
inline WiFiComponent *global_wifi_component = &host_wifi_component;
}
struct Application {
  static constexpr size_t BUILD_TIME_STR_SIZE = 32;
  void get_build_time_string(char *out) const { std::strcpy(out, "host"); }
  std::string get_friendly_name() const { return "host"; }
};
inline Application App;
}

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_ESPNOW_EXIST = 1;
inline const char *esp_err_to_name(esp_err_t) { return "host error"; }
constexpr int WIFI_IF_STA = 0;
using wifi_second_chan_t = int;
using wifi_ps_type_t = int;
constexpr wifi_ps_type_t WIFI_PS_NONE = 0;
struct wifi_config_t { struct { uint8_t ssid[32]; uint8_t password[64]; } sta; };
struct esp_now_recv_info_t { const uint8_t *src_addr; const uint8_t *des_addr; };
struct esp_now_peer_info_t { uint8_t peer_addr[6]; int ifidx; uint8_t channel; bool encrypt; uint8_t lmk[16]; };
struct esp_now_peer_num_t { int total_num; int encrypt_num; };

constexpr esp_err_t ESP_ERR_ESPNOW_NOT_INIT = 2;
constexpr esp_err_t ESP_ERR_ESPNOW_NOT_FOUND = 3;
using esp_event_base_t = const char *;
using esp_event_handler_instance_t = void *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
inline constexpr const char *WIFI_EVENT = "wifi";
constexpr int ESP_EVENT_ANY_ID = -1;
constexpr int WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_STOP = 2;
constexpr int WIFI_EVENT_STA_CONNECTED = 3, WIFI_EVENT_STA_DISCONNECTED = 4;
struct wifi_event_sta_disconnected_t { uint8_t reason; };
namespace adapter_host {
using Mac = std::array<uint8_t, 6>;
using ReceiveCallback = void (*)(const esp_now_recv_info_t *, const uint8_t *, int);
inline std::map<Mac, esp_now_peer_info_t> peers;
inline ReceiveCallback receive_callback = nullptr;
inline esp_event_handler_t wifi_event_callback = nullptr;
inline void *wifi_event_context = nullptr;
inline bool radio_initialized = false;
inline int fail_init = 0, fail_pmk = 0, fail_callback = 0, fail_peer = 0, fail_ps = 0, fail_event = 0;
inline int init_calls = 0, pmk_calls = 0, callback_calls = 0, peer_calls = 0, sends = 0, event_calls = 0;
inline int wifi_channel = 6, power_save = 0;
inline Mac mac_key(const uint8_t *p) { Mac m; std::copy(p, p+6, m.begin()); return m; }
inline bool fail_next(int &count) { if (!count) return false; --count; return true; }
inline void reset_radio() {
  peers.clear(); receive_callback = nullptr; wifi_event_callback = nullptr; wifi_event_context = nullptr;
  radio_initialized = false;
  fail_init = fail_pmk = fail_callback = fail_peer = fail_ps = fail_event = 0;
  init_calls = pmk_calls = callback_calls = peer_calls = sends = event_calls = 0;
  wifi_channel = 6; power_save = 0;
}
inline void wifi_event(int event, uint8_t reason = 0) {
  if (event == WIFI_EVENT_STA_CONNECTED) wifi_connected = true;
  if (event == WIFI_EVENT_STA_DISCONNECTED || event == WIFI_EVENT_STA_STOP) wifi_connected = false;
  wifi_event_sta_disconnected_t data{reason};
  if (wifi_event_callback) wifi_event_callback(wifi_event_context, WIFI_EVENT, event, &data);
}
inline bool deliver(const uint8_t *src, const uint8_t *dst, const void *data, size_t len) {
  auto peer = peers.find(mac_key(src));
  if (!radio_initialized || !receive_callback || power_save || peer == peers.end() || !peer->second.encrypt) return false;
  esp_now_recv_info_t info{src,dst};
  receive_callback(&info, static_cast<const uint8_t *>(data), static_cast<int>(len));
  return true;
}
}
inline esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, esp_event_handler_t cb, void *ctx, esp_event_handler_instance_t *instance) {
  ++adapter_host::event_calls;
  if (adapter_host::fail_next(adapter_host::fail_event)) return ESP_FAIL;
  adapter_host::wifi_event_callback = cb; adapter_host::wifi_event_context = ctx; *instance = ctx; return ESP_OK;
}
inline esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *, size_t) {
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  if (!adapter_host::peers.count(adapter_host::mac_key(mac))) return ESP_ERR_ESPNOW_NOT_FOUND;
  ++adapter_host::sends; return ESP_OK;
}
inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) {
  ++adapter_host::peer_calls;
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  if (adapter_host::fail_next(adapter_host::fail_peer)) return ESP_FAIL;
  auto key=adapter_host::mac_key(peer->peer_addr);
  if (adapter_host::peers.count(key)) return ESP_ERR_ESPNOW_EXIST;
  adapter_host::peers[key]=*peer; return ESP_OK;
}
inline esp_err_t esp_now_mod_peer(const esp_now_peer_info_t *peer) {
  if (adapter_host::fail_next(adapter_host::fail_peer)) return ESP_FAIL;
  adapter_host::peers[adapter_host::mac_key(peer->peer_addr)]=*peer; return ESP_OK;
}
inline esp_err_t esp_now_get_peer(const uint8_t *mac, esp_now_peer_info_t *out) {
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  auto it=adapter_host::peers.find(adapter_host::mac_key(mac));
  if (it==adapter_host::peers.end()) return ESP_ERR_ESPNOW_NOT_FOUND;
  *out=it->second; return ESP_OK;
}
inline esp_err_t esp_now_del_peer(const uint8_t *mac) { adapter_host::peers.erase(adapter_host::mac_key(mac)); return ESP_OK; }
inline esp_err_t esp_now_init() {
  ++adapter_host::init_calls;
  if (adapter_host::fail_next(adapter_host::fail_init)) return ESP_FAIL;
  adapter_host::radio_initialized=true; return ESP_OK;
}
inline esp_err_t esp_now_get_peer_num(esp_now_peer_num_t *num) {
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  num->total_num = static_cast<int>(adapter_host::peers.size());
  num->encrypt_num = 0;
  for (const auto &peer : adapter_host::peers) if (peer.second.encrypt) ++num->encrypt_num;
  return ESP_OK;
}
inline esp_err_t esp_now_set_pmk(const uint8_t *) {
  ++adapter_host::pmk_calls;
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  return adapter_host::fail_next(adapter_host::fail_pmk) ? ESP_FAIL : ESP_OK;
}
inline esp_err_t esp_now_register_recv_cb(adapter_host::ReceiveCallback cb) {
  ++adapter_host::callback_calls;
  if (!adapter_host::radio_initialized) return ESP_ERR_ESPNOW_NOT_INIT;
  if (adapter_host::fail_next(adapter_host::fail_callback)) return ESP_FAIL;
  adapter_host::receive_callback=cb; return ESP_OK;
}
inline esp_err_t esp_wifi_get_mac(int, uint8_t *out) { const uint8_t mac[6] = {2, 0, 0, 0, 0, 0xFF}; std::memcpy(out, mac, 6); return ESP_OK; }
inline esp_err_t esp_wifi_get_channel(uint8_t *channel, wifi_second_chan_t *secondary) { *channel=adapter_host::wifi_channel; *secondary=0; return ESP_OK; }
inline esp_err_t esp_wifi_get_ps(wifi_ps_type_t *ps) { *ps=adapter_host::power_save; return ESP_OK; }
inline esp_err_t esp_wifi_set_ps(wifi_ps_type_t ps) {
  if (adapter_host::fail_next(adapter_host::fail_ps)) return ESP_FAIL;
  adapter_host::power_save=ps; return ESP_OK;
}
inline esp_err_t esp_wifi_get_config(int, wifi_config_t *config) { std::memset(config, 0, sizeof(*config)); return ESP_OK; }
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(adapter_host::now_ms) * 1000; }
inline int esp_reset_reason() { return 0; }
inline void esp_fill_random(void *out, size_t len) { std::memset(out, 0x42, len); }
using nvs_handle_t = int;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
inline esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) { *handle = 1; return ESP_OK; }
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *out, size_t *len) {
  auto it = adapter_host::nvs.find(key);
  if (it == adapter_host::nvs.end() || *len < it->second.size()) return ESP_FAIL;
  *len = it->second.size();
  std::memcpy(out, it->second.data(), *len);
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *in, size_t len) {
  if (adapter_host::fail_storage) return ESP_FAIL;
  const auto *bytes = static_cast<const uint8_t *>(in);
  adapter_host::nvs[key] = {bytes, bytes + len};
  return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) { return adapter_host::fail_storage ? ESP_FAIL : ESP_OK; }
