/**
 * @file time_sync.cpp
 * @brief NTP 校时、墙上时间格式化，以及 MQTT 时间戳打包（与 TZ 环境变量一致）。
 */

#include "time_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cmath>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

namespace gateway::time_sync {

    namespace {
        volatile bool g_first_sync = false;
        bool g_sntp_started = false;

        /** SNTP 完成一次同步后的回调，用于标记墙钟可用。 */
        void sntp_sync_cb(struct timeval *) { g_first_sync = true; }

        /**
         * 从 `gettimeofday` 的「相对格林威治以西分钟数」换算为「相对 UTC 的整点小时」，
         * 与 `tzset()` / `setenv("TZ", …)` 后的系统时区一致（本工具链无 `tm_gmtoff`）。
         * 非整点时区按整数小时截断；调用失败时回退 8（与默认 CST-8 一致）。
         */
        int wall_offset_hours_from_system_tz() {
            struct timeval tv {};
            struct timezone tz {};
            if (gettimeofday(&tv, &tz) != 0) {
                return 8;
            }
            const int minutes_west = tz.tz_minuteswest;
            int hours = -minutes_west / 60;
            if (hours < -12) {
                hours = -12;
            }
            if (hours > 14) {
                hours = 14;
            }
            return hours;
        }

        /** STA 已连接时启动 SNTP，并设置 TZ（与 `format_wall_time_iso8601` / 本地 mktime 一致）。 */
        void maybe_start_sntp() {
            if (g_sntp_started || !WiFi.isConnected()) {
                return;
            }
            setenv("TZ", "CST-8", 1);
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
            esp_sntp_set_sync_interval(3600000);
            esp_sntp_init();
            g_sntp_started = true;
        }
    } // namespace

    void begin() { maybe_start_sntp(); }

    void loop_poll() { maybe_start_sntp(); }

    bool ntp_has_sync() { return g_first_sync; }

    uint64_t utc_epoch_seconds() {
        if (!g_first_sync) {
            return 0;
        }
        const time_t now = time(nullptr);
        if (now < 1577836800) {
            return 0;
        }
        return static_cast<uint64_t>(now);
    }

    void format_wall_time_iso8601(char *buf, unsigned cap) {
        if (!buf || cap == 0) {
            return;
        }
        if (!g_first_sync) {
            snprintf(buf, cap, "%s", "no_sync");
            return;
        }
        time_t now = time(nullptr);
        if (now < 1577836800) {
            snprintf(buf, cap, "%s", "no_sync");
            return;
        }
        struct tm lt {};
        localtime_r(&now, &lt);
        strftime(buf, cap, "%Y-%m-%dT%H:%M:%S%z", &lt);
    }

    /**
     * 打包 MQTT 用墙钟时间戳：低 27 位为「本地时区」当日 0 点起的毫秒；高 5 位为相对 UTC 的整点小时
     *（来自 `gettimeofday` 的时区，与 `setenv("TZ",…)` + `tzset()` 一致）。
     */
    uint32_t pack_mqtt_wall_timestamp() {
        time_t nowt = time(nullptr);
        if (!g_first_sync || nowt < 1700000000) {
            return 0;
        }

        struct tm local_tm {};
        localtime_r(&nowt, &local_tm);

        struct tm sod = local_tm;
        sod.tm_hour = 0;
        sod.tm_min = 0;
        sod.tm_sec = 0;

        time_t t_sod = mktime(&sod);

        int64_t ms =
        (static_cast<int64_t>(nowt) - static_cast<int64_t>(t_sod)) * 1000LL;

        if (ms < 0) {
            ms = 0;
        }

        if (ms > static_cast<int64_t>(0x07FFFFFF)) {
            ms = 0x07FFFFFF;
        }

        constexpr uint32_t LOW27_MASK = 0x07FFFFFFu;

        const int offset_hours = wall_offset_hours_from_system_tz();

        uint32_t low27 = static_cast<uint32_t>(ms) & LOW27_MASK;
        // 高 5bit: 0~26 表示 UTC-12~UTC+14；31 与 `pack_boot_relative_ms27()` 共用为「上电相对 ms」
        uint32_t high5 = static_cast<uint32_t>(offset_hours + 12) << 27;

        return high5 | low27;
    }

    uint32_t pack_boot_relative_ms27() {
        constexpr uint32_t kHigh31 = 31u << 27;
        constexpr uint32_t LOW27_MASK = 0x07FFFFFFu;
        const uint32_t ms = millis();
        const uint32_t low27 = (ms > LOW27_MASK) ? LOW27_MASK : ms;
        return kHigh31 | low27;
    }

    bool can_use_wall_timestamp_for_upload() {
        return g_first_sync && (time(nullptr) > 1700000000);
    }

} // namespace gateway::time_sync
