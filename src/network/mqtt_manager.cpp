/**
 * @file mqtt_manager.cpp
 * @brief MQTT 客户端：主题拼装、连接退避、下行 AT 解析与 CAN 下发；须在 WiFi 同任务轮询。
 */

#include "mqtt_manager.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstring>

#include "system/gateway_context.h"
#include "can/can_tx.h"
#include "config/config_store.h"
#include "protocol/at_protocol.h"
#include "system/state_machine.h"
#include "system/statistics.h"

namespace gateway::mqtt_manager {

namespace {

WiFiClient g_tcp;
PubSubClient g_mqtt(g_tcp);

char g_host[80];
uint16_t g_port{1883};
char g_user[48];
char g_pass[48];

char g_topic_up[96];
char g_topic_dn[96];
char g_topic_will[96];
char g_hex[9];

bool g_topics_ready{false};
bool g_mqtt_connected{false};
bool g_had_success_connect{false};
volatile bool g_mqtt_reconfig_pending{false};

uint32_t g_backoff_ms{1000};
uint32_t g_next_attempt_ms{0};

bool build_topics_locked() {
  if (!ctx::has_product_id()) {
    return false;
  }
  ctx::get_product_id_hex(g_hex);
  if (!g_hex[0]) {
    return false;
  }
  snprintf(g_topic_up, sizeof(g_topic_up), "topic_ps_pro/vehicle_upload/%s", g_hex);
  snprintf(g_topic_dn, sizeof(g_topic_dn), "topic_ps_pro/vehicle_download/%s", g_hex);
  snprintf(g_topic_will, sizeof(g_topic_will), "topic_ps_pro/will/%s", g_hex);
  g_topics_ready = true;
  return true;
}

void copy_creds_from_store() {
  String h;
  uint16_t p = 1883;
  String u;
  String pw;
  if (config_store::mqtt_get(h, p, u, pw) && h.length() > 0) {
    snprintf(g_host, sizeof(g_host), "%s", h.c_str());
    g_port = p;
    snprintf(g_user, sizeof(g_user), "%s", u.c_str());
    snprintf(g_pass, sizeof(g_pass), "%s", pw.c_str());
  } else {
    snprintf(g_host, sizeof(g_host), "%s", "broker.hivemq.com");
    g_port = 1883;
    g_user[0] = '\0';
    g_pass[0] = '\0';
  }
  g_mqtt.setServer(g_host, g_port);
}

void on_mqtt_download(const uint8_t *blob, size_t len) {
  if (!blob || len == 0) {
    return;
  }
  uint8_t work[4096];
  if (len > sizeof(work)) {
    statistics::add_mqtt_rx(1);
    return;
  }
  memcpy(work, blob, len);
  size_t wlen = len;
  at_protocol::consume_at_buffer(
      work, &wlen, nullptr,
      [](void *, const twai_message_t &tm) { can_tx::enqueue(tm); });
  statistics::add_mqtt_rx(1);
}

void mqtt_subscribe_cb(char *topic, byte *payload, unsigned int len) {
  if (payload && len > 0) {
    const unsigned kPreview = 48;
    const unsigned n = static_cast<unsigned>(len) < kPreview
                           ? static_cast<unsigned>(len)
                           : kPreview;
    Serial.print("[MQTT dn] payload(hex): ");
    for (unsigned i = 0; i < n; ++i) {
      Serial.printf("%02x", payload[i]);
    }
    if (static_cast<unsigned>(len) > n) {
      Serial.print("...");
    }
    Serial.print("\n");
  }
  on_mqtt_download(reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(len));
}

} // namespace

void init() {
  /** 上行批约 4KB + MQTT 头；与 `mqtt_uplink` 批大小一致。 */
  g_mqtt.setBufferSize(config::kPubSubBufferBytes);
  g_mqtt.setCallback(mqtt_subscribe_cb);
  copy_creds_from_store();
  g_tcp.setTimeout(8000);
}

void apply_config_from_store() { copy_creds_from_store(); }

void notify_product_id_changed() {
  build_topics_locked();
  g_mqtt_reconfig_pending = true;
}

void loop_poll() {
  if (g_mqtt_reconfig_pending) {
    g_mqtt_reconfig_pending = false;
    if (g_mqtt.connected()) {
      g_mqtt.disconnect();
    }
    g_mqtt_connected = false;
    copy_creds_from_store();
  }

  if (!WiFi.isConnected()) {
    return;
  }
  if (!ctx::has_product_id()) {
    return;
  }
  if (!build_topics_locked()) {
    return;
  }

  if (g_mqtt.connected()) {
    g_mqtt_connected = true;
    g_mqtt.loop();
    state_machine::set_state(state_machine::SystemState::MqttReady);
    return;
  }

  if (g_mqtt_connected) {
    g_mqtt_connected = false;
    state_machine::set_state(state_machine::SystemState::MqttLost);
    g_next_attempt_ms = millis() + g_backoff_ms + (ESP.getCycleCount() % 500);
    if (g_backoff_ms < 60000) {
      g_backoff_ms *= 2;
    }
  }

  const uint32_t now = millis();
  if (now < g_next_attempt_ms) {
    state_machine::set_state(state_machine::SystemState::MqttConnecting);
    return;
  }

  state_machine::set_state(state_machine::SystemState::MqttConnecting);

  char client_id[28];
  snprintf(client_id, sizeof(client_id), "pspro-%s", g_hex);

  const char *user = g_user[0] ? g_user : "";
  const char *pass = g_pass[0] ? g_pass : "";

  Serial.printf("[MQTT] connect %s:%u (PubSubClient) client=%s\n", g_host,
                static_cast<unsigned>(g_port), client_id);

  const bool ok =
      g_topics_ready
          ? g_mqtt.connect(client_id, user, pass, g_topic_will, 0, true, "offline", true)
          : g_mqtt.connect(client_id, user, pass);

  if (!ok) {
    Serial.printf("[MQTT] connect failed rc=%d (see PubSubClient MQTT_*)\n", g_mqtt.state());
    g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
    if (g_backoff_ms < 60000) {
      g_backoff_ms *= 2;
    }
    return;
  }

  if (!g_mqtt.subscribe(g_topic_dn, 1)) {
    Serial.println("[MQTT] subscribe failed");
    g_mqtt.disconnect();
    g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
    if (g_backoff_ms < 60000) {
      g_backoff_ms *= 2;
    }
    return;
  }

  g_mqtt_connected = true;
  g_had_success_connect = true;
  g_backoff_ms = 1000;
  g_next_attempt_ms = 0;
  state_machine::set_state(state_machine::SystemState::MqttReady);
  Serial.printf("[MQTT] connected; subscribe topic=%s\n", g_topic_dn);
}

bool is_connected() { return g_mqtt_connected && g_mqtt.connected(); }

bool had_successful_connection() { return g_had_success_connect; }

bool publish_blob(const char *topic_override, const uint8_t *data, size_t len) {
  if (!data || len == 0 || !g_mqtt_connected || !g_mqtt.connected()) {
    return false;
  }
  const char *t = topic_override ? topic_override : g_topic_up;
  if (!t || !t[0]) {
    return false;
  }
  const bool ok = g_mqtt.publish(t, data, static_cast<unsigned int>(len), false);
  if (!ok) {
    return false;
  }
  statistics::add_mqtt_tx(1);
  return true;
}

bool publish_vehicle_upload(const uint8_t *data, size_t len) {
  return publish_blob(nullptr, data, len);
}

const char *vehicle_upload_topic() {
  return (g_topics_ready && g_topic_up[0]) ? g_topic_up : "";
}

const char *vehicle_download_topic() {
  return (g_topics_ready && g_topic_dn[0]) ? g_topic_dn : "";
}

} // namespace gateway::mqtt_manager
