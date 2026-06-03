/**
 * @file wifi_ap_task.cpp
 * @brief SoftAP 与 STA 侧 `WiFi.loop`、MQTT 与 SNTP 轮询所在任务（通常固定 Core0）。
 */

#include "wifi_ap_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "system/time_sync.h"
#include "network/wifi_manager.h"
#include "network/mqtt_manager.h"

namespace gateway::wifi_ap_task {

namespace {

static constexpr const char kLogTag[] = "AP_TASK";

static constexpr BaseType_t kPinnedCore = static_cast<BaseType_t>(0);

static constexpr uint32_t kWifiLoopDelayMs = 40;

void task_wifi_ap(void *) {
  Serial.printf("[%s] task on core %d\n", kLogTag, static_cast<int>(xPortGetCoreID()));

  wifi_manager::start();
  for (;;) {
    wifi_manager::loop();
    gateway::time_sync::loop_poll();
    gateway::mqtt_manager::loop_poll();
    vTaskDelay(pdMS_TO_TICKS(kWifiLoopDelayMs));
  }
}

} // namespace

void start() {
  Serial.printf("[%s] starting Core0 task stack=%lu\n", kLogTag,
                static_cast<unsigned long>(kTaskStackBytes));
  xTaskCreatePinnedToCore(task_wifi_ap, "WiFi_AP", kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), nullptr, kPinnedCore);
}

} // namespace gateway::wifi_ap_task
