/**
 * @file can_tx.cpp
 * @brief CAN 发送侧：队列缓存 `twai_message_t`，独立任务调用 `twai_transmit` 出队发送。
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
        QueueHandle_t g_q;
        constexpr int kDepth = 48;

        /** 发送任务：阻塞取队列并调用 TWAI 驱动发送。 */
        void task(void *arg) {
            (void)arg;
            twai_message_t m;
            for (;;) {
                if (xQueueReceive(g_q, &m, portMAX_DELAY) == pdTRUE) {
                    esp_err_t e = twai_transmit(&m, pdMS_TO_TICKS(200));
                    if (e == ESP_OK) {
                        statistics::add_can_tx(1);
                    } else {
                        Serial.printf("[CAN TX] twai_transmit failed: %s extd=%u id=0x%08x dlc=%u\n",
                            esp_err_to_name(e), static_cast<unsigned>(m.extd),
                            static_cast<unsigned>(m.identifier & 0x1FFFFFFFu),
                            static_cast<unsigned>(m.data_length_code));
                    }
                }
            }
        }
    } // namespace

    void init() {
        if (g_q) {
            return;
        }
        g_q = xQueueCreate(kDepth, sizeof(twai_message_t));
        xTaskCreatePinnedToCore(task, "can_tx", 4096, nullptr, 3, nullptr, 1);
    }

    bool enqueue(const twai_message_t &msg) {
        if (!g_q) {
            return false;
        }
        return xQueueSend(g_q, &msg, pdMS_TO_TICKS(20)) == pdTRUE;
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
