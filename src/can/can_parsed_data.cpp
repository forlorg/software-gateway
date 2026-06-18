/**
 * @file can_parsed_data.cpp
 * @brief 指定 CAN 帧解析、缓存与结构体查询实现。
 *
 * ObjSystemStatus 各字段来源：
 *   state_machine   — PGN 0x1744: data[0] bits 4-7（系统状态机），映射为可显示字符串
 *   system_pressure — PGN 0x1709: data[0-1] LE uint16（系统压力）, offset=-500, unit=kPa
 *
 * get_obj_system_status() 有效性规则：
 *   - 0x1744 帧必须曾经收到过，且距今 ≤ 2000ms
 *
 * ObjPto 各字段来源：
 *   pto_switch   — PGN 0x1744（frame 0x16174403）: data[3] bit 0-1
 *   pto_lock     — PGN 0x1744（frame 0x16174403）: data[3] bit 4-5（PTO倒车锁）
 *   pto_solenoid — PGN 0x1703（frame 0x16170303）: data[4-5] LE uint16, scaling=1, unit=mA
 *   pto_pressure — PGN 0x1709（frame 0x16170903）: data[4-5] LE uint16, offset=-500, unit=kPa
 *
 * get_obj_pto() 有效性规则：
 *   - PTO 电磁阀电流帧（0x1703）必须曾经收到过
 *   - 最近一次电流帧距今不超过 2000ms
 *
 * ObjDrive 各字段来源：
 *   power_shuttle_state — PGN 0x1744: 换向手柄(data[1]bits6-7) + 换挡手柄(data[1]bits4-5) 综合判定
 *   clutch_padel_slip   — PGN 0x1712: data[2-3] LE uint16（离合器打滑率 raw）, scaling=0.002, unit=%
 *   drive_lock          — PGN 0x1744: data[4] bit 0-1（换向超速锁状态）
 *   drive_solenoid      — PGN 0x1700(data[4-5]) / 0x1701(data[4-5]) / 0x1702(data[4-5])
 *                          根据 power_shuttle_state 选择对应档位的采样电流
 *   drive_pressure      — PGN 0x1708: 高档 data[0-1] / 低档 data[2-3] / 倒档 data[4-5]
 *                          根据 power_shuttle_state 选择对应档位的测量压力
 *
 * get_obj_drive() 有效性规则：
 *   - 非 Stop 状态：对应档位电磁阀电流帧必须曾经收到过，且距今 ≤ 2000ms
 *   - Stop 状态：0x1744 帧必须曾经收到过，且距今 ≤ 2000ms
 */

#include "can_parsed_data.h"

