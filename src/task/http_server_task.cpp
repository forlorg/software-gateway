/**
 * @file http_server_task.cpp
 * @brief 独立任务内运行 HTTP 服务：handleClient、配网状态 tick 与堆内存巡检。
 */

#include "http_server_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "network/provision_fsm.h"
#include "network/web_server.h"

namespace gateway::http_server_task {

    namespace {

        static constexpr const char kLogTag[] = "HTTP_TASK";

        static constexpr BaseType_t kWebTaskCore = static_cast<BaseType_t>(kPinnedCpuCoreIndex);

        /**
         * HTTP 服务任务：启动同步 WebServer，循环处理 HTTP 请求、推进配网状态机，
         * 并定期检查剩余堆内存，低于阈值时输出告警日志。
         */
        void task_http_server(void *) {
            Serial.printf("[%s] task on core %d\r\n", kLogTag, static_cast<int>(xPortGetCoreID()));

            gateway::web_server::start();

            uint32_t last_heap_ms = 0;

            for (;;) {
                gateway::web_server::poll();

                gateway::provision::tick();

                const uint32_t now = millis();
                if (now - last_heap_ms >= kHeapCheckIntervalMs) {
                    last_heap_ms = now;
                    if (ESP.getFreeHeap() < kHeapWarnMinFreeBytes) {
                        Serial.printf("[%s] heap_low free=%lu\r\n", kLogTag,
                            static_cast<unsigned long>(ESP.getFreeHeap()));
                    }
                }

                vTaskDelay(pdMS_TO_TICKS(kLoopDelayMs));
            }
        }

    } // namespace

    void start() {
        Serial.printf("[%s] starting pinned core=%d stack=%lu (sync WebServer)\r\n", kLogTag,
            static_cast<int>(kWebTaskCore), static_cast<unsigned long>(kTaskStackBytes));
        xTaskCreatePinnedToCore(task_http_server, "HTTP_SRV", kTaskStackBytes, nullptr,
            static_cast<UBaseType_t>(tskIDLE_PRIORITY + 2), nullptr, kWebTaskCore);
    }

} // namespace gateway::http_server_task
