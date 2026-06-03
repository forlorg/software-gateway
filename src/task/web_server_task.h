#pragma once

/**
 * @file web_server_task.h
 * @brief Web 服务任务栈、绑核与轮询周期配置（与 LwIP 同核约束相关）。
 */

#include <cstdint>

namespace gateway::web_server_task {

constexpr uint32_t kHeapWarnMinFreeBytes = 30 * 1024;

constexpr uint32_t kTaskStackBytes = 8192;

/**
 * Arduino-ESP32 下 LwIP/tcpip 与同步 `WebServer::handleClient()` 宜与 `loopTask` 同核（ESP32‑S3 常见为 **1**），
 * 否则可能触发 `tcpip_send_msg_wait_sem ... Invalid mbox`。`wifi_ap_task` 仍可留在 Core0。
 */
constexpr int kPinnedCpuCoreIndex = 0;

constexpr uint32_t kLoopDelayMs = 40;
constexpr uint32_t kSsePumpThrottleMs = 100;
constexpr uint32_t kHeapCheckIntervalMs = 2000;

void start();

} // namespace gateway::web_server_task