#include "network/mqtt_manager.h"
#include "system/gateway_context.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace gateway::can_parsed_data {
namespace {

// ── 常量 ────────────────────────────────────────────────

constexpr const char kTag[] = "CAN_PARSE";
constexpr gpio_num_t kSourceSelectGpio = GPIO_NUM_15;
constexpr int64_t kPtoSolenoidValidWindowMs = 2000;
constexpr int64_t kDriveSolenoidValidWindowMs = 2000;
constexpr int64_t k1744ValidWindowMs = 2000;

// ── 帧 ID ───────────────────────────────────────────────

// ── 通道过滤表 ─────────────────────────────────────────
static constexpr uint32_t kChannel0FrameIds[] = {
    0x06FEF003u,
    0x1615A303u,
    0x1615A903u,
    0x06151E03u,
};

static constexpr uint32_t kChannel1FrameIds[] = {
    0x16174403u,  // 0x1744: 换向/换挡手柄、PTO锁/开关、换向超速锁
    0x16170303u,  // 0x1703: PTO 电磁阀电流
    0x16170903u,  // 0x1709: PTO 压力
    0x16174B03u,  // Product Announce
    0x16170003u,  // 0x1700: 高档电磁阀电流 / 计算压力
    0x16170103u,  // 0x1701: 低档电磁阀电流 / 计算压力
    0x16170203u,  // 0x1702: 倒档电磁阀电流 / 计算压力
    0x16170803u,  // 0x1708: 高/低/倒档测量压力
    0x16171203u,  // 0x1712: 离合器打滑率
    0x16178003u,  // 0x1780: 高档空载点 (起步点)
    0x16178203u,  // 0x1782: 低档空载点 (起步点)
    0x16178403u,  // 0x1784: 倒档空载点 (起步点)
    0x16179803u,  // 0x1798: PTO 起步点
};

// ── Product Announce ────────────────────────────────────
static constexpr uint32_t kProductAnnounceCanIdRef = 0x16174B03u;
static constexpr uint32_t kProductAnnounceIdMatchLow24 = kProductAnnounceCanIdRef & 0x00FFFFFFu;

// ── 全局状态 ────────────────────────────────────────────
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
SourceChannel g_source_channel{SourceChannel::kChannel0};

// ── PTO 状态 ────────────────────────────────────────────
uint8_t g_pto_switch_raw = 0;
uint8_t g_pto_lock_raw = 0;
uint16_t g_pto_solenoid_raw = 0;
uint16_t g_pto_pressure_raw = 0;
int64_t g_pto_last_update_ms = 0;

/// 最近一次 PTO 电磁阀电流帧（0x1703）到达时刻（ms）。0 表示从未收到过该帧。
int64_t g_pto_solenoid_last_ms = 0;

// ── 内部枚举（仅用于解析逻辑，对外输出字符串）──────────
enum class PowerShuttleState : uint8_t {
  kStop = 0,
  kForwardHigh = 1,
  kForwardLow = 2,
  kReverse = 3,
};

// ── SystemStatus 状态 ───────────────────────────────────
// 来自 0x1744
uint8_t g_system_status_raw = 0;            // 系统状态机 raw 值 (data[0] bits 4-7)
int64_t g_system_status_1744_last_ms = 0;   // 最近一次 0x1744 帧的时间戳

// 来自 0x1709
uint16_t g_system_pressure_raw = 0;         // 系统压力 (data[0-1] LE uint16)

// ── Drive 状态 ──────────────────────────────────────────
// 来自 0x1744
uint8_t g_drive_direction_raw = 0;          // 换向手柄状态 (data[1] bits 6-7)
uint8_t g_drive_gear_lever_raw = 0;         // 换挡手柄状态 (data[1] bits 4-5)
uint8_t g_drive_lock_raw = 0;               // 换向超速锁状态 (data[4] bits 0-1)
PowerShuttleState g_drive_power_shuttle = PowerShuttleState::kStop;
int64_t g_drive_1744_last_ms = 0;           // 最近一次 0x1744 帧的时间戳

// 来自 0x1700 / 0x1701 / 0x1702（各档位采样电流）
uint16_t g_drive_solenoid_high_raw = 0;
uint16_t g_drive_solenoid_low_raw = 0;
uint16_t g_drive_solenoid_reverse_raw = 0;
int64_t g_drive_solenoid_high_ms = 0;
int64_t g_drive_solenoid_low_ms = 0;
int64_t g_drive_solenoid_reverse_ms = 0;

// 来自 0x1708（各档位测量压力）
uint16_t g_drive_pressure_high_raw = 0;
uint16_t g_drive_pressure_low_raw = 0;
uint16_t g_drive_pressure_reverse_raw = 0;

// 来自 0x1712（离合器打滑率）
uint16_t g_drive_clutch_slip_raw = 0;

// ── ClutchStartup 状态 ────────────────────────────────────
// 来自 0x1780 / 0x1782 / 0x1784 / 0x1798（各离合器起步点 3Y）
uint8_t g_clutch_high_raw_3y = 0;
uint8_t g_clutch_low_raw_3y = 0;
uint8_t g_clutch_rev_raw_3y = 0;
uint8_t g_clutch_pto_raw_3y = 0;
int64_t g_clutch_high_last_ms = 0;
int64_t g_clutch_low_last_ms = 0;
int64_t g_clutch_rev_last_ms = 0;
int64_t g_clutch_pto_last_ms = 0;

// ── 工具函数 ────────────────────────────────────────────

bool id_in_list(uint32_t id, const uint32_t *ids, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (ids[i] == id) return true;
  }
  return false;
}

// void log_frame(uint32_t id, const uint8_t *data, uint8_t dlc) {
//   Serial.printf("[%s] src=%s id=0x%08lX dlc=%u data=", kTag, source_channel_name(),
//                 static_cast<unsigned long>(id), static_cast<unsigned>(dlc));
//   for (uint8_t i = 0; i < dlc; ++i) {
//     Serial.printf("%02X", data[i]);
//     if (i + 1 < dlc) Serial.print(' ');
//   }
//   Serial.print("\r\n");
// }

