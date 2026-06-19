/**
 * @file state_machine.cpp
 * @brief 全局系统状态单例：Boot / WiFi / MQTT 等字符串输出。
 */

#include "state_machine.h"

namespace gateway::state_machine {

    namespace {
        volatile SystemState g_state = SystemState::Boot;
    } // namespace

    SystemState current() { return g_state; }

    void set_state(SystemState s) { g_state = s; }

    const char *system_state_str(SystemState s) {
        switch (s) {
        case SystemState::Boot:
            return "Boot";
        case SystemState::WifiApMode:
            return "WifiApMode";
        case SystemState::WifiStaConnecting:
            return "WifiStaConnecting";
        case SystemState::WifiStaConnected:
            return "WifiStaConnected";
        case SystemState::MqttConnecting:
            return "MqttConnecting";
        case SystemState::MqttReady:
            return "MqttReady";
        case SystemState::WifiLost:
            return "WifiLost";
        case SystemState::MqttLost:
            return "MqttLost";
        }
        return "Boot";
    }

} // namespace gateway::state_machine
