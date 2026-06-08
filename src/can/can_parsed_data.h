#pragma once

/**
 * @file can_parsed_data.h
 * @brief 指定 CAN 帧解析、缓存与结构体查询接口。
 */

#include <cstddef>
#include <cstdint>

#include <Arduino.h>

namespace gateway::can_parsed_data {

enum class SourceChannel : uint8_t {
  kChannel0 = 0,
  kChannel1 = 1,
};

struct ObjSystemStatus {
  bool valid{};
  int64_t last_update_ms{};
  String state_machine{};     // 可直接显示
  String system_pressure{};   // 可直接显示，已换算单位
  String system_uptime{};     // 可直接显示，系统运行时间
};

struct ObjDrive {
  bool valid{};
  int64_t last_update_ms{};
  String power_shuttle_state{};  // 可直接显示
  String clutch_padel_slip{};    // 可直接显示，已换算单位
  String drive_lock{};           // 可直接显示
  String drive_solenoid{};       // 可直接显示，已附带单位
  String drive_pressure{};       // 可直接显示，已换算单位
};

struct ObjPto {
  bool valid{};
  int64_t last_update_ms{};
  String pto_switch{};     // 可直接显示
  String pto_lock{};       // 可直接显示
  String pto_solenoid{};   // 可直接显示，已附带单位
  String pto_pressure{};   // 可直接显示，已换算单位
};

struct ObjClutchStartup {
  bool valid{};
  int64_t last_update_ms{};
  String high_clutch_startup{}; // 可直接显示，已换算单位
  String low_clutch_startup{};  // 可直接显示，已换算单位
  String rev_clutch_startup{};  // 可直接显示，已换算单位
  String pto_clutch_startup{};  // 可直接显示，已换算单位
};

void init();
SourceChannel source_channel();
const char *source_channel_name();

void process_frame(uint32_t extended_id29, const uint8_t *data, uint8_t dlc);

ObjSystemStatus get_obj_system_status();
ObjDrive get_obj_drive();
ObjPto get_obj_pto();
ObjClutchStartup get_obj_clutch_startup();

uint32_t extract_bits_le(const uint8_t *data, size_t len, size_t start_bit, uint8_t bit_count);
uint8_t extract_u8_mask_shift(uint8_t byte_value, uint8_t mask, uint8_t shift);

} // namespace gateway::can_parsed_data
