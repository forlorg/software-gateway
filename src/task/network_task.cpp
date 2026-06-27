/*
 * @file network_task.cpp
 * @brief SoftAP/STA、MQTT、SNTP 与网络状态灯更新所在网络任务（固定 Core0）。
 */

#include "network_task.h"

#include <Arduino.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "network/mqtt_manager.h"
#include "network/wifi_manager.h"
#include "system/time_sync.h"
#include "task/status_led_task.h"

namespace gateway::network_task {

    namespace {

        static constexpr const char kLogTag[] = "NET_TASK";
        static constexpr BaseType_t kPinnedCore = static_cast<BaseType_t>(0);
        static constexpr uint32_t kNetworkLoopDelayMs = 40;
        static constexpr uint32_t kDiagPeriodMs = 30000;    // 每 30s 输出任务诊断

        /**
         * 根据 Wi-Fi STA 和 MQTT 连接状态更新 GPIO2 网络状态灯。
         *
         * 优先级：
         * - Wi-Fi 未连接：熄灭。
         * - Wi-Fi 已连接且 MQTT 已连接：快闪。
         * - Wi-Fi 已连接但 MQTT 未连接：慢闪。
         */
        void update_network_led_mode() {
            if (!wifi_manager::sta_is_linked()) {
                status_led::set_network_mode(status_led::NetworkLedMode::Off);
                return;
            }

            if (mqtt_manager::is_connected()) {
                status_led::set_network_mode(
                    status_led::NetworkLedMode::MqttConnected);
                return;
            }

            status_led::set_network_mode(
                status_led::NetworkLedMode::WifiConnected);
        }

        /**
         * 网络轮询任务：启动 Wi-Fi 管理器后持续处理 SoftAP/STA 配网状态、SNTP 时间同步轮询、
         * MQTT 客户端连接/收发维护，并在同一 Core0 上更新网络状态灯模式。
         */
        void task_network(void *) {
            Serial.printf(
                "[%s] task on core %d\r\n",
                kLogTag,
                static_cast<int>(xPortGetCoreID()));

            wifi_manager::start();

            uint32_t last_diag_ms = 0;

            for (;;) {
                const uint32_t t_loop_start = millis();

                wifi_manager::loop();
                gateway::time_sync::loop_poll();
                gateway::mqtt_manager::loop_poll();
                update_network_led_mode();

                const uint32_t t_loop_elapsed = millis() - t_loop_start;

                // 若单次循环超过 200ms 则告警
                if (t_loop_elapsed > 200) {
                    const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
                    Serial.printf(
                        "[%s] SLOW_LOOP elapsed=%lums stack_free=%lu heap_free=%lu\r\n",
                        kLogTag,
                        static_cast<unsigned long>(t_loop_elapsed),
                        static_cast<unsigned long>(stack_free),
                        static_cast<unsigned long>(ESP.getFreeHeap()));
                }

                // 每 30s 输出网络任务诊断
                if (millis() - last_diag_ms >= kDiagPeriodMs) {
                    last_diag_ms = millis();
                    const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(nullptr);
                    Serial.printf(
                        "[%s] diag: wifi_sta=%d mqtt=%d ntp=%d tz_ready=%d "
                        "tz_offset=%d tz_bits=%u stack_free=%lu heap_free=%lu heap_min=%lu\r\n",
                        kLogTag,
                        static_cast<int>(wifi_manager::sta_is_linked()),
                        static_cast<int>(mqtt_manager::is_connected()),
                        static_cast<int>(gateway::time_sync::ntp_has_sync()),
                        static_cast<int>(gateway::time_sync::timezone_is_ready()),
                        gateway::time_sync::timezone_offset_hours(),
                        static_cast<unsigned>(gateway::time_sync::timezone_bits()),
                        static_cast<unsigned long>(stack_free),
                        static_cast<unsigned long>(ESP.getFreeHeap()),
                        static_cast<unsigned long>(ESP.getMinFreeHeap()));
                }

                vTaskDelay(pdMS_TO_TICKS(kNetworkLoopDelayMs));
            }
        }

    } // namespace

    void start() {
        Serial.printf(
            "[%s] starting Core0 task stack=%lu\r\n",
            kLogTag,
            static_cast<unsigned long>(kTaskStackBytes));

        xTaskCreatePinnedToCore(
            task_network,
            "NET_TASK",
            kTaskStackBytes,
            nullptr,
            static_cast<UBaseType_t>(tskIDLE_PRIORITY + 2),
            nullptr,
            kPinnedCore);
    }

} // namespace gateway::network_task
