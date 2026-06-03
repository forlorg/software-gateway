#pragma once

/**
 * @file mqtt_manager.h
 * @brief PubSubClient 封装：连接状态、发布订阅与 product_id 变更通知。
 */

#include <cstddef>
#include <cstdint>

namespace gateway::mqtt_manager {

/** PubSubClient 动态缓冲，须 ≥ `mqtt_uplink::config::kMaxBatchBytes` + MQTT 头开销。 */
namespace config {
constexpr unsigned kPubSubBufferBytes = 8192;
} // namespace config

void init();
void loop_poll();
void apply_config_from_store();
void notify_product_id_changed();

bool is_connected();
/** 是否曾成功连上 MQTT（用于 RingBuffer：首次成功前不缓存） */
bool had_successful_connection();

bool publish_blob(const char *topic_override, const uint8_t *data, size_t len);
bool publish_vehicle_upload(const uint8_t *data, size_t len);

/** 当前上行主题 `topic_ps_pro/vehicle_upload/<id>`；未就绪时返回空字符串。 */
const char *vehicle_upload_topic();
/** 当前下行订阅主题 `topic_ps_pro/vehicle_download/<id>`；未就绪时返回空字符串。 */
const char *vehicle_download_topic();

} // namespace gateway::mqtt_manager
