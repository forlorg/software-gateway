#include "system/ota_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include <stdio.h>
#include <string.h>

#include "config/ota_config.h"
#include "ota/ota_endpoint_planner.h"
#include "ota/ota_firmware_installer.h"
#include "ota/ota_manifest_client.h"
#include "ota/ota_types.h"
#include "system/version.h"

namespace gateway::ota {
    namespace {

        static constexpr const char* kTag = "OTA";

        portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

        State g_state = State::Disabled;
        bool g_check_requested = false;
        bool g_upgrade_requested = false;
        bool g_auto_first_check_done = false;
        bool g_have_manifest = false;
        bool g_update_available = false;
        uint8_t g_progress = 0;
        uint32_t g_latest_build = 0;
        char g_latest_version[kStatusVersionCapacity]{};
        char g_last_error[kStatusErrorCapacity]{};
        detail::OtaManifest g_manifest;

        uint32_t g_started_ms = 0;
        uint32_t g_last_check_ms = 0;
        uint32_t g_next_retry_ms = 0;
        uint32_t g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMinMs;

        void copy_text(char* destination, size_t capacity, const char* source) {
            if (!destination || capacity == 0) {
                return;
            }
            snprintf(destination, capacity, "%s", source ? source : "");
        }

        bool is_busy_state(State state) {
            return state == State::CheckManifest ||
                   state == State::Downloading ||
                   state == State::Verifying ||
                   state == State::Applying ||
                   state == State::Rebooting;
        }

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
            char next_error[kStatusErrorCapacity]{};
            copy_text(next_error, sizeof(next_error), error);

            portENTER_CRITICAL(&g_lock);
            memcpy(g_last_error, next_error, sizeof(g_last_error));
            portEXIT_CRITICAL(&g_lock);
            Serial.printf("[%s] error=%s\r\n", kTag, error ? error : "(clear)");
        }

        void clear_last_error() {
            portENTER_CRITICAL(&g_lock);
            g_last_error[0] = '\0';
            portEXIT_CRITICAL(&g_lock);
        }

        void set_last_check_ms(uint32_t value) {
            portENTER_CRITICAL(&g_lock);
            g_last_check_ms = value;
            portEXIT_CRITICAL(&g_lock);
        }

        bool update_available_snapshot() {
            portENTER_CRITICAL(&g_lock);
            const bool available = g_update_available;
            portEXIT_CRITICAL(&g_lock);
            return available;
        }

        void store_manifest(const detail::OtaManifest& manifest) {
            // g_manifest 仅由 OTA 任务读写，不在临界区内复制 Arduino String。
            g_manifest = manifest;
            g_have_manifest = manifest.update_available;

            char next_version[kStatusVersionCapacity]{};
            copy_text(
                next_version,
                sizeof(next_version),
                manifest.update_available ? manifest.version.c_str() : "");

            portENTER_CRITICAL(&g_lock);
            g_update_available = manifest.update_available;
            g_latest_build = manifest.update_available ? manifest.build : 0;
            memcpy(g_latest_version, next_version, sizeof(g_latest_version));
            portEXIT_CRITICAL(&g_lock);

            if (manifest.update_available) {
                Serial.printf("[%s] manifest stored source=%s version=%s build=%lu\r\n",
                    kTag,
                    manifest.source_valid
                        ? detail::endpoint_kind_string(manifest.source.kind)
                        : "unknown",
                    manifest.version.c_str(),
                    static_cast<unsigned long>(manifest.build));
            }
        }

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

        bool prepare_endpoint(detail::EndpointContext* endpoint) {
            String error;
            if (detail::prepare_endpoint(endpoint, &error)) {
                return true;
            }
            set_last_error(error.length() > 0 ? error.c_str() : "ota_transport_ip_missing");
            return false;
        }

        bool fetch_manifest_for_endpoint(
            const detail::EndpointContext& endpoint,
            detail::OtaManifest* output) {
            set_state(State::CheckManifest);
            set_progress(0);
            clear_last_error();

            String error;
            if (detail::fetch_manifest(endpoint, output, &error)) {
                return true;
            }
            set_last_error(error.length() > 0 ? error.c_str() : "manifest_http_begin_failed");
            return false;
        }

        void on_install_stage(detail::InstallStage stage) {
            switch (stage) {
            case detail::InstallStage::Downloading:
                set_state(State::Downloading);
                break;
            case detail::InstallStage::Verifying:
                set_state(State::Verifying);
                break;
            case detail::InstallStage::Applying:
                set_state(State::Applying);
                break;
            case detail::InstallStage::Rebooting:
                set_state(State::Rebooting);
                break;
            }
        }

        void on_install_progress(uint8_t progress) {
            set_progress(progress);
        }

        bool install_manifest(const detail::OtaManifest& manifest,
                              const detail::EndpointContext& endpoint) {
            const detail::InstallCallbacks callbacks{
                on_install_stage,
                on_install_progress
            };

            String error;
            if (detail::download_and_apply(manifest, endpoint, callbacks, &error)) {
                return true;
            }
            set_last_error(error.length() > 0 ? error.c_str() : "firmware_http_begin_failed");
            return false;
        }

