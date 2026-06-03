/**
 * @file web_server_task.cpp
 * @brief 独立任务内运行 HTTP 服务：`handleClient`、配网状态 tick 与堆内存巡检。
 */

#include "web_server_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "network/provision_fsm.h"
#include "network/sse_manager.h"
#include "network/web_server.h"

namespace gateway::web_server_task {

namespace {

static constexpr const char kLogTag[] = "WEB_TASK";

static constexpr BaseType_t kWebTaskCore = static_cast<BaseType_t>(kPinnedCpuCoreIndex);

void task_web_server(void *) {
  Serial.printf("[%s] task on core %d\n", kLogTag, static_cast<int>(xPortGetCoreID()));

  gateway::web_server::start();

  uint32_t last_live_ms = 0;
  uint32_t last_heap_ms = 0;

  for (;;) {
    gateway::web_server::poll();

    gateway::provision::tick();

    {
      const uint32_t now = millis();
      if (now - last_live_ms >= kSsePumpThrottleMs) {
        last_live_ms = now;
        gateway::sse_manager::tick();
      }
    }

    {
      const uint32_t now = millis();
      if (now - last_heap_ms >= kHeapCheckIntervalMs) {
        last_heap_ms = now;
        if (ESP.getFreeHeap() < kHeapWarnMinFreeBytes) {
          Serial.printf("[%s] heap_low free=%lu\n", kLogTag,
                        static_cast<unsigned long>(ESP.getFreeHeap()));
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
  }
}

} // namespace

void start() {
  Serial.printf("[%s] starting pinned core=%d stack=%lu (sync WebServer)\n", kLogTag,
                static_cast<int>(kWebTaskCore), static_cast<unsigned long>(kTaskStackBytes));
  xTaskCreatePinnedToCore(task_web_server, "WEB_SRV", kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 2), nullptr, kWebTaskCore);
}

} // namespace gateway::web_server_task
