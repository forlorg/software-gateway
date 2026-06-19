/**
 * @file status_led_task.cpp
 * @brief 状态灯任务，包含 GPIO1 心跳灯和 GPIO2 网络状态灯。
 */

#include "status_led_task.h"

#include <Arduino.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway::status_led {

    namespace {

        static constexpr const char kLogTag[] = "STATUS_LED";
        static constexpr uint32_t kHeartbeatTaskStackBytes = 3072;
        static constexpr uint32_t kNetworkTaskStackBytes = 3072;
        static constexpr BaseType_t kHeartbeatPinnedCore = static_cast<BaseType_t>(1);
        static constexpr BaseType_t kNetworkPinnedCore = static_cast<BaseType_t>(0);

        std::atomic<uint8_t> gNetworkLedMode{
            static_cast<uint8_t>(NetworkLedMode::Off),
        };

        /** 根据 LED 有效电平计算实际 GPIO 输出电平。 */
        uint8_t led_level(bool active_high, bool on) {
            if (active_high) {
                return on ? HIGH : LOW;
            }

            return on ? LOW : HIGH;
        }

        /** 设置 LED 亮灭状态。 */
        void write_led(int pin, bool active_high, bool on) {
            if (pin < 0) {
                return;
            }

            digitalWrite(pin, led_level(active_high, on));
        }

        /**
         * 配置 LED GPIO，并先写入熄灭电平。
         *
         * 在切换为输出前先设置输出锁存值，可降低低电平有效 LED 初始化瞬间误亮的概率。
         */
        void configure_led_off(int pin, bool active_high) {
            if (pin < 0) {
                return;
            }

            digitalWrite(pin, led_level(active_high, false));
            pinMode(pin, OUTPUT);
            write_led(pin, active_high, false);
        }

        /** 以支持 tick 回绕的方式判断是否到达切换时刻。 */
        bool tick_reached(TickType_t now, TickType_t deadline) {
            return static_cast<int32_t>(now - deadline) >= 0;
        }

        /** 返回当前网络灯模式对应的亮灭半周期。 */
        TickType_t network_half_period(NetworkLedMode mode) {
            const uint32_t half_period_ms =
            mode == NetworkLedMode::MqttConnected
            ? kNetworkLedMqttHalfPeriodMs
            : kNetworkLedWifiHalfPeriodMs;

            return pdMS_TO_TICKS(half_period_ms);
        }

        /**
         * GPIO1 心跳灯任务。
         *
         * 固定运行在 Core1，500 ms 翻转一次，用于现场判断固件任务调度仍在运行。
         */
        void task_heartbeat_led(void *) {
            Serial.printf(
                "[%s] heartbeat task on core %d, GPIO %d\r\n",
                kLogTag,
                static_cast<int>(xPortGetCoreID()),
                kHeartbeatLedPin);

            configure_led_off(kHeartbeatLedPin, kHeartbeatLedActiveHigh);

            const TickType_t half_period = pdMS_TO_TICKS(
                kHeartbeatBlinkHalfPeriodMs <= 50
                ? 250u
                : kHeartbeatBlinkHalfPeriodMs);

            bool on = false;

            for (;;) {
                on = !on;
                write_led(kHeartbeatLedPin, kHeartbeatLedActiveHigh, on);
                vTaskDelay(half_period);
            }
        }

        /**
         * GPIO2 网络状态灯任务。
         *
         * 固定运行在 Core0：
         * - Wi-Fi 未连接：熄灭。
         * - Wi-Fi 已连接、MQTT 未连接：1 秒亮、1 秒灭。
         * - Wi-Fi 和 MQTT 均已连接：200 ms 亮、200 ms 灭。
         *
         * 任务按较短周期检查模式变化，因此 Wi-Fi 断开时无需等待当前闪烁半周期结束。
         */
        void task_network_led(void *) {
            Serial.printf(
                "[%s] network task on core %d, GPIO %d, active_low=1\r\n",
                kLogTag,
                static_cast<int>(xPortGetCoreID()),
                kNetworkLedPin);

            configure_led_off(kNetworkLedPin, kNetworkLedActiveHigh);

            NetworkLedMode current_mode = NetworkLedMode::Off;
            bool on = false;
            TickType_t last_wake = xTaskGetTickCount();
            TickType_t next_toggle = last_wake;
            const TickType_t poll_period = pdMS_TO_TICKS(kNetworkLedPollPeriodMs);

            for (;;) {
                const NetworkLedMode requested_mode = static_cast<NetworkLedMode>(
                    gNetworkLedMode.load(std::memory_order_relaxed));
                const TickType_t now = xTaskGetTickCount();

                if (requested_mode != current_mode) {
                    current_mode = requested_mode;
                    on = false;
                    write_led(kNetworkLedPin, kNetworkLedActiveHigh, false);
                    next_toggle = now;
                }

                if (current_mode != NetworkLedMode::Off &&
                    tick_reached(now, next_toggle)) {
                    on = !on;
                    write_led(kNetworkLedPin, kNetworkLedActiveHigh, on);
                    next_toggle = now + network_half_period(current_mode);
                }

                vTaskDelayUntil(&last_wake, poll_period);
            }
        }

        void start_task(
            TaskFunction_t task,
            const char *name,
            uint32_t stack_bytes,
            BaseType_t core) {
            const BaseType_t result = xTaskCreatePinnedToCore(
                task,
                name,
                stack_bytes,
                nullptr,
                static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1),
                nullptr,
                core);

            if (result != pdPASS) {
                Serial.printf(
                    "[%s] failed to start %s on core %d, error=%d\r\n",
                    kLogTag,
                    name,
                    static_cast<int>(core),
                    static_cast<int>(result));
            }
        }

    } // namespace

    void set_network_mode(NetworkLedMode mode) {
        gNetworkLedMode.store(
            static_cast<uint8_t>(mode),
            std::memory_order_relaxed);
    }

    void start() {
        if (kHeartbeatLedPin >= 0) {
            configure_led_off(kHeartbeatLedPin, kHeartbeatLedActiveHigh);
            Serial.printf(
                "[%s] starting heartbeat LED: Core1 GPIO %d, half-period %lu ms\r\n",
                kLogTag,
                kHeartbeatLedPin,
                static_cast<unsigned long>(kHeartbeatBlinkHalfPeriodMs));
            start_task(
                task_heartbeat_led,
                "HB_LED",
                kHeartbeatTaskStackBytes,
                kHeartbeatPinnedCore);
        } else {
            Serial.printf("[%s] heartbeat LED disabled\r\n", kLogTag);
        }

        if (kNetworkLedPin >= 0) {
            configure_led_off(kNetworkLedPin, kNetworkLedActiveHigh);
            Serial.printf(
                "[%s] starting network LED: Core0 GPIO %d, WiFi half-period %lu ms, "
                "MQTT half-period %lu ms\r\n",
                kLogTag,
                kNetworkLedPin,
                static_cast<unsigned long>(kNetworkLedWifiHalfPeriodMs),
                static_cast<unsigned long>(kNetworkLedMqttHalfPeriodMs));
            start_task(
                task_network_led,
                "NET_LED",
                kNetworkTaskStackBytes,
                kNetworkPinnedCore);
        } else {
            Serial.printf("[%s] network LED disabled\r\n", kLogTag);
        }
    }

} // namespace gateway::status_led