bool frame_allowed_for_selected_source(uint32_t id) {
  if (g_source_channel == SourceChannel::kChannel1) {
    return id_in_list(id, kChannel1FrameIds, sizeof(kChannel1FrameIds) / sizeof(kChannel1FrameIds[0]));
  }
  return id_in_list(id, kChannel0FrameIds, sizeof(kChannel0FrameIds) / sizeof(kChannel0FrameIds[0]));
}

bool is_product_announce(uint32_t id) {
  return (id & 0x00FFFFFFu) == kProductAnnounceIdMatchLow24;
}

void process_product_announce(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!is_product_announce(id)) return;
  if (gateway::ctx::set_product_id_from_payload_le(data, dlc)) {
    gateway::mqtt_manager::notify_product_id_changed();
    Serial.printf("[%s] id=0x%08lX product_id updated\r\n", kTag, static_cast<unsigned long>(id));
  }
}

/**
 * 根据换向手柄与换挡手柄原始值，计算动力换挡状态。
 *
 * 换向手柄 (enum_id=8): 0=中位, 1=前进, 2=后退, 3=故障
 * 换挡手柄 (enum_id=9): 0=高档, 1=低档
 *
 * 规则：
 *   - 中位或故障 → kStop
 *   - 前进 + 高档  → kForwardHigh
 *   - 前进 + 低档  → kForwardLow
 *   - 后退（忽略换挡手柄）→ kReverse
 */
PowerShuttleState compute_power_shuttle_state(uint8_t direction_raw, uint8_t gear_lever_raw) {
  switch (direction_raw) {
    case 0:  // 中位
      return PowerShuttleState::kStop;
    case 1:  // 前进
      return (gear_lever_raw == 1) ? PowerShuttleState::kForwardLow
                                   : PowerShuttleState::kForwardHigh;
    case 2:  // 后退
      return PowerShuttleState::kReverse;
    default:  // 故障 (3) 或未知
      return PowerShuttleState::kStop;
  }
}

/**
 * 系统状态机 raw 值 → 可显示字符串。
 *
 * PGN 0x1744 "系统状态机" (enum_id=170150):
 *   0=初始化, 1=锁定, 2=运行, 3=调试, 4=故障, 5=自检, 6=阀测试
 *
 * 业务约定：仅 初始化/锁定/运行 按原样显示，其余统一为 "其它"。
 */
const char *state_machine_to_string(uint8_t raw) {
  switch (raw) {
    case 0:  return "初始化";
    case 1:  return "锁定";
    case 2:  return "运行";
    default: return "其它";
  }
}

/**
 * PowerShuttleState → 可显示字符串。
 */
const char *power_shuttle_to_string(PowerShuttleState s) {
  switch (s) {
    case PowerShuttleState::kStop:        return "停止";
    case PowerShuttleState::kForwardHigh: return "高档";
    case PowerShuttleState::kForwardLow:  return "低档";
    case PowerShuttleState::kReverse:     return "倒档";
    default:                              return "未知";
  }
}

String format_uptime(unsigned long uptime_ms) {
  const unsigned long total_seconds = uptime_ms / 1000UL;
  const unsigned long days = total_seconds / 86400UL;
  const unsigned long hours = (total_seconds % 86400UL) / 3600UL;
  const unsigned long minutes = (total_seconds % 3600UL) / 60UL;
  const unsigned long seconds = total_seconds % 60UL;

  char buf[32]{};
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", days, hours, minutes, seconds);
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  }
  return String(buf);
}

// ── 帧解析 ──────────────────────────────────────────────

/**
 * 解析 PGN 0x1744（frame 0x16174403）。
 *
 * 提取字段：
 *   - 系统状态机        data[0] bit 4-7
 *   - 系统使能          data[0] bit 2-3
 *   - 自检状态          data[0] bit 0-1
 *   - 换向手柄状态      data[1] bit 6-7  → Drive
 *   - 换挡手柄状态      data[1] bit 4-5  → Drive
 *   - 档位状态          data[1] bit 0-3
 *   - PTO锁状态         data[3] bit 2-3  → ObjPto.pto_lock
 *   - PTO开关状态       data[3] bit 0-1  → ObjPto.pto_switch
 *   - 换向超速锁状态    data[4] bit 0-1  → ObjDrive.drive_lock
 */
