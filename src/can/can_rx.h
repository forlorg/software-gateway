#pragma once

/**
 * @file can_rx.h
 * @brief CAN 接收任务参数、product 宣告 ID 规则，以及启动接收任务的全局入口声明。
 */

#include <cstdint>

namespace gateway::can_rx {

/** USB CDC 镜像队列深度；若 `serial_mirror_queue_drops` 持续增长可加大。 */
constexpr unsigned kSerialMirrorQueueDepth = 24;

constexpr int kMirrorTxTaskCore = 0;
constexpr uint32_t kMirrorTxTaskStackBytes = 4096;
constexpr unsigned kMirrorTxTaskPriority = 3;

constexpr int kTaskCore = 1;
constexpr uint32_t kTaskStackBytes = 6144;
constexpr unsigned kTaskPriority = 4;

/** 总线 product_id 宣告帧参考扩展 ID（与方案文档一致）。 */
constexpr uint32_t kProductAnnounceCanIdRef = 0x16174B03u;

/** 匹配规则：`(extended_id29 & 0xFFFFFF) == kProductAnnounceIdMatchLow24`。 */
constexpr uint32_t kProductAnnounceIdMatchLow24 = kProductAnnounceCanIdRef & 0x00FFFFFFu;

constexpr bool extended_id_is_product_announce(uint32_t extended_id29) {
  return (extended_id29 & 0x00FFFFFFu) == kProductAnnounceIdMatchLow24;
}

} // namespace gateway::can_rx

/** 初始化 USB CDC 镜像队列与发送任务；`USBSerial` 须在 `main` 中先 `begin`。须在 `can_rx_start_task` 之前调用。 */
void init_serial_mirror();

void can_rx_start_task();
