#pragma once

/**
 * @file heartbeat_led_task.h
 * @brief 心跳灯引脚、极性与周期常量及启动接口。
 */

#include <cstdint>

namespace gateway::heartbeat_led {

constexpr int kHeartbeatLedPin = 1;
constexpr bool kHeartbeatLedActiveHigh = true;
constexpr uint32_t kHeartbeatBlinkHalfPeriodMs = 500;

void start();

} // namespace gateway::heartbeat_led
