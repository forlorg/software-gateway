#include "system/ota_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <ctype.h>
#include <string.h>

#include "config/config_store.h"
#include "config/ota_config.h"
#include "system/gateway_context.h"
#include "system/version.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"

namespace gateway::ota {
    namespace {

        static constexpr const char* kTag = "OTA";

        enum class EndpointKind : uint8_t {
            CachedDomainIp = 0,
            Domain = 1,
            LanFallback = 2
        };

        struct EndpointContext {
            EndpointKind kind = EndpointKind::Domain;
            String scheme;
            String logical_host;
            uint16_t port = 0;
            bool use_tls = false;
            IPAddress transport_ip;
            bool transport_ip_valid = false;
        };

        struct CandidatePlan {
            EndpointContext endpoints[3];
            size_t count = 0;
        };

        struct OtaManifest {
            bool update_available = false;
            String project;
            String hw;
            String channel;
            String version;
            uint32_t build = 0;
            bool mandatory = false;
            String firmware_url;
            String firmware_path;
            size_t size = 0;
            String md5;
            String sha256;
            String release_notes;
            EndpointContext source;
            bool source_valid = false;
        };

        struct Sha256Context {
            mbedtls_sha256_context ctx;
        };

        /**
         * HTTPClient 仍使用逻辑域名构造 Host 请求头；该客户端只改写底层 TCP
         * 连接目标，使缓存 IP / DNS IP 不会破坏域名虚拟主机。
         */
        class RoutedWiFiClient final : public WiFiClient {
        public:
            explicit RoutedWiFiClient(const IPAddress& transport_ip)
                : transport_ip_(transport_ip) {}

            using WiFiClient::connect;

            int connect(const char*, uint16_t port) override {
                return WiFiClient::connect(transport_ip_, port);
            }

            int connect(const char*, uint16_t port, int32_t timeout_ms) override {
                return WiFiClient::connect(transport_ip_, port, timeout_ms);
            }

        private:
            IPAddress transport_ip_;
        };

        /**
         * 为未来 HTTPS 预留：TCP 连接缓存 IP，但 TLS SNI/证书主机名仍使用
         * HTTPClient 传入的逻辑域名。
         */
        class RoutedWiFiClientSecure final : public WiFiClientSecure {
        public:
            RoutedWiFiClientSecure(const IPAddress& transport_ip, const char* root_ca)
                : transport_ip_(transport_ip), root_ca_(root_ca) {}

            using WiFiClientSecure::connect;

            int connect(const char* host, uint16_t port) override {
                return WiFiClientSecure::connect(
                    transport_ip_, port, host, root_ca_, nullptr, nullptr);
            }

            int connect(const char* host, uint16_t port, int32_t timeout_ms) override {
                const unsigned long timeout_seconds =
                    static_cast<unsigned long>((timeout_ms + 999) / 1000);
                setHandshakeTimeout(timeout_seconds > 0 ? timeout_seconds : 1);
                return WiFiClientSecure::connect(
                    transport_ip_, port, host, root_ca_, nullptr, nullptr);
            }

        private:
            IPAddress transport_ip_;
            const char* root_ca_;
        };

        portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

        State g_state = State::Disabled;
        bool g_check_requested = false;
        bool g_upgrade_requested = false;
        bool g_auto_first_check_done = false;
        bool g_have_manifest = false;
        bool g_update_available = false;
        uint8_t g_progress = 0;
        uint32_t g_latest_build = 0;
        String g_latest_version;
        String g_last_error;
        OtaManifest g_manifest;

        uint32_t g_started_ms = 0;
        uint32_t g_last_check_ms = 0;
        uint32_t g_next_retry_ms = 0;
        uint32_t g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMinMs;

        // =========================================================================
        // 状态与通用工具
        // =========================================================================

        void set_state(State state) {
            portENTER_CRITICAL(&g_lock);
            const State previous = g_state;
            g_state = state;
            portEXIT_CRITICAL(&g_lock);

            if (previous != state) {
                Serial.printf("[%s] state %s -> %s\r\n",
                    kTag, state_string(previous), state_string(state));
            }
        }

        void set_progress(uint8_t progress) {
            portENTER_CRITICAL(&g_lock);
            g_progress = progress;
            portEXIT_CRITICAL(&g_lock);
        }

