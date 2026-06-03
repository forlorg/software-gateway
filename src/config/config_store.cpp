/**
 * @file config_store.cpp
 * @brief NVS（Preferences）读写：Wi-Fi 与 MQTT 凭据，缺键时避免无意义读与错误日志。
 */

#include "config_store.h"

#include <Preferences.h>

namespace gateway::config_store {

namespace {
constexpr char kNs[] = "gw_cfg";
Preferences prefs;
} // namespace

bool begin() { return prefs.begin(kNs, false); }

void factory_reset() { prefs.clear(); }

bool wifi_get(String &ssid, String &password) {
  if (!prefs.isKey("wifi_ssid")) {
    ssid = "";
    password = "";
    return false;
  }
  ssid = prefs.getString("wifi_ssid", "");
  password = prefs.isKey("wifi_password") ? prefs.getString("wifi_password", "") : String("");
  return ssid.length() > 0;
}

bool wifi_set(const String &ssid, const String &password) {
  /** ESP Preferences：putString 会写入 NVS，掉电后可由 `wifi_manager::start()` 读取并重连 STA。 */
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_password", password);
  return true;
}

bool mqtt_get(String &host, uint16_t &port, String &user, String &pass) {
  /** 缺键时避免 `getString` 走 NVS 读并刷 ERROR 日志（ESP32 Arduino Preferences）。 */
  if (!prefs.isKey("mqtt_host")) {
    host = "";
    port = 1883;
    user = "";
    pass = "";
    return false;
  }
  host = prefs.getString("mqtt_host", "");
  port = static_cast<uint16_t>(prefs.getUInt("mqtt_port", 1883));
  user = prefs.isKey("mqtt_username") ? prefs.getString("mqtt_username", "") : String("");
  pass = prefs.isKey("mqtt_password") ? prefs.getString("mqtt_password", "") : String("");
  return host.length() > 0;
}

bool mqtt_set(const String &host, uint16_t port, const String &user, const String &pass) {
  prefs.putString("mqtt_host", host);
  prefs.putUInt("mqtt_port", port);
  prefs.putString("mqtt_username", user);
  prefs.putString("mqtt_password", pass);
  return true;
}

} // namespace gateway::config_store
