#include "ota/ota_manifest_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <ctype.h>

#include "config/ota_config.h"
#include "ota/ota_endpoint_planner.h"
#include "ota/ota_http_route.h"
#include "system/version.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace gateway::ota::detail {
    namespace {

        static constexpr const char* kTag = "OTA";

        bool fail(String* error_code, const char* error) {
            if (error_code) {
                *error_code = error ? error : "";
            }
            return false;
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

        bool validate_manifest(const OtaManifest& manifest,
                               const EndpointContext& endpoint,
                               String* error_code) {
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
                return fail(error_code, "manifest_project_mismatch");
            }
            if (manifest.hw != gateway::version::hardware()) {
                return fail(error_code, "manifest_hw_mismatch");
            }
            if (manifest.channel != gateway::version::channel()) {
                return fail(error_code, "manifest_channel_mismatch");
            }
            if (!is_valid_version(manifest.version)) {
                return fail(error_code, "manifest_version_invalid");
            }
            if (manifest.build <= gateway::version::firmware_build()) {
                return fail(error_code, "manifest_build_not_newer");
            }
            if (manifest.size == 0) {
                return fail(error_code, "manifest_size_invalid");
            }

            const size_t partition_size = ota_partition_size();
            if (partition_size > 0 && manifest.size > partition_size) {
                return fail(error_code, "manifest_size_too_large");
            }
            if (!is_hex_string(manifest.md5, 32)) {
                return fail(error_code, "manifest_md5_invalid");
            }
            if (!is_hex_string(manifest.sha256, 64)) {
                return fail(error_code, "manifest_sha256_invalid");
            }

            const String expected_path = expected_firmware_path(manifest.version);
            if (manifest.firmware_path.length() > 0 &&
                manifest.firmware_path != expected_path) {
                return fail(error_code, "manifest_firmware_path_invalid");
            }
            if (manifest.firmware_path.length() == 0 &&
                manifest.firmware_url.length() == 0) {
                return fail(error_code, "manifest_firmware_path_invalid");
            }

            const String url = firmware_url_from_manifest(manifest, endpoint);
            if (!is_allowed_firmware_url(url, manifest.version, endpoint)) {
                Serial.printf("[%s] firmware url not allowed: '%s' source=%s\r\n",
                    kTag, url.c_str(), endpoint_kind_string(endpoint.kind));
                return fail(error_code, "firmware_url_not_allowed");
            }

            Serial.printf("[%s] manifest validation PASSED source=%s\r\n",
                kTag, endpoint_kind_string(endpoint.kind));
            return true;
        }

    }  // namespace

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
        const String expected =
            build_base_url(endpoint) + expected_firmware_path(version);
        return url == expected;
    }

    bool fetch_manifest(const EndpointContext& endpoint,
                        OtaManifest* output,
                        String* error_code) {
        if (error_code) {
            *error_code = "";
        }
        if (!output) {
            return fail(error_code, "manifest_output_null");
        }
        if (WiFi.status() != WL_CONNECTED) {
            return fail(error_code, "wifi_not_connected");
        }

        RoutedWiFiClient plain(endpoint.transport_ip);
        RoutedWiFiClientSecure secure(
            endpoint.transport_ip, gateway::ota_config::kRootCaPem);
        HTTPClient http;

        const String url = build_check_url(endpoint);
        const uint32_t started = millis();

        if (!begin_http(http, plain, secure, endpoint, url)) {
            return fail(error_code, "manifest_http_begin_failed");
        }

        const int code = http.GET();
        const uint32_t latency = millis() - started;
        Serial.printf("[%s] manifest response source=%s code=%d latency=%lu ms\r\n",
            kTag,
            endpoint_kind_string(endpoint.kind),
            code,
            static_cast<unsigned long>(latency));

        if (code != HTTP_CODE_OK) {
            http.end();
            return fail(error_code, "manifest_http_not_ok");
        }

        const int content_length = http.getSize();
        if (content_length > static_cast<int>(gateway::ota_config::kManifestMaxBytes)) {
            http.end();
            return fail(error_code, "manifest_body_too_large");
        }

        const String body = http.getString();
        http.end();

        if (body.length() > gateway::ota_config::kManifestMaxBytes) {
            return fail(error_code, "manifest_body_too_large");
        }

        JsonDocument document;
        const DeserializationError error = deserializeJson(document, body);
        if (error) {
            Serial.printf("[%s] json parse failed: %s\r\n", kTag, error.c_str());
            return fail(error_code, "manifest_json_parse_failed");
        }

        if ((document["ok"] | false) != true) {
            return fail(error_code, "manifest_ok_false");
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

        if (!validate_manifest(*output, endpoint, error_code)) {
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

}  // namespace gateway::ota::detail
