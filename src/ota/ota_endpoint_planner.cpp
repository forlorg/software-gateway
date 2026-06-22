#include "ota/ota_endpoint_planner.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config/config_store.h"
#include "config/ota_config.h"
#include "system/gateway_context.h"
#include "system/version.h"

namespace gateway::ota::detail {
    namespace {

        static constexpr const char* kTag = "OTA";

        void set_error(String* error_code, const char* error) {
            if (error_code) {
                *error_code = error ? error : "";
            }
        }

        String url_encode(const String& value) {
            static const char* kHex = "0123456789ABCDEF";
            String output;
            output.reserve(value.length() * 3);

            for (size_t i = 0; i < value.length(); ++i) {
                const uint8_t c = static_cast<uint8_t>(value[i]);
                if ((c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~') {
                    output += static_cast<char>(c);
                } else {
                    output += '%';
                    output += kHex[(c >> 4) & 0x0F];
                    output += kHex[c & 0x0F];
                }
            }
            return output;
        }

        String mac_compact() {
            String mac = WiFi.macAddress();
            mac.replace(":", "");
            mac.toLowerCase();
            return mac.length() > 0 ? mac : String("unknown");
        }

        String product_id_or_unknown() {
            char product_id[9]{};
            if (gateway::ctx::has_product_id()) {
                gateway::ctx::get_product_id_hex(product_id);
            }
            return product_id[0] == '\0' ? String("unknown") : String(product_id);
        }

        bool is_default_port(const String& scheme, uint16_t port) {
            return (scheme == "http" && port == 80) ||
                   (scheme == "https" && port == 443);
        }

        bool is_valid_ipv4(const IPAddress& ip) {
            const bool all_zero = ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
            const bool broadcast = ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
            const bool multicast = ip[0] >= 224 && ip[0] <= 239;
            return !all_zero && !broadcast && !multicast;
        }

        bool parse_ipv4(const String& text, IPAddress* output) {
            return output && output->fromString(text) && is_valid_ipv4(*output);
        }

        void clear_domain_cache(const char* reason) {
            gateway::config_store::ota_domain_cache_clear();
            Serial.printf("[%s] domain cache deleted reason=%s\r\n",
                kTag, reason ? reason : "unknown");
        }

        bool load_cached_endpoint(EndpointContext* endpoint) {
            if (!endpoint) {
                return false;
            }

            gateway::config_store::OtaDomainCache cache;
            if (!gateway::config_store::ota_domain_cache_get(cache)) {
                return false;
            }

            const bool signature_matches =
                cache.version == gateway::ota_config::kDomainCacheVersion &&
                cache.scheme == gateway::ota_config::kPrimaryScheme &&
                cache.host == gateway::ota_config::kPrimaryHost &&
                cache.port == gateway::ota_config::kPrimaryPort;

            IPAddress ip;
            if (!signature_matches || !parse_ipv4(cache.ipv4, &ip)) {
                clear_domain_cache("invalid_or_config_changed");
                return false;
            }

            *endpoint = primary_endpoint(EndpointKind::CachedDomainIp);
            endpoint->transport_ip = ip;
            endpoint->transport_ip_valid = true;
            return true;
        }

    }  // namespace

    const char* endpoint_kind_string(EndpointKind kind) {
        switch (kind) {
        case EndpointKind::CachedDomainIp: return "cached_ip";
        case EndpointKind::Domain:         return "domain";
        case EndpointKind::LanFallback:    return "lan_fallback";
        default:                           return "unknown";
        }
    }

    uint8_t endpoint_rank(EndpointKind kind) {
        return static_cast<uint8_t>(kind);
    }

    EndpointContext primary_endpoint(EndpointKind kind) {
        EndpointContext endpoint;
        endpoint.kind = kind;
        endpoint.scheme = gateway::ota_config::kPrimaryScheme;
        endpoint.logical_host = gateway::ota_config::kPrimaryHost;
        endpoint.port = gateway::ota_config::kPrimaryPort;
        endpoint.use_tls = gateway::ota_config::kPrimaryUseTls;
        return endpoint;
    }

    EndpointContext lan_endpoint() {
        EndpointContext endpoint;
        endpoint.kind = EndpointKind::LanFallback;
        endpoint.scheme = gateway::ota_config::kLanScheme;
        endpoint.logical_host = gateway::ota_config::kLanHost;
        endpoint.port = gateway::ota_config::kLanPort;
        endpoint.use_tls = endpoint.scheme == "https";
        return endpoint;
    }

    String build_base_url(const EndpointContext& endpoint) {
        String base = endpoint.scheme + "://" + endpoint.logical_host;
        if (!is_default_port(endpoint.scheme, endpoint.port)) {
            base += ":" + String(endpoint.port);
        }
        return base;
    }

    String endpoint_host_header(const EndpointContext& endpoint) {
        String host = endpoint.logical_host;
        if (!is_default_port(endpoint.scheme, endpoint.port)) {
            host += ":" + String(endpoint.port);
        }
        return host;
    }

