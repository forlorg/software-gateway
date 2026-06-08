#pragma once

/**
 * @file can_tx.h
 * @brief CAN 发送队列与任务初始化、入队接口。
 */

#include <driver/twai.h>

namespace gateway::can_tx {

void init();
bool enqueue(const twai_message_t &msg);

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
