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

/** 监控任务固定在 Core 0 运行，避免因采样自耗导致 Core 1 负载误报 100%。 */
constexpr int kPinnedCore = 0;
constexpr uint32_t kTaskStackBytes = 4096;

void start();

} // namespace gateway::uart2_test_task
