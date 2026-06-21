#pragma once

/**
 * @file config_store.h
 * @brief 网关持久化配置命名空间与键读写声明。
 */

#include <Arduino.h>

namespace gateway::config_store {

    struct OtaDomainCache {
        uint32_t version = 0;
        String scheme;
        String host;
        uint16_t port = 0;
        String ipv4;
    };

    bool begin();
    void factory_reset();

    bool wifi_get(String &ssid, String &password);
    bool wifi_set(const String &ssid, const String &password);

    bool mqtt_get(String &host, uint16_t &port, String &user, String &pass);
    bool mqtt_set(const String &host, uint16_t port, const String &user, const String &pass);

    bool ota_domain_cache_get(OtaDomainCache &cache);
    bool ota_domain_cache_set(const OtaDomainCache &cache);
    void ota_domain_cache_clear();

} // namespace gateway::config_store
