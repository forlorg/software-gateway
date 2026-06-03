#pragma once

/**
 * @file wifi_ap_task.h
 * @brief Wi-Fi / MQTT 主轮询任务栈大小与启动声明。
 */

#include <cstdint>

namespace gateway::wifi_ap_task {

constexpr uint32_t kTaskStackBytes = 9216;

constexpr uint32_t kPlaceholderWakePeriodMs = 5000;

void start();

} // namespace gateway::wifi_ap_task
