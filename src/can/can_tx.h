#pragma once

/**
 * @file can_tx.h
 * @brief CAN 发送队列与任务初始化、入队接口。
 */

#include <driver/twai.h>

namespace gateway::can_tx {

void init();
bool enqueue(const twai_message_t &msg);

} // namespace gateway::can_tx
