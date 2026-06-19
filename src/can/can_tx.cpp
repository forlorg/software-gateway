/**
 * @file can_tx.cpp
 * @brief CAN 发送侧：高/普通两级队列缓存 `twai_message_t`，独立任务优先发送高优先级帧。
 */

#include "can_tx.h"

#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <driver/twai.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "system/statistics.h"

namespace gateway::can_tx {

    namespace {
        QueueHandle_t g_q_high{};
        QueueHandle_t g_q_normal{};

        constexpr int kHighDepth = 32;
        constexpr int kNormalDepth = 48;
        const TickType_t kHighEnqueueWait = pdMS_TO_TICKS(1);
        const TickType_t kNormalEnqueueWait = pdMS_TO_TICKS(20);
        const TickType_t kCloudEnqueueWait = 0;
        const TickType_t kTransmitWait = pdMS_TO_TICKS(200);

        bool send_one(const twai_message_t &m) {
            const esp_err_t e = twai_transmit(&m, kTransmitWait);
            if (e == ESP_OK) {
                statistics::add_can_tx(1);
                return true;
            }

            Serial.printf("[CAN TX] twai_transmit failed: %s extd=%u id=0x%08x dlc=%u\n",
                esp_err_to_name(e), static_cast<unsigned>(m.extd),
                static_cast<unsigned>(m.identifier & 0x1FFFFFFFu),
                static_cast<unsigned>(m.data_length_code));
            statistics::add_can_tx_transmit_failures(1);
            return false;
        }

        bool receive_high(twai_message_t &m) {
            return g_q_high && xQueueReceive(g_q_high, &m, 0) == pdTRUE;
        }

        bool receive_normal(twai_message_t &m) {
            return g_q_normal && xQueueReceive(g_q_normal, &m, 0) == pdTRUE;
        }

        /** 发送任务：高优先级队列永远优先；普通队列只在高优先级暂空时发送。 */
        void task(void *arg) {
            (void)arg;
            twai_message_t m{};
            for (;;) {
                // 先无等待清高优先级积压。
                if (receive_high(m)) {
                    send_one(m);
                    continue;
                }

                // 短等高优先级，保证 ADC 实时帧不被普通云端命令长时间压住。
                if (g_q_high && xQueueReceive(g_q_high, &m, pdMS_TO_TICKS(1)) == pdTRUE) {
                    send_one(m);
                    continue;
                }

                // 高优先级暂空时才发普通队列。
                if (receive_normal(m)) {
                    send_one(m);
                    continue;
                }

                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        bool enqueue_to(QueueHandle_t q, const twai_message_t &msg, TickType_t wait_ticks) {
            if (!q) {
                return false;
            }
            return xQueueSend(q, &msg, wait_ticks) == pdTRUE;
        }
    } // namespace

    void init() {
        if (g_q_high && g_q_normal) {
            return;
        }

        g_q_high = xQueueCreate(kHighDepth, sizeof(twai_message_t));
        g_q_normal = xQueueCreate(kNormalDepth, sizeof(twai_message_t));

        xTaskCreatePinnedToCore(task, "can_tx", 4096, nullptr, 4, nullptr, 1);
    }

    bool enqueue(const twai_message_t &msg) {
        const bool ok = enqueue_to(g_q_normal, msg, kNormalEnqueueWait);
        if (!ok) {
            statistics::add_can_tx_queue_drops(1);
        }
        return ok;
    }

    bool enqueue_high(const twai_message_t &msg) {
        const bool ok = enqueue_to(g_q_high, msg, kHighEnqueueWait);
        if (!ok) {
            statistics::add_can_tx_high_queue_drops(1);
            statistics::add_can_tx_queue_drops(1);
        }
        return ok;
    }

    bool enqueue_cloud_command(const twai_message_t &msg) {
        const bool ok = enqueue_to(g_q_normal, msg, kCloudEnqueueWait);
        if (!ok) {
            statistics::add_mqtt_downlink_can_drops(1);
            statistics::add_can_tx_queue_drops(1);
        }
        return ok;
    }

    bool send_clutch_startup(const char *clutch, const char *value_str) {
        if (!clutch || !value_str || !value_str[0]) {
            return false;
        }

        // ── 查找目标 CAN ID ──────────────────────────────────
        uint32_t can_id = 0;
        if (std::strcmp(clutch, "high") == 0) {
            can_id = 0x06188060u;  // PGN 0x1880, priority=0x06, SA=0x60
        } else if (std::strcmp(clutch, "low") == 0) {
            can_id = 0x06188260u;  // PGN 0x1882
        } else if (std::strcmp(clutch, "rev") == 0) {
            can_id = 0x06188460u;  // PGN 0x1884
        } else if (std::strcmp(clutch, "pto") == 0) {
            can_id = 0x06189860u;  // PGN 0x1898
        } else {
            return false;
        }

        // ── 解析物理值 → raw ────────────────────────────────
        const float physical = std::strtof(value_str, nullptr);
        // raw = physical / 0.1 = physical * 10
        const float raw_f = physical / 0.1f;
        if (raw_f < 0.0f || raw_f > 250.0f) {
            Serial.printf("[CAN TX] send_clutch_startup %s: value out of range (raw=%.1f)\n",
                clutch, static_cast<double>(raw_f));
            return false;
        }
        const uint8_t raw_3y = static_cast<uint8_t>(raw_f + 0.5f);  // 四舍五入
        if (raw_3y > 0xFA) {
            Serial.printf("[CAN TX] send_clutch_startup %s: raw %u > 0xFA invalid\n",
                clutch, static_cast<unsigned>(raw_3y));
            return false;
        }

        // ── 构造 CAN 帧 ────────────────────────────────────
        twai_message_t msg{};
        msg.extd = true;
        msg.identifier = can_id;
        msg.data_length_code = 8;
        // 非 3Y 字节全部填 0xFF
        for (uint8_t i = 0; i < 8; ++i) {
            msg.data[i] = 0xFF;
        }
        msg.data[5] = raw_3y;  // 3Y 在 byte_start=5

        Serial.printf("[CAN TX] send_clutch_startup %s: id=0x%08lX data[5]=%u (phys=%.1f %%)\n",
            clutch, static_cast<unsigned long>(can_id),
            static_cast<unsigned>(raw_3y), static_cast<double>(physical));

        return enqueue(msg);
    }

    bool flash_firmware() {
        // ── 构造 CAN ID: PGN 0x1850, priority=0x06, SA=0x60 (网关→TCU) ──
        constexpr uint32_t kCanId = 0x06185060u;

        // ── 构造 CAN 帧 ────────────────────────────────────
        twai_message_t msg{};
        msg.extd = true;
        msg.identifier = kCanId;
        msg.data_length_code = 8;
        // data[0] = 0x10 (bit 4 置位: FLASH 刷写 = 执行命令), 其余字节填 0
        for (uint8_t i = 0; i < 8; ++i) {
            msg.data[i] = 0x00;
        }
        msg.data[0] = 0x10;

        Serial.printf("[CAN TX] flash_firmware id=0x%08lX data[0]=0x%02X\n",
            static_cast<unsigned long>(kCanId),
            static_cast<unsigned>(msg.data[0]));

        return enqueue(msg);
    }

} // namespace gateway::can_tx
