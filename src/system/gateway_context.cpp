/**
 * @file system/gateway_context.cpp
 * @brief 网关全局上下文：product_id 互斥保存与变更检测（驱动 MQTT topic 重绑）。
 */

#include "gateway_context.h"
#include "protocol/at_protocol.h"
#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace gateway::ctx {

namespace {
SemaphoreHandle_t g_mu;
char g_product_hex[9];
bool g_has_pid;
uint32_t g_last_product_raw;

/** 将 32 位值格式化为 8 位小写十六进制 + 终止符。 */
void u32_to_hex_lower(uint32_t v, char out9[9]) {
  static const char *hex = "0123456789abcdef";
  for (int i = 7; i >= 0; --i) {
    out9[7 - i] = hex[(v >> (i * 4)) & 0xF];
  }
  out9[8] = '\0';
}
} // namespace

void init() {
  if (!g_mu) {
    g_mu = xSemaphoreCreateMutex();
  }
  g_has_pid = false;
  g_product_hex[0] = '\0';
  g_last_product_raw = 0;
}

bool has_product_id() { return g_has_pid; }

void get_product_id_hex(char *out9) {
  if (!out9) {
    return;
  }
  if (xSemaphoreTake(g_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
    memcpy(out9, g_product_hex, 9);
    xSemaphoreGive(g_mu);
  } else {
    out9[0] = '\0';
  }
}

bool set_product_id_from_payload_le(const uint8_t *payload, size_t len) {
  uint32_t raw = at_protocol::decode_product_id_le(payload, len);
  if (raw == 0) {
    return false;
  }
  char hex[9];
  u32_to_hex_lower(raw, hex);
  bool changed = false;
  if (xSemaphoreTake(g_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (raw != g_last_product_raw) {
      g_last_product_raw = raw;
      memcpy(g_product_hex, hex, 9);
      g_has_pid = true;
      changed = true;
      Serial.printf("[GW_CTX] device_product_id=%s\n", g_product_hex);
    }
    xSemaphoreGive(g_mu);
  }
  return changed;
}

} // namespace gateway::ctx
