#pragma once

/**
 * @file can_tx.h
 * @brief CAN 发送队列与任务初始化、两级优先级入队接口。
 */

#include <driver/twai.h>

namespace gateway::can_tx {

    void init();

    /**
     * @brief 普通优先级 CAN 发送入队。
     *
     * 用于云端下发命令、HTTP/调试/标定命令等允许稍晚发送的帧。
     * 队列满时最多短等 20ms。
     */
    bool enqueue(const twai_message_t &msg);

    /**
     * @brief 高优先级 CAN 发送入队。
     *
     * 用于 ADC 实时采样产生的 PGN 0x1708 等实时帧。
     * 队列满时最多短等 1ms，避免采样任务被 CAN TX 反压长时间拖住。
     */
    bool enqueue_high(const twai_message_t &msg);

    /**
     * @brief 云端下发 CAN 命令入队。
     *
     * 云端命令数据量很小、实时性不强；不阻塞 MQTT 回调。队列满则返回 false，调用方统计 drop。
     */
    bool enqueue_cloud_command(const twai_message_t &msg);

    /**
     * @brief 发送单个离合器起步点标定值。
     *
     * @param clutch  离合器标识："high"/"low"/"rev"/"pto"
     * @param value_str 用户输入的物理值字符串（如 "2.5"），内部做 scaling→raw 换算
     * @return true  入队成功
     * @return false 参数非法或入队失败
     */
    bool send_clutch_startup(const char *clutch, const char *value_str);

    /**
     * @brief 发送 FLASH 刷写命令到 TCU（PGN 0x1850）。
     *
     * 向 CAN 总线发送固定帧：data[0]=0x10（FLASH 刷写 bit 置位），其余字节为 0。
     *
     * @return true  入队成功
     * @return false 入队失败
     */
    bool flash_firmware();

} // namespace gateway::can_tx
