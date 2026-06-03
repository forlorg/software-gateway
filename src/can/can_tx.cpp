/**
 * @file can_tx.cpp
 * @brief CAN 发送侧：队列缓存 `twai_message_t`，独立任务调用 `twai_transmit` 出队发送。
 */

#include "can_tx.h"

#include <Arduino.h>
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

} // namespace gateway::can_tx
