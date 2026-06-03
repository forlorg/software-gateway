#pragma once

/**
 * @file state_machine.h
 * @brief 与上位机展示对齐的系统状态枚举与读写接口。
 */

namespace gateway::state_machine {

/** 与 `software_esp32s3` 对齐的系统状态枚举（含 MQTT 连接态）。 */
enum class SystemState {
  Boot,
  WifiApMode,
  WifiStaConnecting,
  WifiStaConnected,
  MqttConnecting,
  MqttReady,
  WifiLost,
  MqttLost,
};

SystemState current();
void set_state(SystemState s);

/** 可读名称，供 `/api/live_state` 展示。 */
const char *system_state_str(SystemState s);

} // namespace gateway::state_machine
