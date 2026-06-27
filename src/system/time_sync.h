#pragma once

/**
 * @file time_sync.h
 * @brief 一次性 SNTP UTC 校时、IP 时区解析，以及 AT 时间戳打包 API。
 */

#include <cstdint>

namespace gateway::time_sync {

    void begin();

    /** 在网络任务里周期调用：STA 联网后启动一次 SNTP，并尝试一次公网 IP 时区解析。 */
    void loop_poll();

    /** 已完成本次启动后的首次 NTP 校时。 */
    bool ntp_has_sync();

    /** 时区已就绪：公网 IP 解析成功，或解析失败后已回退默认东八区。 */
    bool timezone_is_ready();

    /** 当前时区相对 UTC 的整点小时偏移，例如东八区为 +8。未就绪时返回默认 +8。 */
    int timezone_offset_hours();

    /** AT 高 5 位时区编码：offset_hours + 12。东八区为 20。 */
    uint8_t timezone_bits();

    /** 时区来源：ip-api、default_utc_plus_8、pending 等。 */
    const char *timezone_source();

    /** UNIX UTC 纪元秒：未校时则为 0。 */
    uint64_t utc_epoch_seconds();

    /** 将当前墙钟时间写入 ISO8601 字符串；未校时则写入 no_sync。 */
    void format_wall_time_iso8601(char *buf, unsigned cap);

    /**
     * AT 墙钟时间戳：高 5bit=offset_hours+12，低 27bit=本地当天毫秒数。
     * 只有 NTP 已同步且时区已就绪时才返回非 0，否则返回 0。
     */
    uint32_t pack_mqtt_wall_timestamp();

    /**
     * 无墙钟时 AT 行时间戳：高 5bit=31，低 27bit=上电以来 ms 的 27 位回绕值。
     */
    uint32_t pack_boot_relative_ms27();

    /** 仅当 NTP 已同步且时区已就绪时，允许使用墙钟时间戳并允许 MQTT 上行。 */
    bool can_use_wall_timestamp_for_upload();

} // namespace gateway::time_sync
