/**
 * @file wifi_manager.cpp
 * @brief SoftAP/STA 共存策略：扫描、连接排队、快照供 Web 与状态机使用。
 */

#include "wifi_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <cstring>

#include "config/config_store.h"
#include "network/provision_fsm.h"
#include "network/web_server.h"
#include "system/state_machine.h"

namespace gateway::wifi_manager {
bool request_sta_connect(const char *ssid, const char *password);
}

namespace {

enum class StaFsm { Idle, Connecting, Connected };

StaFsm g_sta = StaFsm::Idle;
uint32_t g_connect_start_ms = 0;
constexpr uint32_t kConnectTimeoutMs = 45000;
wl_status_t g_last_st = WL_IDLE_STATUS;

char g_pend_ssid[33]{};
char g_pend_pass[65]{};
volatile bool g_pend_sta{false};

/** NVS 凭证连接超时后不再自动重扫，直到用户在配网页重新提交（对齐 esp32 AP 模式语义）。 */
bool g_block_auto_saved_sta{false};

volatile bool g_pend_disconnect{false};

constexpr unsigned kScanWifiMaxListed = 5;
constexpr unsigned kScanWifiMaxCollected = 64;
constexpr uint32_t kWifiScanWallTimeoutMs = 12000;
/** 单层信道超时；总时长仍受 scanComplete 轮询 + `kWifiScanWallTimeoutMs` 限制。 */
constexpr uint32_t kWifiScanMsPerChannel = 100;

enum class ScanSt : uint8_t { Idle, Pending, Ready };
ScanSt g_scan_st{ScanSt::Idle};
String g_scan_json;
bool g_scan_async_started{false};
uint32_t g_scan_wall_t0_ms{0};

struct ApCand {
  String ssid;
  int32_t rssi{};
};

int g_snap_rssi{-100};
char g_snap_ip[20]{"0.0.0.0"};
bool g_snap_sta_up{false};
const char *g_snap_sta_json = "\"disconnected\"";

void refresh_http_snapshot() {
  wl_status_t st = WiFi.status();
  g_snap_sta_up = (st == WL_CONNECTED);
  g_snap_sta_json = g_snap_sta_up ? "\"connected\"" : "\"disconnected\"";
  g_snap_rssi = g_snap_sta_up ? WiFi.RSSI() : -100;
  IPAddress ip = WiFi.localIP();
  snprintf(g_snap_ip, sizeof(g_snap_ip), "%s", ip.toString().c_str());
}

void drain_pending_disconnect() {
  if (!g_pend_disconnect) {
    return;
  }
  g_pend_disconnect = false;
  g_pend_sta = false;
  WiFi.scanDelete();
  g_scan_st = ScanSt::Idle;
  g_scan_async_started = false;
  /* 否则 NVS 仍有凭据时，驱动会在后台自动重连，且不经过 g_sta=Connecting；扫描会与 STA 冲突并常报 driver 失败。 */
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false);
  vTaskDelay(pdMS_TO_TICKS(40));
  g_sta = StaFsm::Idle;
  g_block_auto_saved_sta = true;
  Serial.println("[WIFI_STA] STA disconnected by user (NVS kept; no auto saved reconnect until form)");
  gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiApMode);
  gateway::provision::notify_sta_user_disconnected();
}

void drain_pending_sta_connect() {
  if (!g_pend_sta) {
    return;
  }
  if (gateway::wifi_manager::request_sta_connect(g_pend_ssid, g_pend_pass)) {
    g_pend_sta = false;
    g_block_auto_saved_sta = false;
  }
}

void emit_scan_json_error(const char *scan_key, const char *reason) {
  JsonDocument doc;
  doc["scan"] = scan_key;
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  (void)doc["aps"].to<JsonArray>();
  g_scan_json.clear();
  serializeJson(doc, g_scan_json);
}

