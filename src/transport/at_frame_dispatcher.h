#pragma once

/**
 * @file at_frame_dispatcher.h
 * @brief 将 CAN 格式帧统一编码为 AT 二进制行，并非阻塞分发到 USB CDC 与 MQTT 上行。
 */

#include <cstdint>
#include <driver/twai.h>

namespace gateway::at_frame_dispatcher {

    /** USB CDC 镜像队列深度；队列满时丢弃当前 USB 镜像，不反压 CAN/ADC 实时任务。 */
    constexpr unsigned kSerialMirrorQueueDepth = 24;

    constexpr int kMirrorTxTaskCore = 0;
    constexpr uint32_t kMirrorTxTaskStackBytes = 4096;
    constexpr unsigned kMirrorTxTaskPriority = 3;

    /**
     * @brief 初始化 USB CDC 镜像队列与独立发送任务。
     *
     * `USBSerial` 须在 main 中先完成 begin；本函数可重复调用。
     */
    void begin();

    /**
     * @brief 将一帧 CAN 格式数据编码一次后，同时投递至 USB 与 MQTT。
     *
     * 调用路径不会执行 USB 写入或 MQTT publish；USB 队列与 MQTT RingBuffer
     * 均采用非阻塞投递，适合 CAN RX 与 ADC 实时任务调用。
     */
    void dispatch(const twai_message_t &msg);

} // namespace gateway::at_frame_dispatcher
