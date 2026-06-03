/**
 * @file web_server.cpp
 * @brief 同步 WebServer：页面资源、分页 REST API、MQTT/WiFi 配置与调试 live_state JSON。
 */

#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <cstring>

#include "can/can_rx.h"
#include "can/can_traffic_stats.h"
#include "config/config_store.h"
#include "network/mqtt_manager.h"
#include "network/provision_fsm.h"
#include "network/web_assets.h"
#include "network/wifi_manager.h"
#include "system/gateway_context.h"
#include "system/state_machine.h"
#include "system/statistics.h"
#include "system/time_sync.h"
#include "task/mqtt_uplink.h"

namespace gateway::web_server {

namespace {

WebServer g_http(kHttpListenPort);

char g_live_state_json[2048]{};

void write_common_status(JsonDocument &doc) {
  const unsigned long ms = static_cast<unsigned long>(millis());
  doc["uptime_ms"] = ms;
  doc["mono_ms"] = ms;
  doc["epoch_sec"] = gateway::time_sync::utc_epoch_seconds();
  char wall_buf[52]{};
  gateway::time_sync::format_wall_time_iso8601(wall_buf, sizeof(wall_buf));
  doc["wall_time"] = wall_buf;
  doc["ntp_synced"] = gateway::time_sync::ntp_has_sync();
  doc["system_state"] = gateway::state_machine::system_state_str(gateway::state_machine::current());
}

void write_wifi_status(JsonDocument &doc) {
  const bool linked = gateway::wifi_manager::sta_is_linked();
  doc["sta"] = linked ? "connected" : "disconnected";
  doc["sta_rssi"] = gateway::wifi_manager::sta_rssi_cached();
  char ip[28]{};
  gateway::wifi_manager::sta_local_ip_str(ip, sizeof(ip));
  doc["sta_ip"] = ip;
  char cssid[40]{};
  gateway::wifi_manager::sta_current_ssid(cssid, sizeof(cssid));
  doc["sta_ssid"] = cssid;
  String s;
  String p;
  if (!linked && gateway::config_store::wifi_get(s, p) && s.length() > 0) {
    doc["saved_ssid"] = s;
  } else {
    doc["saved_ssid"] = "";
  }
}

void write_can_traffic(JsonObject doc) {
  const auto cs = gateway::can_traffic_stats::get_web_snapshot();
  doc["rx_frames"] = cs.rx_frames;
  doc["rx_payload_bytes"] = cs.rx_payload_bytes;
  doc["uplink_bytes"] = cs.uplink_bytes;
  doc["rx_fps_5s"] = cs.rx_fps_5s;
  doc["rx_payload_bytes_s_5s"] = cs.rx_payload_bytes_s_5s;
  doc["uplink_bytes_s_5s"] = cs.uplink_bytes_s_5s;
  char bus_pid[9]{};
  if (gateway::ctx::has_product_id()) {
    gateway::ctx::get_product_id_hex(bus_pid);
  }
  doc["bus_product_id_hex"] = bus_pid;
}

void write_cloud_stats(JsonObject doc) {
  doc["mqtt_connected"] = gateway::mqtt_manager::is_connected();
  doc["mqtt_queue_bytes"] = static_cast<uint32_t>(gateway::mqtt_uplink::ring_used_bytes());
  doc["can_tx_frames"] = gateway::statistics::can_tx();
  doc["mqtt_tx_msgs"] = gateway::statistics::mqtt_tx();
  doc["mqtt_rx_msgs"] = gateway::statistics::mqtt_rx();
  doc["dropped_lines"] = gateway::statistics::dropped();
  doc["serial_mirror_queue_drops"] = gateway::statistics::serial_mirror_queue_drops();
  doc["serial_mirror_queue_depth"] = gateway::can_rx::kSerialMirrorQueueDepth;
}

void refresh_live_json_snapshot() {
  JsonDocument doc;
  write_common_status(doc);
  write_wifi_status(doc);
  {
    JsonDocument can_doc;
    auto can = can_doc.to<JsonObject>();
    write_can_traffic(can);
    doc["can_rx_frames"] = can["rx_frames"];
    doc["can_rx_payload_bytes"] = can["rx_payload_bytes"];
    doc["can_uplink_bytes"] = can["uplink_bytes"];
    doc["can_rx_fps_5s"] = can["rx_fps_5s"];
    doc["can_rx_payload_bytes_s_5s"] = can["rx_payload_bytes_s_5s"];
    doc["can_uplink_bytes_s_5s"] = can["uplink_bytes_s_5s"];
    doc["bus_product_id_hex"] = can["bus_product_id_hex"];
  }
  write_cloud_stats(doc.as<JsonObject>());
  (void)serializeJson(doc, g_live_state_json, sizeof(g_live_state_json));
}

void send_json_doc(JsonDocument &doc, char *buf, size_t cap) {
  const size_t n = serializeJson(doc, buf, cap);
  if (n >= cap) {
    g_http.send(500, "application/json; charset=utf-8", "{\"error\":\"json_truncated\"}");
    return;
  }
  g_http.send(200, "application/json; charset=utf-8", buf);
}

void handle_page_system_status() {
  JsonDocument doc;
  write_common_status(doc);
  auto groups = doc["groups"].to<JsonArray>();
  auto group = groups.add<JsonObject>();
  group["name"] = "PTO 关联开关组";
  auto items = group["items"].to<JsonArray>();
  auto pto_switch = items.add<JsonObject>();
  pto_switch["name"] = "PTO 开关";
  pto_switch["value"] = "—";
  auto pto_valve = items.add<JsonObject>();
  pto_valve["name"] = "PTO 控制阀";
  pto_valve["value"] = "— mA";
  auto pto_pressure = items.add<JsonObject>();
  pto_pressure["name"] = "PTO 压力";
  pto_pressure["value"] = "— MPa";
  char stack[1536];
  send_json_doc(doc, stack, sizeof(stack));
}

void handle_page_calibration() {
  JsonDocument doc;
  write_common_status(doc);
  auto items = doc["items"].to<JsonArray>();
  const char *names[] = {"标定数据 1", "标定数据 2", "标定数据 3"};
  for (const char *name : names) {
    auto item = items.add<JsonObject>();
    item["name"] = name;
    item["current"] = "—";
  }
  char stack[1280];
  send_json_doc(doc, stack, sizeof(stack));
}

void handle_page_traffic_stats() {
  JsonDocument doc;
  auto can = doc["can"].to<JsonObject>();
  write_can_traffic(can);
  auto cloud = doc["cloud"].to<JsonObject>();
  write_cloud_stats(cloud);
  char stack[1536];
  send_json_doc(doc, stack, sizeof(stack));
}

void handle_page_provision() {
  JsonDocument doc;
  write_common_status(doc);
  write_wifi_status(doc);
  doc["mqtt_connected"] = gateway::mqtt_manager::is_connected();
  char stack[1280];
  send_json_doc(doc, stack, sizeof(stack));
}

void handle_calibration_submit() {
  if (!g_http.hasArg("plain")) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, g_http.arg("plain"))) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"json\"}");
    return;
  }
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"placeholder\":true}");
}