void drain_wifi_scan() {
  if (g_scan_st != ScanSt::Pending) {
    return;
  }
  const uint32_t wall_now = millis();
  if (!g_scan_async_started) {
    (void)WiFi.scanNetworks(true, false, false, kWifiScanMsPerChannel);
    g_scan_async_started = true;
    g_scan_wall_t0_ms = wall_now;
    return;
  }
  const int16_t sc = WiFi.scanComplete();

  if (sc == WIFI_SCAN_RUNNING) {
    if (wall_now - g_scan_wall_t0_ms <= kWifiScanWallTimeoutMs) {
      return;
    }
    WiFi.scanDelete();
    emit_scan_json_error("stopped", "wall_timeout_no_new_scan_click");
    g_scan_async_started = false;
    g_scan_st = ScanSt::Ready;
    return;
  }

  if (sc < 0) {
    WiFi.scanDelete();
    emit_scan_json_error("failed", "driver");
    g_scan_async_started = false;
    g_scan_st = ScanSt::Ready;
    return;
  }

  const int n = sc;
  ApCand cand[kScanWifiMaxCollected];
  uint8_t cand_n = 0;
  for (int i = 0; i < n && cand_n < kScanWifiMaxCollected; ++i) {
    cand[cand_n].ssid = WiFi.SSID(i);
    cand[cand_n].rssi = WiFi.RSSI(i);
    ++cand_n;
  }
  WiFi.scanDelete();

  const bool truncated = cand_n > kScanWifiMaxListed;
  const unsigned picks = truncated ? static_cast<unsigned>(kScanWifiMaxListed) : static_cast<unsigned>(cand_n);
  for (unsigned o = 0; o < picks && o < cand_n; ++o) {
    unsigned best = o;
    for (unsigned i = o + 1; i < cand_n; ++i) {
      if (cand[i].rssi > cand[best].rssi) {
        best = i;
      }
    }
    if (best != o) {
      ApCand t = cand[o];
      cand[o] = cand[best];
      cand[best] = t;
    }
  }

  JsonDocument doc;
  JsonArray arr = doc["aps"].to<JsonArray>();
  doc["scan"] = "ok";
  doc["listed"] = picks;
  doc["more_ignored_than_listed"] = truncated;
  const unsigned show = picks;
  for (unsigned i = 0; i < show; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = cand[i].ssid;
    o["rssi"] = cand[i].rssi;
  }
  g_scan_json.clear();
  serializeJson(doc, g_scan_json);
  g_scan_async_started = false;
  g_scan_st = ScanSt::Ready;
}

void setup_ap() {
  uint64_t chip = ESP.getEfuseMac();
  uint16_t suf = static_cast<uint16_t>(chip & 0xFFFF);
  char ssid_ap[24];
  snprintf(ssid_ap, sizeof(ssid_ap), "PSPRO-%04X", suf);
  WiFi.softAP(ssid_ap, "12345678");
  IPAddress ap = WiFi.softAPIP();
  Serial.printf("[AP] SoftAP SSID=%s  配网 http://%s:%u/\n", ssid_ap, ap.toString().c_str(),
                static_cast<unsigned>(gateway::web_server::kHttpListenPort));
}

bool try_start_sta_saved() {
  if (g_sta != StaFsm::Idle) {
    return false;
  }
  String ssid;
  String pass;
  if (!gateway::config_store::wifi_get(ssid, pass) || ssid.isEmpty()) {
    return false;
  }
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  g_sta = StaFsm::Connecting;
  g_connect_start_ms = millis();
  gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiStaConnecting);
  return true;
}

} // namespace

namespace gateway::wifi_manager {

void start() {
  gateway::state_machine::set_state(gateway::state_machine::SystemState::Boot);
  g_block_auto_saved_sta = false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(true, true);
  setup_ap();
  if (!try_start_sta_saved()) {
    Serial.println("[WIFI_STA] No saved credentials; use captive portal on SoftAP.");
    g_sta = StaFsm::Idle;
    gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiApMode);
  }
}

bool request_sta_connect(const char *ssid, const char *password) {
  if (!ssid || ssid[0] == '\0') {
    return false;
  }
  if (g_sta == StaFsm::Connecting) {
    return false;
  }
  WiFi.disconnect(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password ? password : "");
  g_sta = StaFsm::Connecting;
  g_connect_start_ms = millis();
  gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiStaConnecting);
  return true;
}