void parse_frame_1744(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 4) return;

  // log_frame(id, data, dlc);

  const uint8_t raw_system_status     = (data[0] >> 4) & 0x0F;
  const uint8_t raw_system_enable     = (data[0] >> 2) & 0x03;
  const uint8_t raw_self_test         =  data[0]       & 0x03;
  const uint8_t raw_direction         = (data[1] >> 6) & 0x03;  // 换向手柄
  const uint8_t raw_gear_lever        = (data[1] >> 4) & 0x03;  // 换挡手柄
  const uint8_t raw_gear_status       =  data[1]       & 0x0F;
  const uint8_t raw_pto_switch        =  data[3]       & 0x03;
  const uint8_t raw_pto_lock = (data[3] >> 2) & 0x03;  // PTO锁

  // 换向超速锁状态在 data[4] bits 0-1，需 dlc >= 5
  const uint8_t raw_drive_lock = (dlc >= 5) ? (data[4] & 0x03) : 0;

  const auto power_shuttle = compute_power_shuttle_state(raw_direction, raw_gear_lever);
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  // ── SystemStatus 字段更新 ──
  g_system_status_raw          = raw_system_status;
  g_system_status_1744_last_ms = now;

  // ── PTO 字段更新 ──
  g_pto_switch_raw = raw_pto_switch;
  g_pto_lock_raw = raw_pto_lock;
  g_pto_last_update_ms = now;

  // ── Drive 字段更新 ──
  g_drive_direction_raw   = raw_direction;
  g_drive_gear_lever_raw  = raw_gear_lever;
  g_drive_lock_raw        = raw_drive_lock;
  g_drive_power_shuttle   = power_shuttle;
  g_drive_1744_last_ms    = now;
  portEXIT_CRITICAL(&g_lock);

}

/**
 * 解析 PGN 0x1700（frame 0x16170003）— 高档电磁阀。
 *
 * 提取字段：
 *   - 当前高档采样电流  data[4-5] LE uint16, scaling=1, unit=mA  → Drive
 */
void parse_frame_1700(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint16_t raw_solenoid = static_cast<uint16_t>(data[4])
                              | (static_cast<uint16_t>(data[5]) << 8);
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_drive_solenoid_high_raw = raw_solenoid;
  g_drive_solenoid_high_ms  = now;
  portEXIT_CRITICAL(&g_lock);

  // Serial.printf("[%s] id=0x%08lX drive_solenoid_high=%u mA\r\n", kTag,
  //                static_cast<unsigned long>(id), static_cast<unsigned>(raw_solenoid));
}

/**
 * 解析 PGN 0x1701（frame 0x16170103）— 低档电磁阀。
 *
 * 提取字段：
 *   - 当前低档采样电流  data[4-5] LE uint16, scaling=1, unit=mA  → Drive
 */
void parse_frame_1701(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint16_t raw_solenoid = static_cast<uint16_t>(data[4])
                              | (static_cast<uint16_t>(data[5]) << 8);
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_drive_solenoid_low_raw = raw_solenoid;
  g_drive_solenoid_low_ms  = now;
  portEXIT_CRITICAL(&g_lock);

  // Serial.printf("[%s] id=0x%08lX drive_solenoid_low=%u mA\r\n", kTag,
  //               static_cast<unsigned long>(id), static_cast<unsigned>(raw_solenoid));
}

/**
 * 解析 PGN 0x1702（frame 0x16170203）— 倒档电磁阀。
 *
 * 提取字段：
 *   - 当前倒档采样电流  data[4-5] LE uint16, scaling=1, unit=mA  → Drive
 */
void parse_frame_1702(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint16_t raw_solenoid = static_cast<uint16_t>(data[4])
                              | (static_cast<uint16_t>(data[5]) << 8);
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_drive_solenoid_reverse_raw = raw_solenoid;
  g_drive_solenoid_reverse_ms  = now;
  portEXIT_CRITICAL(&g_lock);

  // Serial.printf("[%s] id=0x%08lX drive_solenoid_reverse=%u mA\r\n", kTag,
  //                static_cast<unsigned long>(id), static_cast<unsigned>(raw_solenoid));
}

/**
 * 解析 PGN 0x1703（frame 0x16170303）。
 *
 * 提取字段：
 *   - 当前PTO采样电流  data[4-5] LE uint16, scaling=1, unit=mA  → ObjPto.pto_solenoid
 */
