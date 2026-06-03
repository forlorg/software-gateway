/**
 * @file can_rx.cpp
 * @brief CAN 接收 FreeRTOS 任务：解析 product 宣告、编码 AT 行、USB CDC 镜像（队列 + 独立任务）、条件 MQTT 上行。
 */

#include "can_rx.h"
#include "system/gateway_context.h"
#include "can/can_traffic_stats.h"
#include "protocol/at_protocol.h"
#include "system/time_sync.h"

#include "network/mqtt_manager.h"
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

void rx_task(void *) {
  twai_message_t msg{};
  uint8_t line[gateway::at_protocol::kAtLineMaxBytes];

  for (;;) {
    if (twai_receive(&msg, pdMS_TO_TICKS(200)) != ESP_OK) {
      continue;
    }

    gateway::can_traffic_stats::record_rx_frame(msg.data_length_code);
    gateway::statistics::add_can_rx(1);

    uint32_t cid = msg.identifier & (msg.extd ? 0x1FFFFFFFu : 0x7FFu);
    if (msg.extd && gateway::can_rx::extended_id_is_product_announce(cid)) {
      if (gateway::ctx::set_product_id_from_payload_le(msg.data, msg.data_length_code)) {
        gateway::mqtt_manager::notify_product_id_changed();
      }
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
  xTaskCreatePinnedToCore(rx_task, "can_rx", gateway::can_rx::kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(gateway::can_rx::kTaskPriority), nullptr,
                          static_cast<BaseType_t>(gateway::can_rx::kTaskCore));
}
