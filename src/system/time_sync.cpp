/**
 * @file time_sync.cpp
 * @brief 一次性 SNTP UTC 校时、IP 时区解析，以及 AT 时间戳打包。
 */

#include "time_sync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstring>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <sys/time.h>
#include <time.h>

namespace gateway::time_sync {

    namespace {
        constexpr uint32_t LOW27_MASK = 0x07FFFFFFu;
        constexpr uint64_t kUnixValidAfterSec = 1700000000ULL;  // 2023-11-14 之后视为有效墙钟
        constexpr int kDefaultOffsetHours = 8;
        constexpr int kMinOffsetHours = -12;
        constexpr int kMaxOffsetHours = 14;
        constexpr int64_t kMsPerHour = 3600000LL;
        constexpr int64_t kMsPerDay = 86400000LL;
        constexpr uint32_t kHttpTimeoutMs = 3000;
        constexpr const char *kTimezoneApiUrl =
            "http://ip-api.com/json/?fields=status,message,countryCode,timezone,offset,query";

        portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

        bool g_ntp_synced = false;
        bool g_sntp_started = false;
        bool g_sntp_stop_pending = false;
        uint64_t g_sync_utc_ms = 0;
        uint64_t g_sync_boot_ms = 0;

        bool g_timezone_attempted = false;
        bool g_timezone_ready = false;
        int g_timezone_offset_hours = kDefaultOffsetHours;
        char g_timezone_source[64] = "pending";

        uint64_t boot_ms_now() {
            const int64_t us = esp_timer_get_time();
            return us > 0 ? static_cast<uint64_t>(us / 1000) : 0ULL;
        }

        uint8_t pack_timezone_bits(int offset_hours) {
            if (offset_hours < kMinOffsetHours) {
                offset_hours = kMinOffsetHours;
            }
            if (offset_hours > kMaxOffsetHours) {
                offset_hours = kMaxOffsetHours;
            }
            return static_cast<uint8_t>(offset_hours + 12);
        }

        int clamp_offset_hours(int offset_hours) {
            if (offset_hours < kMinOffsetHours) {
                return kMinOffsetHours;
            }
            if (offset_hours > kMaxOffsetHours) {
                return kMaxOffsetHours;
            }
            return offset_hours;
        }

        uint32_t positive_mod_day_ms(int64_t local_ms) {
            int64_t v = local_ms % kMsPerDay;
            if (v < 0) {
                v += kMsPerDay;
            }
            return static_cast<uint32_t>(v);
        }

        void set_timezone_locked(int offset_hours, const char *source) {
            g_timezone_offset_hours = clamp_offset_hours(offset_hours);
            if (source && source[0]) {
                snprintf(g_timezone_source, sizeof(g_timezone_source), "%s", source);
            } else {
                snprintf(g_timezone_source, sizeof(g_timezone_source), "%s", "unknown");
            }
            g_timezone_ready = true;
        }

        void use_default_timezone(const char *reason) {
            char src[64]{};
            if (reason && reason[0]) {
                snprintf(src, sizeof(src), "default_utc_plus_8:%s", reason);
            } else {
                snprintf(src, sizeof(src), "%s", "default_utc_plus_8");
            }

            portENTER_CRITICAL(&g_lock);
            set_timezone_locked(kDefaultOffsetHours, src);
            portEXIT_CRITICAL(&g_lock);

            Serial.printf("[TIME] timezone fallback offset=+8 tzBits=20 reason=%s\r\n",
                reason ? reason : "unknown");
        }

        bool resolve_timezone_by_public_ip_once() {
            WiFiClient client;
            HTTPClient http;
            http.setTimeout(kHttpTimeoutMs);
            http.setReuse(false);

            if (!http.begin(client, kTimezoneApiUrl)) {
                return false;
            }

            const int code = http.GET();
            if (code != HTTP_CODE_OK) {
                Serial.printf("[TIME] timezone HTTP failed code=%d\r\n", code);
                http.end();
                return false;
            }

            const String body = http.getString();
            http.end();

            JsonDocument doc;
            const DeserializationError err = deserializeJson(doc, body);
            if (err) {
                Serial.printf("[TIME] timezone JSON parse failed: %s\r\n", err.c_str());
                return false;
            }

            const char *status = doc["status"] | "";
            if (strcmp(status, "success") != 0) {
                const char *message = doc["message"] | "unknown";
                Serial.printf("[TIME] timezone API status=%s message=%s\r\n", status, message);
                return false;
            }

            if (!doc["offset"].is<int>()) {
                Serial.print("[TIME] timezone API missing offset\r\n");
                return false;
            }

            const int offset_seconds = doc["offset"].as<int>();
            int offset_hours = offset_seconds / 3600;  // AT 格式只支持整点小时；非整点时区按整数小时截断。
            offset_hours = clamp_offset_hours(offset_hours);

            const char *query = doc["query"] | "";
            const char *country = doc["countryCode"] | "";
            const char *tz = doc["timezone"] | "";

            char src[64]{};
            snprintf(src, sizeof(src), "ip-api:%s:%s:%s", country, tz, query);

            portENTER_CRITICAL(&g_lock);
            set_timezone_locked(offset_hours, src);
            portEXIT_CRITICAL(&g_lock);

            Serial.printf("[TIME] timezone resolved offset=%+d tzBits=%u source=%s\r\n",
                offset_hours,
                static_cast<unsigned>(pack_timezone_bits(offset_hours)),
                src);
            return true;
        }