void handle_wifi_connect() {
  if (!g_http.hasArg("plain")) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, g_http.arg("plain"))) {
    g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
    return;
  }
  const char *ssid = doc["ssid"] | "";
  const char *pass = doc["password"] | "";
  if (!ssid || !ssid[0]) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"ssid\"}");
    return;
  }

  gateway::config_store::wifi_set(String(ssid), pass && pass[0] ? String(pass) : String(""));
  gateway::wifi_manager::schedule_sta_connect(ssid, pass);
  gateway::provision::notify_connect_attempt_started();
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"accepted\":true}");
}

void handle_mqtt_config() {
  if (!g_http.hasArg("plain")) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, g_http.arg("plain"))) {
    g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
    return;
  }
  const char *host = doc["host"] | "";
  uint16_t port = static_cast<uint16_t>((int)doc["port"] | 1883);
  const char *user = doc["username"] | "";
  const char *pwd = doc["password"] | "";
  if (!host || !host[0]) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"host\"}");
    return;
  }
  gateway::config_store::mqtt_set(String(host), port, String(user), String(pwd));
  gateway::mqtt_manager::apply_config_from_store();
  gateway::mqtt_manager::notify_product_id_changed();
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handle_wifi_disconnect() {
  gateway::wifi_manager::schedule_sta_disconnect();
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"accepted\":true,\"note\":\"sta_off_nvs_kept\"}");
}