void parse_frame_1703(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  // log_frame(id, data, dlc);

  const uint16_t raw_solenoid = static_cast<uint16_t>(data[4])
                              | (static_cast<uint16_t>(data[5]) << 8);
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_pto_solenoid_raw = raw_solenoid;
  g_pto_last_update_ms = now;
  g_pto_solenoid_last_ms = now;
  portEXIT_CRITICAL(&g_lock);

  // Serial.printf("[%s] id=0x%08lX pto_solenoid=%u mA\r\n", kTag,
  //               static_cast<unsigned long>(id), static_cast<unsigned>(raw_solenoid));
}

/**
 * 解析 PGN 0x1708（frame 0x16170803）— 各档位测量压力。
 *
 * 提取字段：
 *   - 高档当前测量压力  data[0-1] LE uint16, scaling=1, offset=-500, unit=kPa → Drive
 *   - 低档当前测量压力  data[2-3] LE uint16, scaling=1, offset=-500, unit=kPa → Drive
 *   - 倒档当前测量压力  data[4-5] LE uint16, scaling=1, offset=-500, unit=kPa → Drive
 */
void parse_frame_1708(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint16_t raw_pressure_high    = static_cast<uint16_t>(data[0])
                                      | (static_cast<uint16_t>(data[1]) << 8);
  const uint16_t raw_pressure_low     = static_cast<uint16_t>(data[2])
                                      | (static_cast<uint16_t>(data[3]) << 8);
  const uint16_t raw_pressure_reverse = static_cast<uint16_t>(data[4])
                                      | (static_cast<uint16_t>(data[5]) << 8);

  portENTER_CRITICAL(&g_lock);
  g_drive_pressure_high_raw    = raw_pressure_high;
  g_drive_pressure_low_raw     = raw_pressure_low;
  g_drive_pressure_reverse_raw = raw_pressure_reverse;
  portEXIT_CRITICAL(&g_lock);

}

/**
 * 解析 PGN 0x1709（frame 0x16170903）。
 *
 * 提取字段：
 *   - 系统压力    data[0-1] LE uint16, offset=-500, unit=kPa  → ObjSystemStatus.system_pressure
 *   - PTO 压力    data[4-5] LE uint16, offset=-500, unit=kPa  → ObjPto.pto_pressure
 */
void parse_frame_1709(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  // log_frame(id, data, dlc);

  const uint16_t raw_system_pressure = static_cast<uint16_t>(data[0])
                                     | (static_cast<uint16_t>(data[1]) << 8);
  const uint16_t raw_pto_pressure = static_cast<uint16_t>(data[4])
                                  | (static_cast<uint16_t>(data[5]) << 8);

  portENTER_CRITICAL(&g_lock);
  g_system_pressure_raw = raw_system_pressure;
  g_pto_pressure_raw = raw_pto_pressure;
  g_pto_last_update_ms = millis();
  portEXIT_CRITICAL(&g_lock);

}

/**
 * 解析 PGN 0x1712（frame 0x16171203）— 离合踏板与打滑率。
 *
 * 提取字段：
 *   - 离合器打滑率  data[2-3] LE uint16, scaling=0.002, offset=0, unit=% → ObjDrive.clutch_padel_slip
 */
void parse_frame_1712(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 4) return;

  const uint16_t raw_clutch_slip = static_cast<uint16_t>(data[2])
                                 | (static_cast<uint16_t>(data[3]) << 8);

  portENTER_CRITICAL(&g_lock);
  g_drive_clutch_slip_raw = raw_clutch_slip;
  portEXIT_CRITICAL(&g_lock);

  // 物理值 = raw * 0.002 %
  const float slip_pct = static_cast<float>(raw_clutch_slip) * 0.002f;

  // Serial.printf("[%s] id=0x%08lX clutch_slip=%u raw (phys=%.3f %%)\r\n", kTag,
  //               static_cast<unsigned long>(id), static_cast<unsigned>(raw_clutch_slip),
  //               static_cast<double>(slip_pct));
}

/**
 * 解析 PGN 0x1780（frame 0x16178003）— 高档空载点。
 *
 * 提取字段：
 *   - 当前高档空载点3Y  data[5] uint8, scaling=0.1, unit=%
 */
void parse_frame_1780(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint8_t raw_3y = data[5];
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_clutch_high_raw_3y = raw_3y;
  g_clutch_high_last_ms = now;
  portEXIT_CRITICAL(&g_lock);
}

/**
 * 解析 PGN 0x1782（frame 0x16178203）— 低档空载点。
 *
 * 提取字段：
 *   - 当前低档空载点3Y  data[5] uint8, scaling=0.1, unit=%
 */
