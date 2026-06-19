/**
 * @file can_rx.cpp
 * @brief CAN 接收 FreeRTOS 任务：统计、转交解析模块、编码 AT 行、USB CDC 镜像（队列 + 独立任务）、条件 MQTT 上行。
 */

#include "can_rx.h"
#include "can/can_parsed_data.h"
#include "can/can_traffic_stats.h"
#include "protocol/at_protocol.h"
#include "system/time_sync.h"

#include "task/mqtt_uplink.h"
#include "system/statistics.h"

#include <Arduino.h>
#include <cstring>
#include <driver/twai.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

    struct MirrorLine {
        uint16_t len;
        uint8_t bytes[gateway::at_protocol::kAtLineMaxBytes];
    };

    QueueHandle_t g_mirror_q{};

    /**
     * USB CDC 镜像发送任务：阻塞等待 CAN 接收任务投递的 AT 行，
     * 分段写入 USB CDC/Serial，写失败时短暂让出 CPU 后继续发送。
     */
    void mirror_tx_task(void *) {
        MirrorLine item{};
        for (;;) {
            if (xQueueReceive(g_mirror_q, &item, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            size_t off = 0;
            while (off < item.len) {
                const size_t remain = item.len - off;
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
                const int w = Serial.write(item.bytes + off, remain);
#else
                const int w = USBSerial.write(item.bytes + off, remain);
#endif
                if (w > 0) {
                    off += static_cast<size_t>(w);
                    continue;
                }
                vTaskDelay(1);
            }
        }
    }

    void mirror_line(const uint8_t *buf, size_t len) {
        if (!g_mirror_q || !buf || len == 0 || len > gateway::at_protocol::kAtLineMaxBytes) {
            return;
        }
        MirrorLine item{};
        item.len = static_cast<uint16_t>(len);
        memcpy(item.bytes, buf, len);
        if (xQueueSend(g_mirror_q, &item, 0) != pdTRUE) {
            gateway::statistics::add_serial_mirror_queue_drops(1);
        }
    }

    /**
     * CAN 接收任务：阻塞接收 TWAI 帧，更新流量/业务统计，识别 product 宣告并刷新
     * 设备上下文，将 CAN 帧按 AT 协议编码后投递到 USB CDC 镜像队列，时间同步可用时送入 MQTT 上行缓冲。
     */
    void rx_task(void *) {
        twai_message_t msg{};
        uint8_t line[gateway::at_protocol::kAtLineMaxBytes];

        for (;;) {
            if (twai_receive(&msg, pdMS_TO_TICKS(200)) != ESP_OK) {
                continue;
            }

            gateway::can_traffic_stats::record_rx_frame(msg.data_length_code);
            gateway::statistics::add_can_rx(1);

            if (msg.extd) {
                const uint32_t cid = msg.identifier & 0x1FFFFFFFu;
                gateway::can_parsed_data::process_frame(cid, msg.data, msg.data_length_code);
            }

            uint32_t ts_pack{};
            bool mqtt_ok = false;
            if (gateway::time_sync::can_use_wall_timestamp_for_upload()) {
                ts_pack = gateway::time_sync::pack_mqtt_wall_timestamp();
                if (ts_pack == 0) {
                    gateway::statistics::add_dropped(1);
                    continue;
                }
                mqtt_ok = true;
            } else {
                ts_pack = gateway::time_sync::pack_boot_relative_ms27();
            }

            size_t nw =
            gateway::at_protocol::encode_at_from_twai(line, sizeof(line), ts_pack, msg);
            if (nw == 0) {
                continue;
            }

            mirror_line(line, nw);
            if (mqtt_ok) {
                gateway::mqtt_uplink::offer_at_binary(line, static_cast<uint16_t>(nw));
            }
        }
    }
} // namespace

void init_serial_mirror() {
    if (g_mirror_q) {
        return;
    }
    /* USB CDC 由 `main` 中 `USBSerial.begin` 初始化；此处仅挂队列与发送任务。 */
    g_mirror_q = xQueueCreate(static_cast<UBaseType_t>(gateway::can_rx::kSerialMirrorQueueDepth),
        sizeof(MirrorLine));
    xTaskCreatePinnedToCore(mirror_tx_task, "usb_cdc_mirr", gateway::can_rx::kMirrorTxTaskStackBytes, nullptr,
        static_cast<UBaseType_t>(gateway::can_rx::kMirrorTxTaskPriority), nullptr,
        static_cast<BaseType_t>(gateway::can_rx::kMirrorTxTaskCore));
}

void can_rx_start_task() {
    gateway::can_parsed_data::init();
    xTaskCreatePinnedToCore(rx_task, "can_rx", gateway::can_rx::kTaskStackBytes, nullptr,
        static_cast<UBaseType_t>(gateway::can_rx::kTaskPriority), nullptr,
        static_cast<BaseType_t>(gateway::can_rx::kTaskCore));
}
