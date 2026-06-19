/**
 * @file can_ring_flush_task.cpp
 * @brief 实验用：无 MQTT 时仍按相同 Ring 参数消费 AT 行并累计统计（不真正发布）。
 */

#include "can_ring_flush_task.h"
#include "can/can_traffic_stats.h"
#include "protocol/at_protocol.h"
#include "utils/packet_ringbuffer.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway::can_ring_flush {

    namespace {

        uint8_t g_mem[config::kRingBufferBytes];
        SemaphoreHandle_t g_mu{};
        gateway::PacketRingBuffer *g_ring{nullptr};

        void ensure_ring() {
            if (!g_mu) {
                g_mu = xSemaphoreCreateMutex();
            }
            if (!g_ring) {
                static gateway::PacketRingBuffer inst(g_mem, sizeof(g_mem), g_mu);
                g_ring = &inst;
            }
        }

        static void flush_batch_bytes(const uint8_t *batch, size_t pos) {
            if (pos == 0) {
                return;
            }
            gateway::can_traffic_stats::record_uplink_bytes(static_cast<uint32_t>(pos));
        }

        void flush_task(void *) {
            ensure_ring();
            gateway::PacketRingBuffer &ring = *g_ring;
            uint8_t batch[config::kMaxBatchBytes];
            for (;;) {
                uint32_t period = (ring.bytes_used() > config::kHighWaterBytes) ? config::kAggregateMsFast
                : config::kAggregateMsNormal;
                vTaskDelay(pdMS_TO_TICKS(period));

                size_t pos = 0;
                for (;;) {
                    uint8_t one[gateway::at_protocol::kAtLineMaxBytes];
                    uint16_t n = ring.pop(one, static_cast<uint16_t>(sizeof(one)));
                    if (n == 0) {
                        break;
                    }
                    if (pos + n > sizeof(batch)) {
                        flush_batch_bytes(batch, pos);
                        pos = 0;
                        if (n > sizeof(batch)) {
                            continue;
                        }
                    }
                    memcpy(batch + pos, one, n);
                    pos += n;
                }
                if (pos > 0) {
                    flush_batch_bytes(batch, pos);
                }

                gateway::can_traffic_stats::on_aggregate_tick();
            }
        }
    } // namespace

    void begin() {
        ensure_ring();
        xTaskCreatePinnedToCore(flush_task, "can_ring_flush", config::kTaskStackBytes, nullptr,
            static_cast<UBaseType_t>(config::kTaskPriority), nullptr,
            static_cast<BaseType_t>(config::kTaskCore));
    }

    bool offer_at_binary(const uint8_t *line, uint16_t len) {
        ensure_ring();
        if (!line || len == 0) {
            return false;
        }
        return g_ring->push(line, len);
    }

    uint32_t ring_used_bytes() { return static_cast<uint32_t>(g_ring->bytes_used()); }

} // namespace gateway::can_ring_flush
