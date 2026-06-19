#pragma once

/**
 * @file config_store.h
 * @brief 网关持久化配置命名空间与键读写声明。
 */

#include <Arduino.h>

namespace gateway::config_store {

    bool begin();
    void factory_reset();

    bool wifi_get(String &ssid, String &password);
    bool wifi_set(const String &ssid, const String &password);

    bool mqtt_get(String &host, uint16_t &port, String &user, String &pass);
    bool mqtt_set(const String &host, uint16_t port, const String &user, const String &pass);

} // namespace gateway::config_store
