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
        /** mqtt_uplink 聚合后的单批最大 payload；与 mqtt_uplink::config::kMaxBatchBytes 保持一致。 */
        constexpr size_t kPendingPublishMaxBytes = 4096;
        /** MQTT owner 待发布队列深度；满时丢旧保新。 */
        constexpr unsigned kPendingPublishQueueDepth = 4;
    } // namespace config

    void init();
    void loop_poll();
    void apply_config_from_store();
    void notify_product_id_changed();

    /** 返回 MQTT owner 更新的缓存连接状态；不跨任务直接访问 PubSubClient。 */
    bool is_connected();
    /** 是否曾成功连上 MQTT（用于 RingBuffer：首次成功前不缓存） */
    bool had_successful_connection();

    /**
     * @brief 将车辆上行批数据交给 MQTT owner 任务发布。
     *
     * 不直接调用 PubSubClient，队列满时丢旧保新。
     */
    bool enqueue_vehicle_upload(const uint8_t *data, size_t len);

    /** 兼容旧接口：现在等价于 enqueue_vehicle_upload()，不再跨任务直接 publish。 */
    bool publish_vehicle_upload(const uint8_t *data, size_t len);

    /** 仅 MQTT owner 内部可安全直接调用；外部任务不要调用。 */
    bool publish_blob(const char *topic_override, const uint8_t *data, size_t len);

    /** 当前上行主题 `topic_ps_pro/vehicle_upload/<id>`；未就绪时返回空字符串。 */
    const char *vehicle_upload_topic();
    /** 当前下行订阅主题 `topic_ps_pro/vehicle_download/<id>`；未就绪时返回空字符串。 */
    const char *vehicle_download_topic();

} // namespace gateway::mqtt_manager