void handle_wifi_scan() {
  const char *json = nullptr;
  int phase = gateway::wifi_manager::wifi_scan_take_result_json(&json);
  if (phase == 2 && json != nullptr) {
    g_http.send(200, "application/json; charset=utf-8", json);
    return;
  }
  if (phase == 0) {
    gateway::wifi_manager::wifi_scan_request_from_http();
    phase = gateway::wifi_manager::wifi_scan_take_result_json(&json);
    if (phase == 2 && json != nullptr) {
      g_http.send(200, "application/json; charset=utf-8", json);
      return;
    }
  }
  g_http.send(202, "application/json; charset=utf-8", "{\"scan\":\"pending\"}");
}

void handle_factory_reset() {
  gateway::config_store::factory_reset();
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

void handle_dev_provision() {
  if (!g_http.hasArg("plain")) {
    g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, g_http.arg("plain"))) {
    g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
    return;
  }
  const bool ok = doc["ok"] | false;
  const char *err = doc["error"] | "";
  gateway::provision::dev_force_outcome(ok, err);
  g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handle_live_state() {
  refresh_live_json_snapshot();
  g_http.send(200, "application/json; charset=utf-8", g_live_state_json);
}

} // namespace

void start() {
  g_http.on("/", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_http.send_P(200, "text/html; charset=utf-8", gateway::web_assets::kIndexHtml);
#pragma GCC diagnostic pop
  });
  g_http.on("/style.css", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_http.send_P(200, "text/css; charset=utf-8", gateway::web_assets::kStyleCss);
#pragma GCC diagnostic pop
  });
  g_http.on("/app.js", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_http.send_P(200, "application/javascript; charset=utf-8", gateway::web_assets::kAppJs);
#pragma GCC diagnostic pop
  });

  g_http.on("/api/wifi/provision", HTTP_GET, []() {
    char stack[768];
    gateway::provision::write_status_json(stack, sizeof(stack));
    g_http.send(200, "application/json; charset=utf-8", stack);
  });

  g_http.on("/api/status", HTTP_GET, []() {
    refresh_live_json_snapshot();
    JsonDocument out;
    if (deserializeJson(out, g_live_state_json)) {
      out.clear();
    }
    out["heap_free"] = ESP.getFreeHeap();
    char stack[1536];
    send_json_doc(out, stack, sizeof(stack));
  });

  g_http.on("/api/live_state", HTTP_GET, handle_live_state);
  g_http.on("/api/page/system_status", HTTP_GET, handle_page_system_status);
  g_http.on("/api/page/calibration", HTTP_GET, handle_page_calibration);
  g_http.on("/api/page/traffic_stats", HTTP_GET, handle_page_traffic_stats);
  g_http.on("/api/page/provision", HTTP_GET, handle_page_provision);
  g_http.on("/api/calibration/submit", HTTP_POST, handle_calibration_submit);

  g_http.on("/api/wifi/connect", HTTP_POST, handle_wifi_connect);
  g_http.on("/api/wifi/disconnect", HTTP_POST, handle_wifi_disconnect);
  g_http.on("/api/wifi/scan", HTTP_GET, handle_wifi_scan);
  g_http.on("/api/mqtt/config", HTTP_POST, handle_mqtt_config);
  g_http.on("/api/factory_reset", HTTP_POST, handle_factory_reset);
  g_http.on("/api/dev/provision_outcome", HTTP_POST, handle_dev_provision);

  g_http.onNotFound([]() { g_http.send(404, "text/plain", "Not Found"); });

  g_http.begin();
  refresh_live_json_snapshot();
  Serial.printf("[HTTP] Arduino WebServer (sync) :%u\n", static_cast<unsigned>(kHttpListenPort));
}

void poll() { g_http.handleClient(); }

} // namespace gateway::web_server
