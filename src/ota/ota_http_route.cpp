#include "ota/ota_http_route.h"

#include <Arduino.h>

#include "config/ota_config.h"
#include "ota/ota_endpoint_planner.h"

namespace gateway::ota::detail {
    namespace {
        static constexpr const char* kTag = "OTA";
    }

    RoutedWiFiClient::RoutedWiFiClient(const IPAddress& transport_ip)
        : transport_ip_(transport_ip) {}

    int RoutedWiFiClient::connect(const char*, uint16_t port) {
        return WiFiClient::connect(transport_ip_, port);
    }

    int RoutedWiFiClient::connect(const char*, uint16_t port, int32_t timeout_ms) {
        return WiFiClient::connect(transport_ip_, port, timeout_ms);
    }

    RoutedWiFiClientSecure::RoutedWiFiClientSecure(
        const IPAddress& transport_ip,
        const char* root_ca)
        : transport_ip_(transport_ip), root_ca_(root_ca) {}

    int RoutedWiFiClientSecure::connect(const char* host, uint16_t port) {
        return WiFiClientSecure::connect(
            transport_ip_, port, host, root_ca_, nullptr, nullptr);
    }

    int RoutedWiFiClientSecure::connect(
        const char* host,
        uint16_t port,
        int32_t timeout_ms) {
        const unsigned long timeout_seconds =
            static_cast<unsigned long>((timeout_ms + 999) / 1000);
        setHandshakeTimeout(timeout_seconds > 0 ? timeout_seconds : 1);
        return WiFiClientSecure::connect(
            transport_ip_, port, host, root_ca_, nullptr, nullptr);
    }

    bool begin_http(HTTPClient& http,
                    RoutedWiFiClient& plain,
                    RoutedWiFiClientSecure& secure,
                    const EndpointContext& endpoint,
                    const String& logical_url) {
        if (!endpoint.transport_ip_valid) {
            return false;
        }

        http.setTimeout(gateway::ota_config::kHttpTimeoutMs);
        http.setConnectTimeout(gateway::ota_config::kHttpTimeoutMs);
        http.setReuse(false);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

        Serial.printf("[%s] http route type=%s logical=%s transport=%s:%u host=%s\r\n",
            kTag,
            endpoint_kind_string(endpoint.kind),
            logical_url.c_str(),
            endpoint.transport_ip.toString().c_str(),
            static_cast<unsigned>(endpoint.port),
            endpoint_host_header(endpoint).c_str());

        if (endpoint.use_tls) {
            return http.begin(secure, logical_url);
        }
        return http.begin(plain, logical_url);
    }

}  // namespace gateway::ota::detail