void parse_frame_1782(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint8_t raw_3y = data[5];
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_clutch_low_raw_3y = raw_3y;
  g_clutch_low_last_ms = now;
  portEXIT_CRITICAL(&g_lock);
}

/**
 * 解析 PGN 0x1784（frame 0x16178403）— 倒档空载点。
 *
 * 提取字段：
 *   - 当前倒档空载点3Y  data[5] uint8, scaling=0.1, unit=%
 */
void parse_frame_1784(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint8_t raw_3y = data[5];
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_clutch_rev_raw_3y = raw_3y;
  g_clutch_rev_last_ms = now;
  portEXIT_CRITICAL(&g_lock);
}

/**
 * 解析 PGN 0x1798（frame 0x16179803）— PTO 起步点。
 *
 * 提取字段：
 *   - 当前 PTO 起步点3Y  data[5] uint8, scaling=0.1, unit=%
 */
void parse_frame_1798(uint32_t id, const uint8_t *data, uint8_t dlc) {
  if (!data || dlc < 6) return;

  const uint8_t raw_3y = data[5];
  const int64_t now = millis();

  portENTER_CRITICAL(&g_lock);
  g_clutch_pto_raw_3y = raw_3y;
  g_clutch_pto_last_ms = now;
  portEXIT_CRITICAL(&g_lock);
}

} // namespace

// ── 公开接口 ─────────────────────────────────────────────

void init() {
  pinMode(static_cast<uint8_t>(kSourceSelectGpio), INPUT);
  delay(2);
  const int level = digitalRead(static_cast<uint8_t>(kSourceSelectGpio));

  Serial.printf("[%s] GPIO15 level=%d\r\n", kTag, level);
  portENTER_CRITICAL(&g_lock);
  g_source_channel = (level == HIGH) ? SourceChannel::kChannel1 : SourceChannel::kChannel0;
  // ----------------------------------------------------------------------------
  g_source_channel = SourceChannel::kChannel1;  // 强制使用 channel1，忽略 GPIO15
  // ----------------------------------------------------------------------------
  g_pto_switch_raw = 0;
  g_pto_lock_raw = 0;
  g_pto_solenoid_raw = 0;
  g_pto_pressure_raw = 0;
  g_pto_last_update_ms = 0;
  g_pto_solenoid_last_ms = 0;

  // SystemStatus 状态复位
  g_system_status_raw          = 0;
  g_system_status_1744_last_ms = 0;
  g_system_pressure_raw        = 0;

  // Drive 状态复位
  g_drive_direction_raw   = 0;
  g_drive_gear_lever_raw  = 0;
  g_drive_lock_raw        = 0;
  g_drive_power_shuttle   = PowerShuttleState::kStop;
  g_drive_1744_last_ms    = 0;
  g_drive_solenoid_high_raw    = 0;
  g_drive_solenoid_low_raw     = 0;
  g_drive_solenoid_reverse_raw = 0;
  g_drive_solenoid_high_ms     = 0;
  g_drive_solenoid_low_ms      = 0;
  g_drive_solenoid_reverse_ms  = 0;
  g_drive_pressure_high_raw    = 0;
  g_drive_pressure_low_raw     = 0;
  g_drive_pressure_reverse_raw = 0;
  g_drive_clutch_slip_raw      = 0;

  // ClutchStartup 状态复位
  g_clutch_high_raw_3y   = 0;
  g_clutch_low_raw_3y    = 0;
  g_clutch_rev_raw_3y    = 0;
  g_clutch_pto_raw_3y    = 0;
  g_clutch_high_last_ms  = 0;
  g_clutch_low_last_ms   = 0;
  g_clutch_rev_last_ms   = 0;
  g_clutch_pto_last_ms   = 0;
  portEXIT_CRITICAL(&g_lock);

  Serial.printf("[%s] GPIO15 level=%d, selected source=%s\r\n", kTag, level, source_channel_name());
}

SourceChannel source_channel() {
  portENTER_CRITICAL(&g_lock);
  const SourceChannel value = g_source_channel;
  portEXIT_CRITICAL(&g_lock);
  return value;
}

const char *source_channel_name() {
  return g_source_channel == SourceChannel::kChannel1 ? "channel1" : "channel0";
}

