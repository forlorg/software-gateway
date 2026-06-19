#pragma once
/**
 * @file adc_pressure_can_task.h
 * @brief ADS7924 压力采样并通过 CAN PGN 0x1708 发送的正式任务。
 */
#include <cstdint>

namespace gateway::adc_pressure_can_task {

constexpr int kGpio3InputPin = 3;
constexpr int kPinnedCore = 1;
constexpr uint32_t kTaskStackBytes = 6144;
constexpr uint32_t kTaskPriority = 2;

/**
 * @brief 检查 GPIO3 后按需启动 ADC 压力采样任务。
 *
 * GPIO3 稳定为 HIGH：不创建任务，不发送 0x1708。
 * GPIO3 稳定为 LOW：创建任务，200 Hz 采样，50 Hz 发送 0x1708。
 */
void start();

} // namespace gateway::adc_pressure_can_task
