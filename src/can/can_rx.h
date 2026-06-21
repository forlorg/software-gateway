#pragma once

/**
 * @file can_rx.h
 * @brief CAN 接收任务参数以及启动入口声明。
 */

#include <cstdint>

namespace gateway::can_rx {

    constexpr int kTaskCore = 1;
    constexpr uint32_t kTaskStackBytes = 6144;
    constexpr unsigned kTaskPriority = 5;

} // namespace gateway::can_rx

void can_rx_start_task();