        void set_last_error(const char* error) {
            portENTER_CRITICAL(&g_lock);
            g_last_error = error ? error : "";
            portEXIT_CRITICAL(&g_lock);
            Serial.printf("[%s] error=%s\r\n", kTag, error ? error : "(clear)");
        }

        void clear_last_error() {
            portENTER_CRITICAL(&g_lock);
            g_last_error = "";
            portEXIT_CRITICAL(&g_lock);
        }

        bool is_busy_state(State state) {
            return state == State::CheckManifest ||
                   state == State::Downloading ||
                   state == State::Verifying ||
                   state == State::Applying ||
                   state == State::Rebooting;
        }

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

        bool is_hex_string(const String& value, size_t expected_length) {
            if (value.length() != expected_length) {
                Serial.printf("[%s] hex string len mismatch: got=%u expect=%u\r\n",
                    kTag,
                    static_cast<unsigned>(value.length()),
                    static_cast<unsigned>(expected_length));
                return false;
            }

            for (size_t i = 0; i < value.length(); ++i) {
                if (!isxdigit(static_cast<unsigned char>(value[i]))) {
                    Serial.printf("[%s] hex string invalid char at pos=%u\r\n",
                        kTag, static_cast<unsigned>(i));
                    return false;
                }
            }
            return true;
        }

        bool is_valid_version(const String& version) {
            if (version.length() == 0 || version.length() > 32) {
                Serial.printf("[%s] version invalid len=%u\r\n",
                    kTag, static_cast<unsigned>(version.length()));
                return false;
            }

            for (size_t i = 0; i < version.length(); ++i) {
                const char c = version[i];
                const bool valid = isalnum(static_cast<unsigned char>(c)) ||
                                   c == '.' || c == '_' || c == '-';
                if (!valid) {
                    Serial.printf("[%s] version invalid char='%c' at pos=%u\r\n",
                        kTag, c, static_cast<unsigned>(i));
                    return false;
                }
            }
            return true;
        }