        void maybe_resolve_timezone_once() {
            const bool wifi_connected = WiFi.isConnected();
            bool already_done = false;
            portENTER_CRITICAL(&g_lock);
            already_done = g_timezone_attempted || g_timezone_ready;
            if (!already_done && wifi_connected) {
                g_timezone_attempted = true;
            }
            portEXIT_CRITICAL(&g_lock);

            if (already_done || !wifi_connected) {
                return;
            }

            if (!resolve_timezone_by_public_ip_once()) {
                use_default_timezone("ip_lookup_failed");
            }
        }

        void sntp_sync_cb(struct timeval *tv) {
            struct timeval now_tv{};
            if (!tv) {
                gettimeofday(&now_tv, nullptr);
                tv = &now_tv;
            }

            const uint64_t utc_ms =
                static_cast<uint64_t>(tv->tv_sec) * 1000ULL +
                static_cast<uint64_t>(tv->tv_usec / 1000);
            const uint64_t boot_ms = boot_ms_now();

            portENTER_CRITICAL(&g_lock);
            if (!g_ntp_synced && tv->tv_sec > static_cast<time_t>(kUnixValidAfterSec)) {
                g_sync_utc_ms = utc_ms;
                g_sync_boot_ms = boot_ms;
                g_ntp_synced = true;
                g_sntp_stop_pending = true;
            }
            portEXIT_CRITICAL(&g_lock);
        }

        void maybe_start_sntp_once() {
            const bool wifi_connected = WiFi.isConnected();
            bool should_start = false;
            portENTER_CRITICAL(&g_lock);
            should_start = !g_ntp_synced && !g_sntp_started && wifi_connected;
            if (should_start) {
                g_sntp_started = true;
            }
            portEXIT_CRITICAL(&g_lock);

            if (!should_start) {
                return;
            }

            // 系统 time_t 维持 UTC；AT 本地时间戳使用单独的 offset_hours 计算，不依赖 TZ/localtime。
            setenv("TZ", "UTC0", 1);
            tzset();

            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
            // 本次启动只使用首次同步结果；同步成功后会停止 SNTP，此处间隔仅作兜底。
            esp_sntp_set_sync_interval(3600000);
            esp_sntp_init();

            Serial.print("[TIME] SNTP started, waiting for first UTC sync\r\n");
        }

        void maybe_stop_sntp_after_first_sync() {
            bool should_stop = false;
            uint64_t sync_utc_ms = 0;
            uint64_t sync_boot_ms = 0;

            portENTER_CRITICAL(&g_lock);
            should_stop = g_sntp_stop_pending;
            if (should_stop) {
                g_sntp_stop_pending = false;
                sync_utc_ms = g_sync_utc_ms;
                sync_boot_ms = g_sync_boot_ms;
            }
            portEXIT_CRITICAL(&g_lock);

            if (!should_stop) {
                return;
            }

            esp_sntp_stop();
            Serial.printf("[TIME] first UTC sync captured utc_ms=%llu boot_ms=%llu; SNTP stopped\r\n",
                static_cast<unsigned long long>(sync_utc_ms),
                static_cast<unsigned long long>(sync_boot_ms));
        }

        bool current_utc_ms(uint64_t &out_ms) {
            bool synced = false;
            uint64_t sync_utc_ms = 0;
            uint64_t sync_boot_ms = 0;

            portENTER_CRITICAL(&g_lock);
            synced = g_ntp_synced;
            sync_utc_ms = g_sync_utc_ms;
            sync_boot_ms = g_sync_boot_ms;
            portEXIT_CRITICAL(&g_lock);

            if (!synced || sync_utc_ms == 0) {
                return false;
            }

            const uint64_t now_boot_ms = boot_ms_now();
            const uint64_t elapsed_ms =
                (now_boot_ms >= sync_boot_ms) ? (now_boot_ms - sync_boot_ms) : 0ULL;
            out_ms = sync_utc_ms + elapsed_ms;
            return true;
        }

