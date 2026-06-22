#include "ota/ota_firmware_installer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <ctype.h>
#include <Update.h>
#include <WiFiClient.h>

#include "config/ota_config.h"
#include "ota/ota_endpoint_planner.h"
#include "ota/ota_http_route.h"
#include "ota/ota_manifest_client.h"

#include "mbedtls/sha256.h"
#include "mbedtls/version.h"

namespace gateway::ota::detail {
    namespace {

        static constexpr const char* kTag = "OTA";

        struct Sha256Context {
            mbedtls_sha256_context ctx;
        };

        bool fail(String* error_code, const char* error) {
            if (error_code) {
                *error_code = error ? error : "";
            }
            return false;
        }

        void notify_stage(const InstallCallbacks& callbacks, InstallStage stage) {
            if (callbacks.on_stage) {
                callbacks.on_stage(stage);
            }
        }

        void notify_progress(const InstallCallbacks& callbacks, uint8_t progress) {
            if (callbacks.on_progress) {
                callbacks.on_progress(progress);
            }
        }

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

        uint8_t calculate_progress(size_t written, size_t total) {
            if (total == 0) {
                return 0;
            }

            uint32_t progress = static_cast<uint32_t>((written * 100UL) / total);
            if (progress > 100) {
                progress = 100;
            }
            return static_cast<uint8_t>(progress);
        }

    }  // namespace

    bool download_and_apply(const OtaManifest& manifest,
                            const EndpointContext& endpoint,
                            const InstallCallbacks& callbacks,
                            String* error_code) {
        if (error_code) {
            *error_code = "";
        }

        const String firmware_url = firmware_url_from_manifest(manifest, endpoint);
        if (!is_allowed_firmware_url(firmware_url, manifest.version, endpoint)) {
            return fail(error_code, "firmware_url_not_allowed");
        }

        Serial.printf("[%s] download start source=%s url=%s transport=%s\r\n",
            kTag,
            endpoint_kind_string(endpoint.kind),
            firmware_url.c_str(),
            endpoint.transport_ip.toString().c_str());

        notify_stage(callbacks, InstallStage::Downloading);
        notify_progress(callbacks, 0);

        RoutedWiFiClient plain(endpoint.transport_ip);
        RoutedWiFiClientSecure secure(
            endpoint.transport_ip, gateway::ota_config::kRootCaPem);
        HTTPClient http;

        if (!begin_http(http, plain, secure, endpoint, firmware_url)) {
            return fail(error_code, "firmware_http_begin_failed");
        }

        const uint32_t download_started = millis();
        const int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[%s] firmware http code=%d source=%s\r\n",
                kTag, code, endpoint_kind_string(endpoint.kind));
            http.end();
            return fail(error_code, "firmware_http_not_ok");
        }

        const int content_length = http.getSize();
        if (content_length <= 0 ||
            static_cast<size_t>(content_length) != manifest.size) {
            http.end();
            return fail(error_code, "firmware_size_mismatch");
        }

        if (!Update.begin(content_length, U_FLASH)) {
            Serial.printf("[%s] Update.begin failed: %s\r\n",
                kTag, Update.errorString());
            http.end();
            return fail(error_code, "update_begin_failed");
        }

        Update.setMD5(manifest.md5.c_str());

        WiFiClient* stream = http.getStreamPtr();
        if (!stream) {
            Update.abort();
            http.end();
            return fail(error_code, "firmware_stream_missing");
        }

        uint8_t buffer[gateway::ota_config::kDownloadBufferSize];
        Sha256Context sha_context;
        sha256_init(&sha_context);

        size_t written = 0;
        uint32_t last_received_ms = millis();
        uint8_t last_printed_progress = 255;

        while (http.connected() && written < static_cast<size_t>(content_length)) {
            const size_t available = stream->available();
            if (available == 0) {
                if (millis() - last_received_ms >
                    gateway::ota_config::kDownloadIdleTimeoutMs) {
                    sha256_abort(&sha_context);
                    Update.abort();
                    http.end();
                    return fail(error_code, "download_idle_timeout");
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
                http.end();
                return fail(error_code, "stream_read_failed");
            }

            last_received_ms = millis();
            sha256_update(&sha_context, buffer, static_cast<size_t>(read_length));

            const size_t write_length =
                Update.write(buffer, static_cast<size_t>(read_length));
            if (write_length != static_cast<size_t>(read_length)) {
                sha256_abort(&sha_context);
                Update.abort();
                http.end();
                return fail(error_code, "flash_write_failed");
            }

            written += write_length;
            const uint8_t progress = calculate_progress(
                written, static_cast<size_t>(content_length));
            notify_progress(callbacks, progress);

            if (progress != last_printed_progress &&
                (progress == 100 || progress % 10 == 0)) {
                last_printed_progress = progress;
                Serial.printf("[%s] download progress=%u%% (%u/%u B)\r\n",
                    kTag,
                    static_cast<unsigned>(progress),
                    static_cast<unsigned>(written),
                    static_cast<unsigned>(content_length));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (written != static_cast<size_t>(content_length)) {
            sha256_abort(&sha_context);
            Update.abort();
            http.end();
            return fail(error_code, "download_incomplete");
        }

        notify_stage(callbacks, InstallStage::Verifying);
        const String actual_sha256 = sha256_final_hex(&sha_context);
        if (!equals_ignore_case(actual_sha256, manifest.sha256)) {
            Update.abort();
            http.end();
            return fail(error_code, "sha256_mismatch");
        }

        notify_stage(callbacks, InstallStage::Applying);
        if (!Update.end(true)) {
            Serial.printf("[%s] Update.end failed: ", kTag);
            Update.printError(Serial);
            Update.abort();
            http.end();
            return fail(error_code, "update_end_failed");
        }

        if (!Update.isFinished()) {
            Update.abort();
            http.end();
            return fail(error_code, "update_not_finished");
        }

        http.end();

        const uint32_t duration_ms = millis() - download_started;
        Serial.printf("[%s] update success source=%s bytes=%u duration=%lu ms, rebooting\r\n",
            kTag,
            endpoint_kind_string(endpoint.kind),
            static_cast<unsigned>(written),
            static_cast<unsigned long>(duration_ms));

        notify_stage(callbacks, InstallStage::Rebooting);
        delay(500);
        ESP.restart();
        return true;
    }

}  // namespace gateway::ota::detail
