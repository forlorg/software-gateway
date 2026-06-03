/**
 * @file heartbeat_led_task.cpp
 * @brief 板载 LED 心跳闪烁任务，便于现场判断固件未死机。
 */

#include "heartbeat_led_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway::heartbeat_led {

namespace {

static constexpr const char kLogTag[] = "HB_LED";

static constexpr uint32_t kStackBytes = 3072;
static constexpr BaseType_t kPinnedCore = static_cast<BaseType_t>(1);

/**
 * 心跳 LED 任务：初始化板载 LED GPIO，并按配置半周期持续翻转电平，
 * 用于现场观察固件调度是否仍在运行。
 */
void task_heartbeat_led(void *) {
  const int pin = kHeartbeatLedPin;
  Serial.printf("[%s] task on core %d\n", kLogTag, xPortGetCoreID());
  if (pin >= 0) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, kHeartbeatLedActiveHigh ? LOW : HIGH);
  }
  const TickType_t half = pdMS_TO_TICKS(
      kHeartbeatBlinkHalfPeriodMs <= 50 ? 250u : kHeartbeatBlinkHalfPeriodMs);
  bool on = false;
  for (;;) {
    if (pin >= 0) {
      on = !on;
      const uint8_t level = kHeartbeatLedActiveHigh ? (on ? HIGH : LOW) : (on ? LOW : HIGH);
      digitalWrite(pin, level);
    }
    vTaskDelay(half);
  }
}

} // namespace

void start() {
  if (kHeartbeatLedPin < 0) {
    Serial.printf("[%s] disabled (kHeartbeatLedPin < 0)\n", kLogTag);
    return;
  }
  Serial.printf("[%s] starting Core1 task, GPIO %d, half-period %lu ms, active_high=%d\n", kLogTag,
                kHeartbeatLedPin, static_cast<unsigned long>(kHeartbeatBlinkHalfPeriodMs),
                kHeartbeatLedActiveHigh ? 1 : 0);
  xTaskCreatePinnedToCore(task_heartbeat_led, "HB_LED", kStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), nullptr, kPinnedCore);
}

} // namespace gateway::heartbeat_led
