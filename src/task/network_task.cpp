/*
 * @file network_task.cpp
 * @brief SoftAP/STA、MQTT 与 SNTP 轮询所在网络任务（通常固定 Core0）。
 */

#include "network_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "network/mqtt_manager.h"
#include "network/wifi_manager.h"
#include "system/time_sync.h"

namespace gateway::network_task {

namespace {

static constexpr const char kLogTag[] = "NET_TASK";

static constexpr BaseType_t kPinnedCore = static_cast<BaseType_t>(0);

static constexpr uint32_t kNetworkLoopDelayMs = 40;

/**
 * 网络轮询任务：启动 WiFi 管理器后持续处理 SoftAP/STA 配网状态、SNTP 时间同步轮询
 * 以及 MQTT 客户端连接/收发维护，按固定周期让出 CPU。
 */
void task_network(void *) {
  Serial.printf("[%s] task on core %d\r\n", kLogTag, static_cast<int>(xPortGetCoreID()));

  wifi_manager::start();
  for (;;) {
    wifi_manager::loop();
    gateway::time_sync::loop_poll();
    gateway::mqtt_manager::loop_poll();
    vTaskDelay(pdMS_TO_TICKS(kNetworkLoopDelayMs));
  }
}

} // namespace

void start() {
  Serial.printf("[%s] starting Core0 task stack=%lu\r\n", kLogTag,
                static_cast<unsigned long>(kTaskStackBytes));
  xTaskCreatePinnedToCore(task_network, "NET_TASK", kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), nullptr, kPinnedCore);
}

} // namespace gateway::network_task