    String expected_firmware_path(const String& version) {
        return String("/api/v1/ota/") +
               gateway::version::project_name() +
               "/" + version + "/" +
               gateway::ota_config::kFirmwareFileName;
    }

    String build_check_url(const EndpointContext& endpoint) {
        String url = build_base_url(endpoint) + gateway::ota_config::kCheckPath;
        url += "?project=" + url_encode(gateway::version::project_name());
        url += "&hw=" + url_encode(gateway::version::hardware());
        url += "&channel=" + url_encode(gateway::version::channel());
        url += "&version=" + url_encode(gateway::version::firmware_version());
        url += "&build=" + String(gateway::version::firmware_build());
        url += "&device_id=" + url_encode(mac_compact());
        url += "&product_id=" + url_encode(product_id_or_unknown());
        url += "&mac=" + url_encode(WiFi.macAddress());
        return url;
    }

    bool save_domain_cache(const EndpointContext& endpoint) {
        if (endpoint.kind != EndpointKind::Domain || !endpoint.transport_ip_valid) {
            return false;
        }

        gateway::config_store::OtaDomainCache cache;
        cache.version = gateway::ota_config::kDomainCacheVersion;
        cache.scheme = endpoint.scheme;
        cache.host = endpoint.logical_host;
        cache.port = endpoint.port;
        cache.ipv4 = endpoint.transport_ip.toString();

        const bool saved = gateway::config_store::ota_domain_cache_set(cache);
        Serial.printf("[%s] domain cache %s ip=%s\r\n",
            kTag, saved ? "saved" : "save_failed", cache.ipv4.c_str());
        return saved;
    }

    CandidatePlan build_candidate_plan(uint8_t minimum_rank) {
        CandidatePlan plan;

        if (minimum_rank <= endpoint_rank(EndpointKind::CachedDomainIp)) {
            EndpointContext cached;
            if (load_cached_endpoint(&cached)) {
                plan.endpoints[plan.count++] = cached;
            }
        }

        if (minimum_rank <= endpoint_rank(EndpointKind::Domain)) {
            plan.endpoints[plan.count++] = primary_endpoint(EndpointKind::Domain);
        }

        if (minimum_rank <= endpoint_rank(EndpointKind::LanFallback)) {
            plan.endpoints[plan.count++] = lan_endpoint();
        }

        String text;
        for (size_t i = 0; i < plan.count; ++i) {
            if (i > 0) {
                text += " -> ";
            }
            text += endpoint_kind_string(plan.endpoints[i].kind);
        }
        Serial.printf("[%s] candidate plan: %s\r\n",
            kTag, text.length() > 0 ? text.c_str() : "(empty)");
        return plan;
    }

    bool prepare_endpoint(EndpointContext* endpoint, String* error_code) {
        if (!endpoint) {
            set_error(error_code, "ota_transport_ip_missing");
            return false;
        }

        if (endpoint->kind == EndpointKind::CachedDomainIp) {
            if (endpoint->transport_ip_valid && is_valid_ipv4(endpoint->transport_ip)) {
                return true;
            }
            set_error(error_code, "ota_transport_ip_missing");
            return false;
        }

        if (endpoint->kind == EndpointKind::Domain) {
            const uint32_t started = millis();
            IPAddress resolved;
            const int result = WiFi.hostByName(endpoint->logical_host.c_str(), resolved);
            const uint32_t latency = millis() - started;

            if (result != 1 || !is_valid_ipv4(resolved)) {
                Serial.printf("[%s] dns failed host=%s result=%d latency=%lu ms\r\n",
                    kTag,
                    endpoint->logical_host.c_str(),
                    result,
                    static_cast<unsigned long>(latency));
                set_error(error_code,
                    result == 1 ? "ota_dns_ipv4_invalid" : "ota_dns_failed");
                return false;
            }

            endpoint->transport_ip = resolved;
            endpoint->transport_ip_valid = true;
            Serial.printf("[%s] dns ok host=%s ip=%s latency=%lu ms\r\n",
                kTag,
                endpoint->logical_host.c_str(),
                resolved.toString().c_str(),
                static_cast<unsigned long>(latency));
            return true;
        }

        IPAddress lan_ip;
        if (!parse_ipv4(endpoint->logical_host, &lan_ip)) {
            set_error(error_code, "ota_lan_ipv4_invalid");
            return false;
        }
        endpoint->transport_ip = lan_ip;
        endpoint->transport_ip_valid = true;
        return true;
    }

    void handle_candidate_failure(const EndpointContext& endpoint, const char* stage) {
        Serial.printf("[%s] candidate failed type=%s stage=%s transport=%s\r\n",
            kTag,
            endpoint_kind_string(endpoint.kind),
            stage ? stage : "unknown",
            endpoint.transport_ip_valid ? endpoint.transport_ip.toString().c_str() : "unresolved");

        if (endpoint.kind == EndpointKind::CachedDomainIp) {
            clear_domain_cache(stage ? stage : "cached_endpoint_failed");
        }
    }

}  // namespace gateway::ota::detail
