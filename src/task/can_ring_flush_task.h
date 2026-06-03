#pragma once

/**
 * @file can_ring_flush_task.h
 * @brief 无 MQTT 时 RingBuffer 消费与聚合统计任务参数；与 `mqtt_uplink::config` 保持同步。
 */

#include <cstddef>
#include <cstdint>

/**
 * CAN 上行：无 MQTT 时 RingBuffer 消费与聚合统计（不对外发送）。
 * 缓冲与周期与 `mqtt_uplink::config` 一致，修改时请两边同步。
 */
namespace gateway::can_ring_flush {

namespace config {
constexpr size_t kRingBufferBytes = 48 * 1024;
constexpr uint32_t kAggregateMsNormal = 500;
constexpr uint32_t kAggregateMsFast = 200;
constexpr size_t kHighWaterBytes = 20 * 1024;
constexpr size_t kMaxBatchBytes = 4096;
constexpr int kTaskCore = 1;
constexpr uint32_t kTaskStackBytes = 8192;
constexpr unsigned kTaskPriority = 2;
} // namespace config

void begin();

bool offer_at_binary(const uint8_t *line, uint16_t len);

uint32_t ring_used_bytes();

} // namespace gateway::can_ring_flush
