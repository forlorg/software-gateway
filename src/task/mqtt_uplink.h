#pragma once

/**
 * @file mqtt_uplink.h
 * @brief MQTT 车辆上行：环形缓冲、聚合周期与批发布参数声明（实现见同目录 .cpp）。
 */

#include <cstddef>
#include <cstdint>

namespace gateway::mqtt_uplink {

    /** Ring 聚合与 MQTT 上行批大小（与 `can_ring_flush::config` 数值保持一致）。 */
    namespace config {
        constexpr size_t kRingBufferBytes = 48 * 1024;
        constexpr uint32_t kAggregateMsNormal = 500;
        constexpr uint32_t kAggregateMsFast = 200;
        constexpr size_t kHighWaterBytes = 20 * 1024;
        constexpr size_t kMaxBatchBytes = 4096;
    } // namespace config

    void begin();
    /** 送入一条完整 AT...\r\n 行（Binary）。按规格：首次 MQTT 成功前不入队→丢弃返回 false */
    bool offer_at_binary(const uint8_t *line, uint16_t len);

    uint32_t ring_used_bytes();

} // namespace gateway::mqtt_uplink