        bool equals_ignore_case(const String& left, const String& right) {
            if (left.length() != right.length()) {
                return false;
            }

            for (size_t i = 0; i < left.length(); ++i) {
                if (tolower(static_cast<unsigned char>(left[i])) !=
                    tolower(static_cast<unsigned char>(right[i]))) {
                    return false;
                }
            }
            return true;
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

        String build_base_url(const EndpointContext& endpoint) {
            String base = endpoint.scheme + "://" + endpoint.logical_host;
            if (!is_default_port(endpoint.scheme, endpoint.port)) {
                base += ":" + String(endpoint.port);
            }
            return base;
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

        String endpoint_host_header(const EndpointContext& endpoint) {
            String host = endpoint.logical_host;
            if (!is_default_port(endpoint.scheme, endpoint.port)) {
                host += ":" + String(endpoint.port);
            }
            return host;
        }

        bool is_valid_ipv4(const IPAddress& ip) {
            const bool all_zero = ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
            const bool broadcast = ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
            const bool multicast = ip[0] >= 224 && ip[0] <= 239;
            return !all_zero && !broadcast && !multicast;
        }

        bool parse_ipv4(const String& text, IPAddress* output) {
            if (!output || !output->fromString(text) || !is_valid_ipv4(*output)) {
                return false;
            }
            return true;
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

        bool prepare_endpoint(EndpointContext* endpoint) {
            if (!endpoint) {
                return false;
            }

            if (endpoint->kind == EndpointKind::CachedDomainIp) {
                return endpoint->transport_ip_valid && is_valid_ipv4(endpoint->transport_ip);
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
                    set_last_error(result == 1 ? "ota_dns_ipv4_invalid" : "ota_dns_failed");
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
                set_last_error("ota_lan_ipv4_invalid");
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

        // =========================================================================
        // HTTP client
        // =========================================================================

        template <typename PlainClient, typename SecureClient>
        bool begin_http(HTTPClient& http,
                        PlainClient& plain,
                        SecureClient& secure,
                        const EndpointContext& endpoint,
                        const String& logical_url) {
            if (!endpoint.transport_ip_valid) {
                set_last_error("ota_transport_ip_missing");
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

        // =========================================================================
        // SHA-256
        // =========================================================================

        void sha256_init(Sha256Context* context) {
            mbedtls_sha256_init(&context->ctx);
#if MBEDTLS_VERSION_MAJOR >= 3
            mbedtls_sha256_starts(&context->ctx, 0);
#else
            mbedtls_sha256_starts_ret(&context->ctx, 0);
#endif
        }

        void sha256_update(Sha256Context* context, const uint8_t* data, size_t length) {
#if MBEDTLS_VERSION_MAJOR >= 3
            mbedtls_sha256_update(&context->ctx, data, length);
#else
            mbedtls_sha256_update_ret(&context->ctx, data, length);
#endif
        }

        void sha256_abort(Sha256Context* context) {
            mbedtls_sha256_free(&context->ctx);
        }

        String sha256_final_hex(Sha256Context* context) {
            uint8_t output[32];
#if MBEDTLS_VERSION_MAJOR >= 3
            mbedtls_sha256_finish(&context->ctx, output);
#else
            mbedtls_sha256_finish_ret(&context->ctx, output);
#endif
            mbedtls_sha256_free(&context->ctx);

            char hex[65];
            for (int i = 0; i < 32; ++i) {
                snprintf(hex + i * 2, 3, "%02x", output[i]);
            }
            hex[64] = '\0';
            return String(hex);
        }

        // =========================================================================
        // OTA 分区与 URL 校验
        // =========================================================================

        size_t ota_partition_size() {
            const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
            if (!partition) {
                Serial.printf("[%s] ota partition: not found\r\n", kTag);
                return 0;
            }

            Serial.printf("[%s] ota partition: addr=0x%lx size=%lu KiB\r\n",
                kTag,
                static_cast<unsigned long>(partition->address),
                static_cast<unsigned long>(partition->size / 1024));
            return partition->size;
        }

        String firmware_url_from_manifest(const OtaManifest& manifest,
                                          const EndpointContext& endpoint) {
            const String expected_path = expected_firmware_path(manifest.version);
            if (manifest.firmware_path == expected_path) {
                return build_base_url(endpoint) + manifest.firmware_path;
            }

            if (manifest.firmware_path.length() == 0 &&
                manifest.firmware_url.length() > 0) {
                Serial.printf("[%s] firmware_path empty, fallback to firmware_url\r\n", kTag);
                return manifest.firmware_url;
            }
            return "";
        }

        bool is_allowed_firmware_url(const String& url,
                                     const String& version,
                                     const EndpointContext& endpoint) {
            const String expected = build_base_url(endpoint) + expected_firmware_path(version);
            return url == expected;
        }

        bool validate_manifest(const OtaManifest& manifest,
                               const EndpointContext& endpoint) {
            Serial.printf("[%s] validating manifest: project=%s hw=%s channel=%s "
                          "version=%s build=%lu size=%u source=%s\r\n",
                kTag,
                manifest.project.c_str(),
                manifest.hw.c_str(),
                manifest.channel.c_str(),
                manifest.version.c_str(),
                static_cast<unsigned long>(manifest.build),
                static_cast<unsigned>(manifest.size),
                endpoint_kind_string(endpoint.kind));

            if (manifest.project != gateway::version::project_name()) {
                set_last_error("manifest_project_mismatch");
                return false;
            }
            if (manifest.hw != gateway::version::hardware()) {
                set_last_error("manifest_hw_mismatch");
                return false;
            }
            if (manifest.channel != gateway::version::channel()) {
                set_last_error("manifest_channel_mismatch");
                return false;
            }
            if (!is_valid_version(manifest.version)) {
                set_last_error("manifest_version_invalid");
                return false;
            }
            if (manifest.build <= gateway::version::firmware_build()) {
                set_last_error("manifest_build_not_newer");
                return false;
            }
            if (manifest.size == 0) {
                set_last_error("manifest_size_invalid");
                return false;
            }

            const size_t partition_size = ota_partition_size();
            if (partition_size > 0 && manifest.size > partition_size) {
                set_last_error("manifest_size_too_large");
                return false;
            }
            if (!is_hex_string(manifest.md5, 32)) {
                set_last_error("manifest_md5_invalid");
                return false;
            }
            if (!is_hex_string(manifest.sha256, 64)) {
                set_last_error("manifest_sha256_invalid");
                return false;
            }

            const String expected_path = expected_firmware_path(manifest.version);
            if (manifest.firmware_path.length() > 0 &&
                manifest.firmware_path != expected_path) {
                set_last_error("manifest_firmware_path_invalid");
                return false;
            }
            if (manifest.firmware_path.length() == 0 &&
                manifest.firmware_url.length() == 0) {
                set_last_error("manifest_firmware_path_invalid");
                return false;
            }

            const String url = firmware_url_from_manifest(manifest, endpoint);
            if (!is_allowed_firmware_url(url, manifest.version, endpoint)) {
                Serial.printf("[%s] firmware url not allowed: '%s' source=%s\r\n",
                    kTag, url.c_str(), endpoint_kind_string(endpoint.kind));
                set_last_error("firmware_url_not_allowed");
                return false;
            }

            Serial.printf("[%s] manifest validation PASSED source=%s\r\n",
                kTag, endpoint_kind_string(endpoint.kind));
            return true;
        }

        // =========================================================================
        // Manifest 获取与缓存
        // =========================================================================

        bool fetch_manifest(const EndpointContext& endpoint, OtaManifest* output) {
            if (!output) {
                set_last_error("manifest_output_null");
                return false;
            }
            if (WiFi.status() != WL_CONNECTED) {
                set_last_error("wifi_not_connected");
                return false;
            }

            set_state(State::CheckManifest);
            set_progress(0);
            clear_last_error();

            RoutedWiFiClient plain(endpoint.transport_ip);
            RoutedWiFiClientSecure secure(
                endpoint.transport_ip, gateway::ota_config::kRootCaPem);
            HTTPClient http;

            const String url = build_check_url(endpoint);
            const uint32_t started = millis();

            if (!begin_http(http, plain, secure, endpoint, url)) {
                set_last_error("manifest_http_begin_failed");
                return false;
            }

            const int code = http.GET();
            const uint32_t latency = millis() - started;
            Serial.printf("[%s] manifest response source=%s code=%d latency=%lu ms\r\n",
                kTag,
                endpoint_kind_string(endpoint.kind),
                code,
                static_cast<unsigned long>(latency));

            if (code != HTTP_CODE_OK) {
                set_last_error("manifest_http_not_ok");
                http.end();
                return false;
            }

            const int content_length = http.getSize();
            if (content_length > static_cast<int>(gateway::ota_config::kManifestMaxBytes)) {
                set_last_error("manifest_body_too_large");
                http.end();
                return false;
            }

            const String body = http.getString();
            http.end();

            if (body.length() > gateway::ota_config::kManifestMaxBytes) {
                set_last_error("manifest_body_too_large");
                return false;
            }

            JsonDocument document;
            const DeserializationError error = deserializeJson(document, body);
            if (error) {
                Serial.printf("[%s] json parse failed: %s\r\n", kTag, error.c_str());
                set_last_error("manifest_json_parse_failed");
                return false;
            }

            if ((document["ok"] | false) != true) {
                set_last_error("manifest_ok_false");
                return false;
            }

            *output = OtaManifest{};
            output->source = endpoint;
            output->source_valid = true;
            output->update_available = document["update_available"] | false;

            if (!output->update_available) {
                const char* reason = document["reason"] | "";
                Serial.printf("[%s] no update source=%s reason=%s\r\n",
                    kTag, endpoint_kind_string(endpoint.kind), reason);
                return true;
            }

            output->project = document["project"] | "";
            output->hw = document["hw"] | "";
            output->channel = document["channel"] | "";
            output->version = document["version"] | "";
            output->build = document["build"] | 0;
            output->mandatory = document["mandatory"] | false;
            output->firmware_path = document["firmware_path"] | "";
            output->firmware_url = document["firmware_url"] | "";
            output->size = document["size"] | 0;
            output->md5 = document["md5"] | "";
            output->sha256 = document["sha256"] | "";
            output->release_notes = document["release_notes"] | "";

            if (!validate_manifest(*output, endpoint)) {
                return false;
            }

            Serial.printf("[%s] manifest ok source=%s version=%s build=%lu size=%u\r\n",
                kTag,
                endpoint_kind_string(endpoint.kind),
                output->version.c_str(),
                static_cast<unsigned long>(output->build),
                static_cast<unsigned>(output->size));
            return true;
        }

        void store_manifest(const OtaManifest& manifest) {
            portENTER_CRITICAL(&g_lock);
            g_manifest = manifest;
            g_have_manifest = manifest.update_available;
            g_update_available = manifest.update_available;
            g_latest_version = manifest.update_available ? manifest.version : "";
            g_latest_build = manifest.update_available ? manifest.build : 0;
            portEXIT_CRITICAL(&g_lock);

            if (manifest.update_available) {
                Serial.printf("[%s] manifest stored source=%s version=%s build=%lu\r\n",
                    kTag,
                    manifest.source_valid
                        ? endpoint_kind_string(manifest.source.kind)
                        : "unknown",
                    manifest.version.c_str(),
                    static_cast<unsigned long>(manifest.build));
            }
        }

        // =========================================================================
        // 退避与进度
        // =========================================================================

        void enter_backoff() {
            set_state(State::Backoff);
            const uint32_t now = millis();
            g_next_retry_ms = now + g_retry_backoff_ms;

            Serial.printf("[%s] backoff: retry in %lu s\r\n",
                kTag,
                static_cast<unsigned long>(g_retry_backoff_ms / 1000));

            if (g_retry_backoff_ms < gateway::ota_config::kRetryBackoffMaxMs / 2) {
                g_retry_backoff_ms *= 2;
            } else {
                g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMaxMs;
            }
        }

        void reset_backoff() {
            g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMinMs;
            g_next_retry_ms = 0;
        }

        void update_progress(size_t written, size_t total) {
            uint8_t progress = 0;
            if (total > 0) {
                progress = static_cast<uint8_t>((written * 100UL) / total);
                if (progress > 100) {
                    progress = 100;
                }
            }

            set_progress(progress);
            static uint8_t last_printed = 255;
            if (progress != last_printed &&
                (progress == 100 || progress % 10 == 0)) {
                last_printed = progress;
                Serial.printf("[%s] download progress=%u%% (%u/%u B)\r\n",
                    kTag,
                    static_cast<unsigned>(progress),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(total));
            }
        }

        // =========================================================================
        // 固件下载、校验与写入
        // =========================================================================

        bool download_and_apply(const OtaManifest& manifest,
                                const EndpointContext& endpoint) {
            const String firmware_url = firmware_url_from_manifest(manifest, endpoint);
            if (!is_allowed_firmware_url(firmware_url, manifest.version, endpoint)) {
                set_last_error("firmware_url_not_allowed");
                return false;
            }

            Serial.printf("[%s] download start source=%s url=%s transport=%s\r\n",
                kTag,
                endpoint_kind_string(endpoint.kind),
                firmware_url.c_str(),
                endpoint.transport_ip.toString().c_str());

            set_state(State::Downloading);
            set_progress(0);

            RoutedWiFiClient plain(endpoint.transport_ip);
            RoutedWiFiClientSecure secure(
                endpoint.transport_ip, gateway::ota_config::kRootCaPem);
            HTTPClient http;

            if (!begin_http(http, plain, secure, endpoint, firmware_url)) {
                set_last_error("firmware_http_begin_failed");
                return false;
            }

            const uint32_t download_started = millis();
            const int code = http.GET();
            if (code != HTTP_CODE_OK) {
                Serial.printf("[%s] firmware http code=%d source=%s\r\n",
                    kTag, code, endpoint_kind_string(endpoint.kind));
                set_last_error("firmware_http_not_ok");
                http.end();
                return false;
            }

            const int content_length = http.getSize();
            if (content_length <= 0 ||
                static_cast<size_t>(content_length) != manifest.size) {
                set_last_error("firmware_size_mismatch");
                http.end();
                return false;
            }

            if (!Update.begin(content_length, U_FLASH)) {
                Serial.printf("[%s] Update.begin failed: %s\r\n",
                    kTag, Update.errorString());
                set_last_error("update_begin_failed");
                http.end();
                return false;
            }

            Update.setMD5(manifest.md5.c_str());

            WiFiClient* stream = http.getStreamPtr();
            if (!stream) {
                Update.abort();
                set_last_error("firmware_stream_missing");
                http.end();
                return false;
            }

            uint8_t buffer[gateway::ota_config::kDownloadBufferSize];
            Sha256Context sha_context;
            sha256_init(&sha_context);

            size_t written = 0;
            uint32_t last_received_ms = millis();

            while (http.connected() && written < static_cast<size_t>(content_length)) {
                const size_t available = stream->available();
                if (available == 0) {
                    if (millis() - last_received_ms >
                        gateway::ota_config::kDownloadIdleTimeoutMs) {
                        sha256_abort(&sha_context);
                        Update.abort();
                        set_last_error("download_idle_timeout");
                        http.end();
                        return false;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                const size_t to_read = available > sizeof(buffer)
                    ? sizeof(buffer)
                    : available;
                const int read_length = stream->readBytes(buffer, to_read);
                if (read_length <= 0) {
                    sha256_abort(&sha_context);
                    Update.abort();
                    set_last_error("stream_read_failed");
                    http.end();
                    return false;
                }

                last_received_ms = millis();
                sha256_update(&sha_context, buffer, static_cast<size_t>(read_length));

                const size_t write_length =
                    Update.write(buffer, static_cast<size_t>(read_length));
                if (write_length != static_cast<size_t>(read_length)) {
                    sha256_abort(&sha_context);
                    Update.abort();
                    set_last_error("flash_write_failed");
                    http.end();
                    return false;
                }

                written += write_length;
                update_progress(written, static_cast<size_t>(content_length));
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            if (written != static_cast<size_t>(content_length)) {
                sha256_abort(&sha_context);
                Update.abort();
                set_last_error("download_incomplete");
                http.end();
                return false;
            }

            set_state(State::Verifying);
            const String actual_sha256 = sha256_final_hex(&sha_context);
            if (!equals_ignore_case(actual_sha256, manifest.sha256)) {
                Update.abort();
                set_last_error("sha256_mismatch");
                http.end();
                return false;
            }

            set_state(State::Applying);
            if (!Update.end(true)) {
                Serial.printf("[%s] Update.end failed: ", kTag);
                Update.printError(Serial);
                Update.abort();
                set_last_error("update_end_failed");
                http.end();
                return false;
            }

            if (!Update.isFinished()) {
                Update.abort();
                set_last_error("update_not_finished");
                http.end();
                return false;
            }

            http.end();

            const uint32_t duration_ms = millis() - download_started;
            Serial.printf("[%s] update success source=%s bytes=%u duration=%lu ms, rebooting\r\n",
                kTag,
                endpoint_kind_string(endpoint.kind),
                static_cast<unsigned>(written),
                static_cast<unsigned long>(duration_ms));

            set_state(State::Rebooting);
            delay(500);
            ESP.restart();
            return true;
        }

        // =========================================================================
        // 候选端点执行
        // =========================================================================

        bool execute_candidate_plan(const CandidatePlan& plan,
                                    bool upgrade_after_check) {
            for (size_t i = 0; i < plan.count; ++i) {
                EndpointContext endpoint = plan.endpoints[i];
                Serial.printf("[%s] candidate attempt=%u/%u type=%s\r\n",
                    kTag,
                    static_cast<unsigned>(i + 1),
                    static_cast<unsigned>(plan.count),
                    endpoint_kind_string(endpoint.kind));

                if (!prepare_endpoint(&endpoint)) {
                    handle_candidate_failure(endpoint, "prepare");
                    continue;
                }

                OtaManifest manifest;
                const bool check_ok = fetch_manifest(endpoint, &manifest);
                g_last_check_ms = millis();

                if (!check_ok) {
                    handle_candidate_failure(endpoint, "check");
                    continue;
                }

                // 域名有效 check 成功（含无更新）后，保存本次实际使用的单个 IPv4。
                if (endpoint.kind == EndpointKind::Domain) {
                    save_domain_cache(endpoint);
                }

                reset_backoff();

                if (!manifest.update_available) {
                    store_manifest(manifest);
                    set_state(State::UpToDate);
                    Serial.printf("[%s] firmware is up-to-date source=%s\r\n",
                        kTag, endpoint_kind_string(endpoint.kind));
                    return true;
                }

                store_manifest(manifest);
                set_state(State::UpdateAvailable);

                const bool should_upgrade = upgrade_after_check || manifest.mandatory;
                if (!should_upgrade) {
                    Serial.printf("[%s] update available, waiting for upgrade trigger\r\n", kTag);
                    return true;
                }

                if (manifest.mandatory) {
                    Serial.printf("[%s] mandatory update, auto-upgrading\r\n", kTag);
                }

                if (download_and_apply(manifest, endpoint)) {
                    return true;
                }

                // 下载失败后不得跨端点复用 manifest；循环会在下一候选重新 check。
                handle_candidate_failure(endpoint, "download");
                Serial.printf("[%s] download failed, next endpoint must re-check\r\n", kTag);
            }

            Serial.printf("[%s] all OTA candidates exhausted\r\n", kTag);
            return false;
        }

        void do_check(bool upgrade_after_check) {
            Serial.printf("[%s] do_check upgrade_after_check=%d\r\n",
                kTag, static_cast<int>(upgrade_after_check));

            const CandidatePlan plan = build_candidate_plan(0);
            if (!execute_candidate_plan(plan, upgrade_after_check)) {
                enter_backoff();
            }
        }

        void upgrade_cached_manifest() {
            OtaManifest manifest = g_manifest;
            if (!manifest.source_valid) {
                Serial.printf("[%s] cached manifest source missing, re-checking\r\n", kTag);
                do_check(true);
                return;
            }

            Serial.printf("[%s] using cached manifest version=%s source=%s\r\n",
                kTag,
                manifest.version.c_str(),
                endpoint_kind_string(manifest.source.kind));

            if (download_and_apply(manifest, manifest.source)) {
                return;
            }

            handle_candidate_failure(manifest.source, "download");

            const uint8_t next_rank = endpoint_rank(manifest.source.kind) + 1;
            const CandidatePlan fallback_plan = build_candidate_plan(next_rank);
            if (fallback_plan.count == 0 ||
                !execute_candidate_plan(fallback_plan, true)) {
                enter_backoff();
            }
        }

    }  // namespace

    // =========================================================================
    // 公开接口
    // =========================================================================

    void begin() {
        g_started_ms = millis();
        set_state(State::Idle);
        clear_last_error();

        Serial.printf("[%s] === OTA module init ===\r\n", kTag);
        Serial.printf("[%s] project    : %s\r\n", kTag, gateway::version::project_name());
        Serial.printf("[%s] version    : %s\r\n", kTag, gateway::version::firmware_version());
        Serial.printf("[%s] build      : %lu\r\n",
            kTag, static_cast<unsigned long>(gateway::version::firmware_build()));
        Serial.printf("[%s] hw         : %s\r\n", kTag, gateway::version::hardware());
        Serial.printf("[%s] channel    : %s\r\n", kTag, gateway::version::channel());

        const EndpointContext primary = primary_endpoint(EndpointKind::Domain);
        const EndpointContext lan = lan_endpoint();
        Serial.printf("[%s] primary    : %s\r\n", kTag, build_base_url(primary).c_str());
        Serial.printf("[%s] lan backup : %s\r\n", kTag, build_base_url(lan).c_str());
        Serial.printf("[%s] future tls : https://%s:%u\r\n",
            kTag,
            gateway::ota_config::kPrimaryHost,
            static_cast<unsigned>(gateway::ota_config::kPrimaryHttpsPort));
        Serial.printf("[%s] order      : cached_ip -> domain -> lan_fallback\r\n", kTag);
        Serial.printf("[%s] first check: %lu s after boot\r\n",
            kTag,
            static_cast<unsigned long>(gateway::ota_config::kFirstCheckDelayMs / 1000));
        Serial.printf("[%s] interval   : %lu h\r\n",
            kTag,
            static_cast<unsigned long>(gateway::ota_config::kCheckIntervalMs / 3600000));
        Serial.printf("[%s] build time : %s\r\n", kTag, gateway::version::build_time());

        ota_partition_size();
        Serial.printf("[%s] === OTA module ready ===\r\n", kTag);
    }

    void loop() {
        State current;
        bool check_requested = false;
        bool upgrade_requested = false;

        portENTER_CRITICAL(&g_lock);
        current = g_state;
        check_requested = g_check_requested;
        upgrade_requested = g_upgrade_requested;
        g_check_requested = false;
        g_upgrade_requested = false;
        portEXIT_CRITICAL(&g_lock);

        if (is_busy_state(current)) {
            return;
        }

        if (WiFi.status() != WL_CONNECTED) {
            set_state(State::WaitingNetwork);
            return;
        }

        const uint32_t now = millis();
        if (current == State::Backoff &&
            g_next_retry_ms != 0 &&
            now < g_next_retry_ms) {
            return;
        }

        bool auto_due = false;
        if (!g_auto_first_check_done &&
            now - g_started_ms >= gateway::ota_config::kFirstCheckDelayMs) {
            g_auto_first_check_done = true;
            auto_due = true;
            Serial.printf("[%s] first auto-check triggered\r\n", kTag);
        } else if (g_last_check_ms != 0 &&
                   now - g_last_check_ms >= gateway::ota_config::kCheckIntervalMs) {
            auto_due = true;
            Serial.printf("[%s] periodic auto-check triggered\r\n", kTag);
        }

        if (upgrade_requested) {
            Serial.printf("[%s] manual upgrade requested\r\n", kTag);
            if (g_have_manifest && g_update_available) {
                upgrade_cached_manifest();
            } else {
                do_check(true);
            }
            return;
        }

        if (auto_due) {
            do_check(true);
            return;
        }

        if (check_requested) {
            Serial.printf("[%s] manual check requested\r\n", kTag);
            do_check(false);
            return;
        }

        if (current == State::Backoff) {
            Serial.printf("[%s] backoff expired, retrying check\r\n", kTag);
            do_check(false);
            return;
        }

        if (current == State::WaitingNetwork) {
            set_state(g_update_available ? State::UpdateAvailable : State::Idle);
        }
    }

    bool request_check_now() {
        const uint32_t now = millis();

        portENTER_CRITICAL(&g_lock);
        const bool busy = is_busy_state(g_state);
        const bool too_soon =
            g_last_check_ms != 0 &&
            now - g_last_check_ms < gateway::ota_config::kManualCheckMinIntervalMs;
        if (!busy && !too_soon) {
            g_check_requested = true;
        }
        portEXIT_CRITICAL(&g_lock);

        if (busy) {
            Serial.printf("[%s] request_check_now: busy, rejected\r\n", kTag);
        } else if (too_soon) {
            Serial.printf("[%s] request_check_now: too soon\r\n", kTag);
        } else {
            Serial.printf("[%s] request_check_now: accepted\r\n", kTag);
        }
        return !busy && !too_soon;
    }

    bool request_upgrade_now() {
        portENTER_CRITICAL(&g_lock);
        const bool busy = is_busy_state(g_state);
        if (!busy) {
            g_upgrade_requested = true;
        }
        portEXIT_CRITICAL(&g_lock);

        Serial.printf("[%s] request_upgrade_now: %s\r\n",
            kTag, busy ? "busy, rejected" : "accepted");
        return !busy;
    }

    Status status() {
        Status status{};
        portENTER_CRITICAL(&g_lock);
        status.state = g_state;
        status.update_available = g_update_available;
        status.update_in_progress = is_busy_state(g_state);
        status.progress = g_progress;
        status.current_build = gateway::version::firmware_build();
        status.latest_build = g_latest_build;
        status.current_version = gateway::version::firmware_version();
        status.latest_version = g_latest_version.c_str();
        status.last_error = g_last_error.c_str();
        portEXIT_CRITICAL(&g_lock);
        return status;
    }

    bool update_in_progress() {
        portENTER_CRITICAL(&g_lock);
        const bool busy = is_busy_state(g_state);
        portEXIT_CRITICAL(&g_lock);
        return busy;
    }

    const char* state_string(State state) {
        switch (state) {
        case State::Disabled:        return "disabled";
        case State::Idle:            return "idle";
        case State::WaitingNetwork:  return "waiting_network";
        case State::CheckManifest:   return "check_manifest";
        case State::UpToDate:        return "up_to_date";
        case State::UpdateAvailable: return "update_available";
        case State::Downloading:     return "downloading";
        case State::Verifying:       return "verifying";
        case State::Applying:        return "applying";
        case State::Backoff:         return "backoff";
        case State::Rebooting:       return "rebooting";
        case State::Failed:          return "failed";
        default:                     return "unknown";
        }
    }

}  // namespace gateway::ota