        bool read_timezone(int &offset_hours, uint8_t &bits) {
            bool ready = false;
            int offset = kDefaultOffsetHours;

            portENTER_CRITICAL(&g_lock);
            ready = g_timezone_ready;
            offset = g_timezone_offset_hours;
            portEXIT_CRITICAL(&g_lock);

            offset_hours = clamp_offset_hours(offset);
            bits = pack_timezone_bits(offset_hours);
            return ready;
        }

    } // namespace

    void begin() {
        maybe_start_sntp_once();
        maybe_resolve_timezone_once();
        maybe_stop_sntp_after_first_sync();
    }

    void loop_poll() {
        maybe_start_sntp_once();
        maybe_resolve_timezone_once();
        maybe_stop_sntp_after_first_sync();
    }

    bool ntp_has_sync() {
        portENTER_CRITICAL(&g_lock);
        const bool v = g_ntp_synced;
        portEXIT_CRITICAL(&g_lock);
        return v;
    }

    bool timezone_is_ready() {
        portENTER_CRITICAL(&g_lock);
        const bool v = g_timezone_ready;
        portEXIT_CRITICAL(&g_lock);
        return v;
    }

    int timezone_offset_hours() {
        int offset = kDefaultOffsetHours;
        portENTER_CRITICAL(&g_lock);
        offset = g_timezone_offset_hours;
        portEXIT_CRITICAL(&g_lock);
        return clamp_offset_hours(offset);
    }

    uint8_t timezone_bits() {
        return pack_timezone_bits(timezone_offset_hours());
    }

    const char *timezone_source() {
        return g_timezone_source;
    }

    uint64_t utc_epoch_seconds() {
        uint64_t utc_ms = 0;
        if (!current_utc_ms(utc_ms)) {
            return 0;
        }
        return utc_ms / 1000ULL;
    }

    void format_wall_time_iso8601(char *buf, unsigned cap) {
        if (!buf || cap == 0) {
            return;
        }

        uint64_t utc_ms = 0;
        if (!current_utc_ms(utc_ms)) {
            snprintf(buf, cap, "%s", "no_sync");
            return;
        }

        int offset_hours = 0;
        uint8_t bits = 0;
        const bool tz_ready = read_timezone(offset_hours, bits);
        if (!tz_ready) {
            offset_hours = 0;
        }

        const int64_t local_sec =
            static_cast<int64_t>(utc_ms / 1000ULL) +
            static_cast<int64_t>(offset_hours) * 3600LL;

        time_t t = static_cast<time_t>(local_sec);
        struct tm tmv{};
        gmtime_r(&t, &tmv);

        const char sign = offset_hours >= 0 ? '+' : '-';
        const int abs_offset = offset_hours >= 0 ? offset_hours : -offset_hours;
        snprintf(buf,
                 cap,
                 "%04d-%02d-%02dT%02d:%02d:%02d%c%02d00",
                 tmv.tm_year + 1900,
                 tmv.tm_mon + 1,
                 tmv.tm_mday,
                 tmv.tm_hour,
                 tmv.tm_min,
                 tmv.tm_sec,
                 sign,
                 abs_offset);
    }

    uint32_t pack_mqtt_wall_timestamp() {
        uint64_t utc_ms = 0;
        if (!current_utc_ms(utc_ms)) {
            return 0;
        }

        int offset_hours = 0;
        uint8_t tz_bits = 0;
        if (!read_timezone(offset_hours, tz_bits)) {
            return 0;
        }

        const int64_t local_ms =
            static_cast<int64_t>(utc_ms) +
            static_cast<int64_t>(offset_hours) * kMsPerHour;
        const uint32_t low27 = positive_mod_day_ms(local_ms) & LOW27_MASK;
        const uint32_t high5 = static_cast<uint32_t>(tz_bits) << 27;
        return high5 | low27;
    }

    uint32_t pack_boot_relative_ms27() {
        constexpr uint32_t kHigh31 = 31u << 27;
        const uint32_t low27 = static_cast<uint32_t>(boot_ms_now()) & LOW27_MASK;
        return kHigh31 | low27;
    }

    bool can_use_wall_timestamp_for_upload() {
        return ntp_has_sync() && timezone_is_ready();
    }

} // namespace gateway::time_sync
