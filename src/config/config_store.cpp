/**
 * @file config_store.cpp
 * @brief NVS（Preferences）读写：Wi-Fi、MQTT 凭据与 OTA 域名 IPv4 缓存。
 */

#include "config_store.h"

#include <Preferences.h>

namespace gateway::config_store {

    namespace {
        constexpr char kNs[] = "gw_cfg";

        // Preferences 的 key 最长 15 字符，以下键均保持短名。
        constexpr char kOtaCacheVersionKey[] = "ota_cv";
        constexpr char kOtaSchemeKey[] = "ota_sch";
        constexpr char kOtaHostKey[] = "ota_host";
        constexpr char kOtaPortKey[] = "ota_port";
        constexpr char kOtaIpv4Key[] = "ota_ip";

        Preferences prefs;

        void remove_ota_domain_cache_keys() {
            prefs.remove(kOtaCacheVersionKey);
            prefs.remove(kOtaSchemeKey);
            prefs.remove(kOtaHostKey);
            prefs.remove(kOtaPortKey);
            prefs.remove(kOtaIpv4Key);
        }
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

    bool ota_domain_cache_get(OtaDomainCache &cache) {
        cache = OtaDomainCache{};

        if (!prefs.isKey(kOtaCacheVersionKey) ||
            !prefs.isKey(kOtaSchemeKey) ||
            !prefs.isKey(kOtaHostKey) ||
            !prefs.isKey(kOtaPortKey) ||
            !prefs.isKey(kOtaIpv4Key)) {
            // 清理断电或旧版本遗留的半写入记录；无缓存时 remove 是无害操作。
            remove_ota_domain_cache_keys();
            return false;
        }

        cache.version = prefs.getUInt(kOtaCacheVersionKey, 0);
        cache.scheme = prefs.getString(kOtaSchemeKey, "");
        cache.host = prefs.getString(kOtaHostKey, "");
        cache.port = static_cast<uint16_t>(prefs.getUInt(kOtaPortKey, 0));
        cache.ipv4 = prefs.getString(kOtaIpv4Key, "");

        return cache.version != 0 &&
               cache.scheme.length() > 0 &&
               cache.host.length() > 0 &&
               cache.port != 0 &&
               cache.ipv4.length() > 0;
    }

    bool ota_domain_cache_set(const OtaDomainCache &cache) {
        if (cache.version == 0 ||
            cache.scheme.length() == 0 ||
            cache.host.length() == 0 ||
            cache.port == 0 ||
            cache.ipv4.length() == 0) {
            return false;
        }

        // 版本号是提交标记：先置 0，其他字段全部成功后最后写入真实版本号。
        remove_ota_domain_cache_keys();
        prefs.putUInt(kOtaCacheVersionKey, 0);

        const bool scheme_ok = prefs.putString(kOtaSchemeKey, cache.scheme) > 0;
        const bool host_ok = prefs.putString(kOtaHostKey, cache.host) > 0;
        const bool port_ok = prefs.putUInt(kOtaPortKey, cache.port) > 0;
        const bool ip_ok = prefs.putString(kOtaIpv4Key, cache.ipv4) > 0;
        const bool version_ok = scheme_ok && host_ok && port_ok && ip_ok &&
                                prefs.putUInt(kOtaCacheVersionKey, cache.version) > 0;

        if (!version_ok) {
            remove_ota_domain_cache_keys();
        }
        return version_ok;
    }

    void ota_domain_cache_clear() {
        remove_ota_domain_cache_keys();
    }

} // namespace gateway::config_store
