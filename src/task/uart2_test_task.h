#pragma once

/**
 * @file uart2_test_task.h
 * @brief 临时：UART2 周期发测试帧，收包经 USB `Serial` 打日志；完成后可从 `main` 去掉 `start()`。
 */

#include <cstdint>

namespace gateway::uart2_test_task {

/** `Serial2` RX / TX（ESP32-S3，勿与 CAN 4/5、镜像串口 6/7 冲突）。 */
constexpr int kUart2RxPin = 18;
constexpr int kUart2TxPin = 17;
constexpr uint32_t kBaud = 230400;
constexpr uint32_t kSendPeriodMs = 1000;

constexpr int kPinnedCore = 1;
constexpr uint32_t kTaskStackBytes = 4096;

void start();

} // namespace gateway::uart2_test_task
