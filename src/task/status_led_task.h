#pragma once

/**
 * @file status_led_task.h
 * @brief 状态灯配置与接口，包含 GPIO1 心跳灯和 GPIO2 网络状态灯。
 */

#include <cstdint>

namespace gateway::status_led {

    /**
     * GPIO2 网络状态灯模式。
     *
     * GPIO2 为低电平有效：
     * - Off：Wi-Fi 未连接，LED 熄灭。
     * - WifiConnected：Wi-Fi 已连接但 MQTT 未连接，1 秒亮、1 秒灭。
     * - MqttConnected：Wi-Fi 和 MQTT 均已连接，200 ms 亮、200 ms 灭。
     */
    enum class NetworkLedMode : uint8_t {
        Off = 0,
        WifiConnected,
        MqttConnected,
    };

    /** GPIO1 心跳灯：高电平有效，用于判断固件任务调度仍在运行。 */
    constexpr int kHeartbeatLedPin = 1;
    constexpr bool kHeartbeatLedActiveHigh = true;
    constexpr uint32_t kHeartbeatBlinkHalfPeriodMs = 500;

    /**
     * GPIO2 网络状态灯：低电平有效。
     *
     * Wi-Fi 已连接时使用 2 秒完整周期慢闪，作为呼吸节奏指示；
     * MQTT 同时连接时使用 400 ms 完整周期快闪。
     */
    constexpr int kNetworkLedPin = 2;
    constexpr bool kNetworkLedActiveHigh = false;
    constexpr uint32_t kNetworkLedWifiHalfPeriodMs = 1000;
    constexpr uint32_t kNetworkLedMqttHalfPeriodMs = 200;
    constexpr uint32_t kNetworkLedPollPeriodMs = 20;

    /**
     * 设置 GPIO2 网络状态灯模式。
     *
     * 可由 Core0 网络任务调用；状态通过原子变量传递给网络灯任务。
     */
    void set_network_mode(NetworkLedMode mode);

    /** 启动 GPIO1 心跳灯任务和 GPIO2 网络状态灯任务。 */
    void start();

} // namespace gateway::status_led
