#pragma once

/**
 * @file time_sync.h
 * @brief SNTP 校时、ISO8601 本地时间字符串与 MQTT 时间戳打包 API。
 */

#include <cstdint>

namespace gateway::time_sync {

    void begin();

    /** 在 WiFi/NTP 同任务里周期调用（与 esp32 全功能机在 `loop` 中一致）。 */
    void loop_poll();

    /** 已成功 NTP 校时（≥1 次同步回调）。 */
    bool ntp_has_sync();

    /** UNIX 纪元秒：未校时或未联网则为 0。 */
    uint64_t utc_epoch_seconds();

    /** 将当前本地墙上时间写入缓冲区（CST 等与 `TZ` 一致）；无有效时钟则写入 `no_sync`。 */
    void format_wall_time_iso8601(char *buf, unsigned cap);

    /** 沿用 esp32 工程中用于 MQTT 的编码（网关若不用 MQTT，仍可供扩展）。 */
    uint32_t pack_mqtt_wall_timestamp();

    /**
     * 无墙钟时 AT 行时间戳：高 5bit=31，低 27bit=上电以来 ms（饱和到 0x07FFFFFF）。
     * 与 `pack_mqtt_wall_timestamp` 中高 5bit 0~26（时区小时）区分。
     */
    uint32_t pack_boot_relative_ms27();

    /** NTP 未就绪前为 false，与 `software_esp32s3` CAN 上行丢弃规则一致。 */
    bool can_use_wall_timestamp_for_upload();

} // namespace gateway::time_sync
