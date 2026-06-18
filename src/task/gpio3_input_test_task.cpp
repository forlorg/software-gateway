/**
 * @file gpio3_input_test_task.cpp
 * @brief 读取 GPIO3，不启用内部上下拉；启动、变化及每 5 秒打印一次状态。
 */

#include "task/gpio3_input_test_task.h"

#include <Arduino.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gateway::gpio3_input_test_task {
namespace {

static constexpr const char kLogTag[] = "GPIO3_TEST";
static TaskHandle_t g_task_handle = nullptr;

const char *levelName(int level) {
  return level == HIGH ? "HIGH" : "LOW";
}

void printState(int level, const char *reason) {
  Serial.printf("[%s] GPIO%d=%s raw=%d reason=%s\r\n", kLogTag, kInputPin,
                levelName(level), level == HIGH ? 1 : 0, reason);
}

void taskMain(void *) {
  pinMode(kInputPin, INPUT);

  // 明确关闭内部上下拉，GPIO3 电平仅由外部选择器/外部电路决定。
  gpio_pullup_dis(static_cast<gpio_num_t>(kInputPin));
  gpio_pulldown_dis(static_cast<gpio_num_t>(kInputPin));

  int stable_level = digitalRead(kInputPin);
  int candidate_level = stable_level;
  uint8_t candidate_count = 0;

  printState(stable_level, "initial");

  TickType_t last_wake = xTaskGetTickCount();
  TickType_t last_periodic_print = last_wake;

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kPollPeriodMs));

    const int sampled_level = digitalRead(kInputPin);

    if (sampled_level != candidate_level) {
      candidate_level = sampled_level;
      candidate_count = 1;
    } else if (candidate_level != stable_level) {
      if (candidate_count < kStableSampleCount) {
        ++candidate_count;
      }

      if (candidate_count >= kStableSampleCount) {
        stable_level = candidate_level;
        candidate_count = 0;
        printState(stable_level, "changed");
      }
    } else {
      candidate_count = 0;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<TickType_t>(now - last_periodic_print) >=
        pdMS_TO_TICKS(kPeriodicPrintMs)) {
      last_periodic_print = now;
      printState(stable_level, "periodic");
    }
  }
}

}  // namespace

void start() {
  if (g_task_handle != nullptr) {
    Serial.printf("[%s] already started\r\n", kLogTag);
    return;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      taskMain, "gpio3_input_test", kTaskStackBytes, nullptr,
      static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), &g_task_handle,
      kPinnedCore);

  if (result != pdPASS) {
    g_task_handle = nullptr;
    Serial.printf("[%s] FAIL create task\r\n", kLogTag);
    return;
  }

  Serial.printf(
      "[%s] started pin=%d mode=INPUT(no internal pull) core=%d "
      "poll=%lums periodic=%lums\r\n",
      kLogTag, kInputPin, kPinnedCore,
      static_cast<unsigned long>(kPollPeriodMs),
      static_cast<unsigned long>(kPeriodicPrintMs));
}

}  // namespace gateway::gpio3_input_test_task