        bool execute_candidate_plan(const detail::CandidatePlan& plan,
                                    bool upgrade_after_check) {
            for (size_t i = 0; i < plan.count; ++i) {
                detail::EndpointContext endpoint = plan.endpoints[i];
                Serial.printf("[%s] candidate attempt=%u/%u type=%s\r\n",
                    kTag,
                    static_cast<unsigned>(i + 1),
                    static_cast<unsigned>(plan.count),
                    detail::endpoint_kind_string(endpoint.kind));

                if (!prepare_endpoint(&endpoint)) {
                    detail::handle_candidate_failure(endpoint, "prepare");
                    continue;
                }

                detail::OtaManifest manifest;
                const bool check_ok = fetch_manifest_for_endpoint(endpoint, &manifest);
                set_last_check_ms(millis());

                if (!check_ok) {
                    detail::handle_candidate_failure(endpoint, "check");
                    continue;
                }

                // 域名 check 成功（包括无更新）后保存本次实际使用的 IPv4。
                if (endpoint.kind == detail::EndpointKind::Domain) {
                    detail::save_domain_cache(endpoint);
                }

                reset_backoff();

                if (!manifest.update_available) {
                    store_manifest(manifest);
                    set_state(State::UpToDate);
                    Serial.printf("[%s] firmware is up-to-date source=%s\r\n",
                        kTag, detail::endpoint_kind_string(endpoint.kind));
                    return true;
                }

                store_manifest(manifest);
                set_state(State::UpdateAvailable);

                const bool should_upgrade = upgrade_after_check || manifest.mandatory;
                if (!should_upgrade) {
                    Serial.printf(
                        "[%s] update available, waiting for upgrade trigger\r\n", kTag);
                    return true;
                }

                if (manifest.mandatory) {
                    Serial.printf("[%s] mandatory update, auto-upgrading\r\n", kTag);
                }

                if (install_manifest(manifest, endpoint)) {
                    return true;
                }

                // 下载失败后不得跨端点复用 manifest；下一候选必须重新 check。
                detail::handle_candidate_failure(endpoint, "download");
                Serial.printf(
                    "[%s] download failed, next endpoint must re-check\r\n", kTag);
            }

            Serial.printf("[%s] all OTA candidates exhausted\r\n", kTag);
            return false;
        }

        void do_check(bool upgrade_after_check) {
            Serial.printf("[%s] do_check upgrade_after_check=%d\r\n",
                kTag, static_cast<int>(upgrade_after_check));

            const detail::CandidatePlan plan = detail::build_candidate_plan(0);
            if (!execute_candidate_plan(plan, upgrade_after_check)) {
                enter_backoff();
            }
        }

        void upgrade_cached_manifest() {
            const detail::OtaManifest manifest = g_manifest;
            if (!manifest.source_valid) {
                Serial.printf("[%s] cached manifest source missing, re-checking\r\n", kTag);
                do_check(true);
                return;
            }

            Serial.printf("[%s] using cached manifest version=%s source=%s\r\n",
                kTag,
                manifest.version.c_str(),
                detail::endpoint_kind_string(manifest.source.kind));

            if (install_manifest(manifest, manifest.source)) {
                return;
            }

            detail::handle_candidate_failure(manifest.source, "download");

            const uint8_t next_rank = detail::endpoint_rank(manifest.source.kind) + 1;
            const detail::CandidatePlan fallback_plan =
                detail::build_candidate_plan(next_rank);
            if (fallback_plan.count == 0 ||
                !execute_candidate_plan(fallback_plan, true)) {
                enter_backoff();
            }
        }

    }  // namespace

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

        const detail::EndpointContext primary =
            detail::primary_endpoint(detail::EndpointKind::Domain);
        const detail::EndpointContext lan = detail::lan_endpoint();
        Serial.printf("[%s] primary    : %s\r\n",
            kTag, detail::build_base_url(primary).c_str());
        Serial.printf("[%s] lan backup : %s\r\n",
            kTag, detail::build_base_url(lan).c_str());
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

        detail::ota_partition_size();
        Serial.printf("[%s] === OTA module ready ===\r\n", kTag);
    }

    void loop() {
        State current;
        bool check_requested = false;
        bool upgrade_requested = false;
        uint32_t last_check_ms = 0;

        portENTER_CRITICAL(&g_lock);
        current = g_state;
        check_requested = g_check_requested;
        upgrade_requested = g_upgrade_requested;
        last_check_ms = g_last_check_ms;
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
        } else if (last_check_ms != 0 &&
                   now - last_check_ms >= gateway::ota_config::kCheckIntervalMs) {
            auto_due = true;
            Serial.printf("[%s] periodic auto-check triggered\r\n", kTag);
        }

        if (upgrade_requested) {
            Serial.printf("[%s] manual upgrade requested\r\n", kTag);
            if (g_have_manifest && update_available_snapshot()) {
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
            set_state(update_available_snapshot() ? State::UpdateAvailable : State::Idle);
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
        Status snapshot{};
        snapshot.current_build = gateway::version::firmware_build();
        copy_text(
            snapshot.current_version,
            sizeof(snapshot.current_version),
            gateway::version::firmware_version());

        portENTER_CRITICAL(&g_lock);
        snapshot.state = g_state;
        snapshot.update_available = g_update_available;
        snapshot.update_in_progress = is_busy_state(g_state);
        snapshot.progress = g_progress;
        snapshot.latest_build = g_latest_build;
        memcpy(
            snapshot.latest_version,
            g_latest_version,
            sizeof(snapshot.latest_version));
        memcpy(snapshot.last_error, g_last_error, sizeof(snapshot.last_error));
        portEXIT_CRITICAL(&g_lock);
        return snapshot;
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