void process_frame(uint32_t extended_id29, const uint8_t *data, uint8_t dlc) {
  if (!data) return;

  process_product_announce(extended_id29, data, dlc);

  if (!frame_allowed_for_selected_source(extended_id29)) return;

  switch (extended_id29) {
  case 0x16174403u: parse_frame_1744(extended_id29, data, dlc); break;
  case 0x16170003u: parse_frame_1700(extended_id29, data, dlc); break;
  case 0x16170103u: parse_frame_1701(extended_id29, data, dlc); break;
  case 0x16170203u: parse_frame_1702(extended_id29, data, dlc); break;
  case 0x16170303u: parse_frame_1703(extended_id29, data, dlc); break;
  case 0x16170803u: parse_frame_1708(extended_id29, data, dlc); break;
  case 0x16170903u: parse_frame_1709(extended_id29, data, dlc); break;
  case 0x16171203u: parse_frame_1712(extended_id29, data, dlc); break;
  case 0x16178003u: parse_frame_1780(extended_id29, data, dlc); break;
  case 0x16178203u: parse_frame_1782(extended_id29, data, dlc); break;
  case 0x16178403u: parse_frame_1784(extended_id29, data, dlc); break;
  case 0x16179803u: parse_frame_1798(extended_id29, data, dlc); break;
  default: break;
  }
}

ObjSystemStatus get_obj_system_status() {
  portENTER_CRITICAL(&g_lock);
  const uint8_t  raw_sm        = g_system_status_raw;
  const uint16_t raw_pressure  = g_system_pressure_raw;
  const int64_t  last_1744_ms  = g_system_status_1744_last_ms;
  portEXIT_CRITICAL(&g_lock);

  ObjSystemStatus result{};
  result.state_machine   = String(state_machine_to_string(raw_sm));
  result.system_pressure = String(static_cast<int>(raw_pressure) - 500) + " kPa";
  result.system_uptime   = format_uptime(millis());
  result.last_update_ms  = last_1744_ms;

  // 有效条件：0x1744 帧曾经收到，且距今 ≤ 2s
  const int64_t now = millis();
  result.valid = (last_1744_ms > 0) && ((now - last_1744_ms) <= k1744ValidWindowMs);
  return result;
}

ObjPto get_obj_pto() {
  portENTER_CRITICAL(&g_lock);
  const uint8_t pto_switch = g_pto_switch_raw;
  const uint8_t pto_lock = g_pto_lock_raw;
  const uint16_t pto_solenoid = g_pto_solenoid_raw;
  const uint16_t pto_pressure = g_pto_pressure_raw;
  const int64_t last_update_ms = g_pto_last_update_ms;
  const int64_t solenoid_ms = g_pto_solenoid_last_ms;
  portEXIT_CRITICAL(&g_lock);

  ObjPto value{};
  value.pto_switch = (pto_switch == 0) ? String("停止") : String("启动");
  value.pto_lock = (pto_lock == 0) ? String("未锁定") : String("锁定");
  value.pto_solenoid = String(pto_solenoid) + " mA";
  value.pto_pressure = String(static_cast<int>(pto_pressure) - 500) + " kPa";
  value.last_update_ms = last_update_ms;

  // 有效条件：电磁阀电流帧曾经收到，且距今 ≤ 2s
  const int64_t now = millis();
  value.valid = (solenoid_ms > 0) && ((now - solenoid_ms) <= kPtoSolenoidValidWindowMs);
  return value;
}

