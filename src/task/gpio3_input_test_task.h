#pragma once

/**
 * @file gpio3_input_test_task.h
 * @brief GPIO3 外部高低电平选择器测试任务。
 */

#include <cstdint>

namespace gateway::gpio3_input_test_task {

constexpr int kInputPin = 3;
constexpr int kPinnedCore = 1;
constexpr uint32_t kTaskStackBytes = 3072;
constexpr uint32_t kPollPeriodMs = 20;
constexpr uint32_t kPeriodicPrintMs = 5000;
constexpr uint8_t kStableSampleCount = 3;

void start();

}  // namespace gateway::gpio3_input_test_task
