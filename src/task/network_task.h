#pragma once

/**
 * @file network_task.h
 * @brief Wi-Fi、MQTT 与 SNTP 主轮询任务栈大小与启动声明。
 */

#include <cstdint>

namespace gateway::network_task {

    constexpr uint32_t kTaskStackBytes = 12288;  // 12KB, 留足 WiFi/MQTT 调用深度余量

    void start();

} // namespace gateway::network_task