ObjDrive get_obj_drive() {
  portENTER_CRITICAL(&g_lock);
  const PowerShuttleState pss    = g_drive_power_shuttle;
  const uint8_t drive_lock       = g_drive_lock_raw;
  const uint16_t clutch_slip     = g_drive_clutch_slip_raw;
  const int64_t frame_1744_ms    = g_drive_1744_last_ms;

  // 根据动力换挡状态选择对应的电磁阀电流与时间戳
  uint16_t solenoid_raw = 0;
  int64_t solenoid_ms = 0;
  uint16_t pressure_raw = 0;

  switch (pss) {
    case PowerShuttleState::kForwardHigh:
      solenoid_raw = g_drive_solenoid_high_raw;
      solenoid_ms  = g_drive_solenoid_high_ms;
      pressure_raw = g_drive_pressure_high_raw;
      break;
    case PowerShuttleState::kForwardLow:
      solenoid_raw = g_drive_solenoid_low_raw;
      solenoid_ms  = g_drive_solenoid_low_ms;
      pressure_raw = g_drive_pressure_low_raw;
      break;
    case PowerShuttleState::kReverse:
      solenoid_raw = g_drive_solenoid_reverse_raw;
      solenoid_ms  = g_drive_solenoid_reverse_ms;
      pressure_raw = g_drive_pressure_reverse_raw;
      break;
    case PowerShuttleState::kStop:
    default:
      // Stop 状态下无对应电磁阀，solenoid=0, pressure=0
      break;
  }
  portEXIT_CRITICAL(&g_lock);

  ObjDrive result{};
  result.power_shuttle_state = String(power_shuttle_to_string(pss));
  result.clutch_padel_slip   = String(static_cast<float>(clutch_slip) * 0.002f, 3) + " %";
  result.drive_lock          = (drive_lock == 0) ? String("未锁定") : String("锁定");
  result.drive_solenoid      = String(solenoid_raw) + " mA";
  result.drive_pressure      = String(static_cast<int>(pressure_raw) - 500) + " kPa";
  result.last_update_ms      = frame_1744_ms;

  const int64_t now = millis();

  if (pss == PowerShuttleState::kStop) {
    // Stop 状态：无电磁阀帧，以 0x1744 帧为有效性依据
    result.valid = (frame_1744_ms > 0) && ((now - frame_1744_ms) <= k1744ValidWindowMs);
  } else {
    // 非 Stop 状态：对应档位电磁阀电流帧必须曾经收到，且距今 ≤ 2s
    result.valid = (solenoid_ms > 0) && ((now - solenoid_ms) <= kDriveSolenoidValidWindowMs);
  }

  return result;
}

ObjClutchStartup get_obj_clutch_startup() {
  portENTER_CRITICAL(&g_lock);
  const uint8_t  raw_high  = g_clutch_high_raw_3y;
  const uint8_t  raw_low   = g_clutch_low_raw_3y;
  const uint8_t  raw_rev   = g_clutch_rev_raw_3y;
  const uint8_t  raw_pto   = g_clutch_pto_raw_3y;
  const int64_t  last_high = g_clutch_high_last_ms;
  const int64_t  last_low  = g_clutch_low_last_ms;
  const int64_t  last_rev  = g_clutch_rev_last_ms;
  const int64_t  last_pto  = g_clutch_pto_last_ms;
  portEXIT_CRITICAL(&g_lock);

  ObjClutchStartup result{};

  /** 格式化 3Y 值：raw > 0xFA (250) 视为无效，否则 physical = raw * 0.1 % */
  auto fmt_3y = [](uint8_t raw, int64_t last_ms, bool &valid_out) -> String {
    const int64_t now = millis();
    constexpr int64_t kValidWindowMs = 2000;
    if (raw > 0xFA || last_ms <= 0 || (now - last_ms) > kValidWindowMs) {
      valid_out = false;
      return String("—");
    }
    valid_out = true;
    return String(static_cast<float>(raw) * 0.1f, 1) + " %";
  };

  bool high_ok = false, low_ok = false, rev_ok = false, pto_ok = false;
  result.high_clutch_startup = fmt_3y(raw_high, last_high, high_ok);
  result.low_clutch_startup  = fmt_3y(raw_low,  last_low,  low_ok);
  result.rev_clutch_startup  = fmt_3y(raw_rev,  last_rev,  rev_ok);
  result.pto_clutch_startup  = fmt_3y(raw_pto,  last_pto,  pto_ok);
  result.valid = high_ok || low_ok || rev_ok || pto_ok;
  result.last_update_ms = last_high;  // 最近一次高档帧的时间戳

  return result;
}

uint32_t extract_bits_le(const uint8_t *data, size_t len, size_t start_bit, uint8_t bit_count) {
  if (!data || bit_count == 0 || bit_count > 32 || start_bit + bit_count > len * 8) {
    return 0;
  }

  uint32_t value = 0;
  for (uint8_t i = 0; i < bit_count; ++i) {
    const size_t bit_index = start_bit + i;
    const uint8_t bit = static_cast<uint8_t>((data[bit_index / 8] >> (bit_index % 8)) & 0x01u);
    value |= static_cast<uint32_t>(bit) << i;
  }
  return value;
}

uint8_t extract_u8_mask_shift(uint8_t byte_value, uint8_t mask, uint8_t shift) {
  return static_cast<uint8_t>((byte_value & mask) >> shift);
}

} // namespace gateway::can_parsed_data
