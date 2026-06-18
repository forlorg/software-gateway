#pragma once

/**
 * @file ads7924_i2c_test_task.h
 * @brief ADS7924 INT 诊断与低 CPU 高速四通道采样测试任务。
 *
 * 不测试 GPIO14 RESET。驱动初始化时仅保持 GPIO14 为高电平，防止 ADS7924
 * 意外进入硬件复位；所有状态初始化均通过 I2C 软件复位完成。
 */

#include <cstdint>

namespace gateway::ads7924_i2c_test_task {

constexpr int kSdaPin = 11;
constexpr int kSclPin = 12;
constexpr int kIntPin = 13;
constexpr int kResetPin = 14;

constexpr uint8_t kAddress = 0x48;

/** ADS7924 支持的 Fast-mode 上限。 */
constexpr uint32_t kI2cClockHz = 400000;

constexpr int kPinnedCore = 1;
constexpr uint32_t kTaskStackBytes = 7168;
constexpr uint32_t kTaskPriority = 2;

/**
 * 高速模式的 ACQTIME code。
 *
 * 0  => 6 us acquisition，单通道最快约 10 us（6 us acquisition + 4 us conversion）。
 * 31 => 68 us acquisition，适合更高源阻抗，但速率明显降低。
 *
 * 最高速率测试默认使用 0。若切换到 0 后发现读数偏差或通道串扰增大，
 * 应增大此值，而不是继续追求理论最高速率。
 */
constexpr uint8_t kHighRateAcquisitionCode = 0;

/** INT 功能测试使用较长采集时间，使示波器和软件诊断更容易观察。 */
constexpr uint8_t kIntTestAcquisitionCode = 31;

/**
 * 没有 INT 时，使用 esp_timer 单次定时唤醒采样任务。
 *
 * ACQTIME=0 时四通道名义转换时间约 40 us；考虑内部时钟误差和调度余量，
 * 默认等待 70 us。该模式不忙等，但吞吐量和完成时刻确定性略逊于硬件 INT。
 */
constexpr uint32_t kTimerFallbackWaitUs = 70;

constexpr uint32_t kCompletionTimeoutMs = 20;
constexpr uint32_t kStatisticsPeriodMs = 1000;
constexpr uint8_t kRuntimeTimeoutsBeforeFallback = 3;

/** INT 诊断失败后是否自动使用 esp_timer 继续高速采样。 */
constexpr bool kEnableTimerFallback = true;

void start();

}  // namespace gateway::ads7924_i2c_test_task
