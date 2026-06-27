#pragma once

/**
 * @file usb_at_downlink_task.h
 * @brief USB CDC 下行 AT Packet 接收任务。
 */

#include <cstdint>

namespace gateway::usb_at_downlink_task {

    constexpr int kTaskCore = 0;
    constexpr unsigned kTaskPriority = 3;
    // FreeRTOS 的栈深度参数以“字”为单位；8192 words 约等于 32 KB。
    constexpr uint32_t kTaskStackBytes = 8192;

    void start();

    /** 最近一次成功解析的 AT Packet CAN 通道号；仅 RAM 暂存，当前不参与 CAN 发送路由。 */
    bool has_last_channel_no();
    uint8_t last_channel_no();

} // namespace gateway::usb_at_downlink_task