void schedule_sta_connect(const char *ssid, const char *password) {
  if (!ssid || ssid[0] == '\0') {
    return;
  }
  snprintf(g_pend_ssid, sizeof(g_pend_ssid), "%s", ssid);
  snprintf(g_pend_pass, sizeof(g_pend_pass), "%s", password ? password : "");
  g_pend_sta = true;
  g_block_auto_saved_sta = false;
}

void schedule_sta_disconnect() { g_pend_disconnect = true; }

void loop() {
  drain_pending_disconnect();
  drain_pending_sta_connect();
  drain_wifi_scan();

  wl_status_t st = WiFi.status();

  if (g_sta == StaFsm::Connected && st != WL_CONNECTED) {
    g_sta = StaFsm::Idle;
    g_block_auto_saved_sta = false;
    Serial.println("[WIFI_STA] link lost, retry saved profile");
    gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiLost);
    try_start_sta_saved();
  }

  if (g_sta == StaFsm::Connecting) {
    if (st != g_last_st) {
      Serial.printf("[WIFI_STA] status=%d\n", static_cast<int>(st));
      g_last_st = st;
    }
    if (st == WL_CONNECTED) {
      g_sta = StaFsm::Connected;
      Serial.printf("[WIFI_STA] STA IP=%s RSSI=%d AP仍保持\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiStaConnected);
      gateway::provision::notify_wifi_sta_linked();
    } else if (millis() - g_connect_start_ms > kConnectTimeoutMs) {
      WiFi.disconnect(false);
      g_sta = StaFsm::Idle;
      g_block_auto_saved_sta = true;
      Serial.println("[WIFI_STA] connect timeout (check password / router)");
      gateway::state_machine::set_state(gateway::state_machine::SystemState::WifiApMode);
      gateway::provision::notify_wifi_sta_failed("sta_timeout");
    } else {
      refresh_http_snapshot();
      return;
    }
  }

  if (g_sta == StaFsm::Idle && st == WL_DISCONNECTED && !g_block_auto_saved_sta) {
    try_start_sta_saved();
  }

  refresh_http_snapshot();
}

bool sta_is_linked() { return g_snap_sta_up; }

const char *sta_status_json_fragment() { return g_snap_sta_json; }

int sta_rssi_cached() { return g_snap_rssi; }

void sta_local_ip_str(char *buf, unsigned cap) {
  if (!buf || cap == 0) {
    return;
  }
  snprintf(buf, cap, "%s", g_snap_ip);
}

void sta_current_ssid(char *buf, unsigned cap) {
  if (!buf || cap == 0) {
    return;
  }
  if (!g_snap_sta_up) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, cap, "%s", WiFi.SSID().c_str());
}

void wifi_scan_request_from_http() {
  if (g_scan_st != ScanSt::Idle) {
    return;
  }
  /* Web 与 WiFi 分属不同任务：点「断开」后 g_pend_disconnect 置位期间仍可能 WL_CONNECTED，此时允许排队扫描。
   * 禁止条件以驱动层为准，并覆盖「仅 g_sta==Idle 但自动重连已挂上 STA」的情况。 */
  const bool forbid_scan =
      !g_pend_disconnect &&
      (WiFi.status() == WL_CONNECTED || g_sta == StaFsm::Connecting);
  if (forbid_scan) {
    emit_scan_json_error("unavailable", "sta_connected");
    g_scan_async_started = false;
    g_scan_st = ScanSt::Ready;
    return;
  }
  g_scan_st = ScanSt::Pending;
  g_scan_async_started = false;
}

int wifi_scan_take_result_json(const char **out) {
  if (!out) {
    return 0;
  }
  if (g_scan_st == ScanSt::Ready) {
    *out = g_scan_json.c_str();
    g_scan_st = ScanSt::Idle;
    return 2;
  }
  if (g_scan_st == ScanSt::Pending) {
    return 1;
  }
  return 0;
}

} // namespace gateway::wifi_manager
