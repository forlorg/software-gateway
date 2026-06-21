/**
 * @file at_frame_dispatcher.cpp
 * @brief CAN 格式帧的统一 AT 编码与 USB/MQTT 非阻塞扇出。
 */

#include "transport/at_frame_dispatcher.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "protocol/at_protocol.h"
#include "system/statistics.h"
#include "system/time_sync.h"
#include "task/mqtt_uplink.h"

namespace gateway::at_frame_dispatcher {

    namespace {

        struct MirrorLine {
            uint16_t len;
            uint8_t bytes[at_protocol::kAtLineMaxBytes];
        };

        QueueHandle_t g_mirror_q{};
        TaskHandle_t g_mirror_task{};

        /**
         * USB CDC 镜像发送任务：阻塞等待完整 AT 行。
         *
         * USB 主机不读取时，单条写入最多占用 20ms；超时后丢弃当前镜像帧，
         * 避免 USB 故障反压 CAN RX、ADC 与网络链路。
         */
        void mirror_tx_task(void *) {
            constexpr uint32_t kItemWriteTimeoutMs = 20;
            constexpr size_t kYieldEveryBytes = 512;

            MirrorLine item{};
            for (;;) {
                if (xQueueReceive(g_mirror_q, &item, portMAX_DELAY) != pdTRUE) {
                    continue;
                }

                const uint32_t started_ms = millis();
                size_t offset = 0;
                size_t written_since_yield = 0;

                while (offset < item.len) {
                    if (millis() - started_ms > kItemWriteTimeoutMs) {
                        statistics::add_serial_mirror_queue_drops(1);
                        break;
                    }

                    const size_t remaining = item.len - offset;
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
                    const int written = Serial.write(item.bytes + offset, remaining);
#else
                    const int written = USBSerial.write(item.bytes + offset, remaining);
#endif
                    if (written > 0) {
                        const size_t count = static_cast<size_t>(written);
                        offset += count;
                        written_since_yield += count;
                        if (written_since_yield >= kYieldEveryBytes) {
                            written_since_yield = 0;
                            taskYIELD();
                        }
                        continue;
                    }

                    vTaskDelay(1);
                }

                taskYIELD();
            }
        }

        void enqueue_usb_mirror(const uint8_t *line, size_t length) {
            if (!g_mirror_q || !line || length == 0 ||
                length > at_protocol::kAtLineMaxBytes) {
                return;
            }

            MirrorLine item{};
            item.len = static_cast<uint16_t>(length);
            memcpy(item.bytes, line, length);

            if (xQueueSend(g_mirror_q, &item, 0) != pdTRUE) {
                statistics::add_serial_mirror_queue_drops(1);
            }
        }

    } // namespace

    void begin() {
        if (g_mirror_q && g_mirror_task) {
            return;
        }

        if (!g_mirror_q) {
            g_mirror_q = xQueueCreate(
                static_cast<UBaseType_t>(kSerialMirrorQueueDepth), sizeof(MirrorLine));
            if (!g_mirror_q) {
                Serial.println("[AT DISP] FAIL create USB mirror queue");
                return;
            }
        }

        if (!g_mirror_task) {
            const BaseType_t result = xTaskCreatePinnedToCore(
                mirror_tx_task, "usb_cdc_mirr", kMirrorTxTaskStackBytes, nullptr,
                static_cast<UBaseType_t>(kMirrorTxTaskPriority), &g_mirror_task,
                static_cast<BaseType_t>(kMirrorTxTaskCore));

            if (result != pdPASS) {
                g_mirror_task = nullptr;
                vQueueDelete(g_mirror_q);
                g_mirror_q = nullptr;
                Serial.println("[AT DISP] FAIL create USB mirror task");
                return;
            }
        }

        Serial.printf("[AT DISP] ready USB queue=%u core=%d priority=%u\r\n",
            static_cast<unsigned>(kSerialMirrorQueueDepth), kMirrorTxTaskCore,
            static_cast<unsigned>(kMirrorTxTaskPriority));
    }

    void dispatch(const twai_message_t &msg) {
        uint8_t line[at_protocol::kAtLineMaxBytes];

        bool mqtt_allowed = false;
        uint32_t timestamp = 0;

        if (time_sync::can_use_wall_timestamp_for_upload()) {
            timestamp = time_sync::pack_mqtt_wall_timestamp();
            mqtt_allowed = timestamp != 0;

            // 墙钟瞬时不可用时，USB 仍使用启动相对时间继续镜像；仅跳过 MQTT。
            if (!mqtt_allowed) {
                statistics::add_dropped(1);
                timestamp = time_sync::pack_boot_relative_ms27();
            }
        } else {
            timestamp = time_sync::pack_boot_relative_ms27();
        }

        const size_t length =
            at_protocol::encode_at_from_twai(line, sizeof(line), timestamp, msg);
        if (length == 0) {
            statistics::add_at_encode_failures(1);
            return;
        }

        statistics::add_at_dispatch_frames(1);
        enqueue_usb_mirror(line, length);

        if (mqtt_allowed) {
            mqtt_uplink::offer_at_binary(line, static_cast<uint16_t>(length));
        }
    }

} // namespace gateway::at_frame_dispatcher
