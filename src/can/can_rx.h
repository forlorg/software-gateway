#pragma once

/**
 * @file can_rx.h
 * @brief CAN 接收任务参数以及启动接收任务的全局入口声明。
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

} // namespace gateway::can_rx

/** 初始化 USB CDC 镜像队列与发送任务；`USBSerial` 须在 `main` 中先 `begin`。须在 `can_rx_start_task` 之前调用。 */
void init_serial_mirror();

void can_rx_start_task();
