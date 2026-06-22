#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "ota/ota_types.h"

namespace gateway::ota::detail {

    class RoutedWiFiClient final : public WiFiClient {
    public:
        explicit RoutedWiFiClient(const IPAddress& transport_ip);

        using WiFiClient::connect;

        int connect(const char* host, uint16_t port) override;
        int connect(const char* host, uint16_t port, int32_t timeout_ms) override;

    private:
        IPAddress transport_ip_;
    };

    class RoutedWiFiClientSecure final : public WiFiClientSecure {
    public:
        RoutedWiFiClientSecure(const IPAddress& transport_ip, const char* root_ca);

        using WiFiClientSecure::connect;

        int connect(const char* host, uint16_t port) override;
        int connect(const char* host, uint16_t port, int32_t timeout_ms) override;

    private:
        IPAddress transport_ip_;
        const char* root_ca_;
    };

    bool begin_http(HTTPClient& http,
                    RoutedWiFiClient& plain,
                    RoutedWiFiClientSecure& secure,
                    const EndpointContext& endpoint,
                    const String& logical_url);

}  // namespace gateway::ota::detail
