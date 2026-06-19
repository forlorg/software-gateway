/**
 * @file mqtt_uplink.cpp
 * @brief MQTT 上行聚合任务：从 RingBuffer 取 AT 行、按批交给 MQTT owner 发布。
 */

#include "mqtt_uplink.h"
#include "can/can_traffic_stats.h"
#include "network/mqtt_manager.h"
#include "protocol/at_protocol.h"
#include "system/statistics.h"
#include "utils/packet_ringbuffer.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>


namespace gateway::mqtt_uplink {

    namespace {
        /** 首次成功发布时打印一次 topic，便于现场确认配置。 */
        void log_vehicle_upload_topic_once() {
            static bool logged = false;
            if (logged) {
                return;
            }
            logged = true;
            Serial.printf("[MQTT uplink] publish topic=%s (first publish only)\r\n",
                mqtt_manager::vehicle_upload_topic());
        }
    } // namespace

    namespace {

        uint8_t g_mem[config::kRingBufferBytes];
        SemaphoreHandle_t g_mu{};
        PacketRingBuffer *g_ring{nullptr};

        /** 懒初始化互斥与环形缓冲单例。 */
        void ensure_ring() {
            if (!g_mu) {
                g_mu = xSemaphoreCreateMutex();
            }
            if (!g_ring) {
                static PacketRingBuffer inst(g_mem, sizeof(g_mem), g_mu);
                g_ring = &inst;
            }
        }

        /** 将已拼好的批缓冲区交给 MQTT owner；不直接访问 PubSubClient。 */
        static void enqueue_batch(const uint8_t *batch, size_t pos) {
            if (pos == 0) {
                return;
            }
            if (mqtt_manager::enqueue_vehicle_upload(batch, pos)) {
                log_vehicle_upload_topic_once();
            } else {
                statistics::add_mqtt_publish_queue_drops(1);
            }
        }

        /**
         * MQTT 上行聚合任务：从共享环形缓冲区取出 CAN 转换后的 AT 行，按高/低水位
         * 切换聚合周期，MQTT 连接可用时批量发布并累计上行流量统计。
         */
        void agg_task(void *) {
            ensure_ring();
            PacketRingBuffer &ring = *g_ring;
            uint8_t batch[config::kMaxBatchBytes];
            for (;;) {
                uint32_t period = (ring.bytes_used() > config::kHighWaterBytes) ? config::kAggregateMsFast
                : config::kAggregateMsNormal;
                vTaskDelay(pdMS_TO_TICKS(period));

                if (!mqtt_manager::is_connected()) {
                    continue;
                }

                size_t pos = 0;
                for (;;) {
                    uint8_t one[gateway::at_protocol::kAtLineMaxBytes];
                    uint16_t n = ring.pop(one, static_cast<uint16_t>(sizeof(one)));
                    if (n == 0) {
                        break;
                    }
                    if (pos + n > sizeof(batch)) {
                        enqueue_batch(batch, pos);
                        pos = 0;
                        if (n > sizeof(batch)) {
                            continue;
                        }
                    }
                    memcpy(batch + pos, one, n);
                    pos += n;
                }
                if (pos > 0) {
                    enqueue_batch(batch, pos);
                }
                can_traffic_stats::on_aggregate_tick();
            }
        }
    } // namespace

    void begin() {
        ensure_ring();
        // MQTT 聚合不是实时链路，放 Core0 低优先级，避免与 CAN/ADC 实时任务抢 Core1。
        xTaskCreatePinnedToCore(agg_task, "mqtt_agg", 8192, nullptr, 1, nullptr, 0);
    }

    bool offer_at_binary(const uint8_t *line, uint16_t len) {
        ensure_ring();
        if (!line || len == 0) {
            return false;
        }
        if (!mqtt_manager::had_successful_connection() && !mqtt_manager::is_connected()) {
            statistics::add_dropped(1);
            return false;
        }
        // CAN RX 是实时主链路；MQTT 上行不能等 mutex，拿不到锁或空间不足就丢。
        // PacketRingBuffer 在空间不足时会丢旧保新。
        bool ok = g_ring->push_nonblocking(line, len);
        if (!ok) {
            statistics::add_mqtt_uplink_queue_drops(1);
        }
        return ok;
    }

    uint32_t ring_used_bytes() { return static_cast<uint32_t>(g_ring->bytes_used()); }

} // namespace gateway::mqtt_uplink
