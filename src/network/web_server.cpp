/**
 * @file web_server.cpp
 * @brief 同步 WebServer：配网页、REST API、MQTT/WiFi 配置与 live_state JSON。
 */

#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <cstring>

#include "network/provision_fsm.h"
#include "network/wifi_manager.h"
#include "config/config_store.h"
#include "system/state_machine.h"
#include "system/time_sync.h"
#include "system/gateway_context.h"
#include "can/can_rx.h"
#include "can/can_traffic_stats.h"
#include "network/mqtt_manager.h"
#include "task/mqtt_uplink.h"
#include "system/statistics.h"

namespace gateway::web_server {

namespace {

WebServer g_http(kHttpListenPort);

char g_live_state_json[2048]{};

void refresh_live_json_snapshot() {
  const unsigned long ms = static_cast<unsigned long>(millis());
  JsonDocument doc;
  doc["uptime_ms"] = ms;
  doc["mono_ms"] = ms;
  doc["epoch_sec"] = gateway::time_sync::utc_epoch_seconds();
  char wall_buf[52]{};
  gateway::time_sync::format_wall_time_iso8601(wall_buf, sizeof(wall_buf));
  doc["wall_time"] = wall_buf;
  doc["ntp_synced"] = gateway::time_sync::ntp_has_sync();
  doc["system_state"] = gateway::state_machine::system_state_str(gateway::state_machine::current());
  doc["sta"] = gateway::wifi_manager::sta_is_linked() ? "connected" : "disconnected";
  doc["sta_rssi"] = gateway::wifi_manager::sta_rssi_cached();
  char ip[28]{};
  gateway::wifi_manager::sta_local_ip_str(ip, sizeof(ip));
  doc["sta_ip"] = ip;
  char cssid[40]{};
  gateway::wifi_manager::sta_current_ssid(cssid, sizeof(cssid));
  doc["sta_ssid"] = cssid;
  {
    const auto cs = gateway::can_traffic_stats::get_web_snapshot();
    doc["can_rx_frames"] = cs.rx_frames;
    doc["can_rx_payload_bytes"] = cs.rx_payload_bytes;
    doc["can_uplink_bytes"] = cs.uplink_bytes;
    doc["can_rx_fps_5s"] = cs.rx_fps_5s;
    doc["can_rx_payload_bytes_s_5s"] = cs.rx_payload_bytes_s_5s;
    doc["can_uplink_bytes_s_5s"] = cs.uplink_bytes_s_5s;
    char bus_pid[9]{};
    if (gateway::ctx::has_product_id()) {
      gateway::ctx::get_product_id_hex(bus_pid);
    }
    doc["bus_product_id_hex"] = bus_pid;
  }
  doc["mqtt_connected"] = gateway::mqtt_manager::is_connected();
  doc["mqtt_queue_bytes"] = static_cast<uint32_t>(gateway::mqtt_uplink::ring_used_bytes());
  doc["can_tx_frames"] = gateway::statistics::can_tx();
  doc["mqtt_tx_msgs"] = gateway::statistics::mqtt_tx();
  doc["mqtt_rx_msgs"] = gateway::statistics::mqtt_rx();
  doc["dropped_lines"] = gateway::statistics::dropped();
  doc["serial_mirror_queue_drops"] = gateway::statistics::serial_mirror_queue_drops();
  doc["serial_mirror_queue_depth"] = gateway::can_rx::kSerialMirrorQueueDepth;
  if (!gateway::wifi_manager::sta_is_linked()) {
    String s;
    String p;
    if (gateway::config_store::wifi_get(s, p) && s.length() > 0) {
      doc["saved_ssid"] = s;
    } else {
      doc["saved_ssid"] = "";
    }
  }
  (void)serializeJson(doc, g_live_state_json, sizeof(g_live_state_json));
}

const char kIndexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PSPRO Gateway 配网</title></head><body>
<h1>网关配网</h1>
<p>请连接设备热点 <code>PSPRO-xxxx</code>（默认密码 <code>12345678</code>）后浏览本页面。</p>
<div id="ui-offline">
<h2>未连接外网 STA</h2>
<p><strong>SSID/密码</strong>会写入 NVS；<strong>断电重启</strong>后设备会<strong>自动</strong>用已保存凭据尝试连路由器。</p>
<p id="saved-hint-wrap"><small>NVS：<span id="saved-hint"></span></small></p>
<p><small><strong>说明：</strong>STA 已连通路由器时，芯片侧无法再发起周边扫描；需换热点时请先在下方断开或连上后直接填新 SSID。</small></p>
<button type="button" onclick="scanWifi()">扫描周边热点（至多列出信号最强的 5 个）</button>
<pre id="scan"></pre>
<form id="wf" onsubmit="return wfSubmit(event)">
<label>SSID <input name="ssid"></label><br>
<label>密码 <input name="password" type="password"></label><br>
<button type="submit">保存并连接</button></form>
</div>
<div id="ui-online" style="display:none">
<h2>已连接外网 STA</h2>
<dl style="margin:0 0 12px;padding:12px;background:#f4f8ff;border-radius:8px">
<dt style="opacity:.65">SSID</dt><dd id="disp-ssid" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">IP</dt><dd id="disp-ip" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">RSSI</dt><dd id="disp-rssi" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">系统状态</dt><dd id="disp-state" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">UTC 纪元 (秒)</dt><dd id="disp-epoch" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">本地墙钟（NTP）</dt><dd id="disp-wall" style="margin:0 0 8px;"></dd>
<dt style="opacity:.65">NTP</dt><dd id="disp-ntp" style="margin:0 0 8px;"></dd>
</dl>
<p><button type="button" onclick="disconnectSta()">断开 WiFi（清空 STA 链路，保留 NVS）</button></p>
<p><small>断开后将出现「扫描 / 表单」面板，可切换其它路由器。</small></p>
</div>
<h2>CAN 流量统计</h2>
<dl style="margin:0 0 12px;padding:12px;background:#f8fff4;border-radius:8px">
<dt style="opacity:.65">总线设备编号</dt><dd id="bus-product-id" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">接收帧总数</dt><dd id="can-rx-frames" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">接收载荷字节累计</dt><dd id="can-rx-payload" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">上行字节累计（拟云端）</dt><dd id="can-up-bytes" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">近 5s 接收帧率</dt><dd id="can-rx-fps" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">近 5s 接收载荷速率</dt><dd id="can-rx-bps" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">近 5s 上行速率</dt><dd id="can-up-bps" style="margin:0 0 8px;">—</dd>
</dl>
<h2>云端连接统计</h2>
<p><small>需总线已上报设备编号且 STA 联网后才会连接云端；配置写入 NVS，保存后自动应用。</small></p>
<form id="mq" onsubmit="return mqSubmit(event)">
<label>Host <input name="host" value="broker.hivemq.com" style="width:min(100%,280px)"></label><br>
<label>Port <input name="port" type="number" value="1883" style="width:6em"></label><br>
<label>User <input name="username" style="width:min(100%,200px)"></label><br>
<label>Pass <input name="password" type="password" style="width:min(100%,200px)"></label><br>
<button type="submit">保存云端配置</button></form>
<dl style="margin:0 0 12px;padding:12px;background:#fff8f4;border-radius:8px">
<dt style="opacity:.65">云端已连接</dt><dd id="mqtt-conn" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">上行队列占用</dt><dd id="mqtt-queue" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">CAN 下发帧</dt><dd id="can-tx-frames" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">云端上行条数</dt><dd id="mqtt-tx" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">云端下行条数</dt><dd id="mqtt-rx" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">丢弃行数</dt><dd id="mqtt-drop" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">串口镜像队列满丢弃</dt><dd id="ser-mirror-drop" style="margin:0 0 8px;">—</dd>
<dt style="opacity:.65">串口镜像队列深度</dt><dd id="ser-mirror-depth" style="margin:0 0 8px;">—</dd>
</dl>
<script>
var liveCache={sta:'disconnected'};
function setText(id,s){var e=document.getElementById(id);if(e)e.textContent=s==null?'':String(s);}
function fmtRate(n){if(n==null||n!==n)return '—';return Number(n).toFixed(2);}
function applyCanStats(){
  if(liveCache.can_rx_frames===undefined)return;
  setText('bus-product-id',liveCache.bus_product_id_hex?liveCache.bus_product_id_hex:'（尚未收到）');
  setText('can-rx-frames',liveCache.can_rx_frames);
  setText('can-rx-payload',liveCache.can_rx_payload_bytes);
  setText('can-up-bytes',liveCache.can_uplink_bytes);
  setText('can-rx-fps',fmtRate(liveCache.can_rx_fps_5s)+' 帧/s');
  setText('can-rx-bps',fmtRate(liveCache.can_rx_payload_bytes_s_5s)+' 字节/s');
  setText('can-up-bps',fmtRate(liveCache.can_uplink_bytes_s_5s)+' 字节/s');
}
function applyMqttStats(){
  if(liveCache.mqtt_connected===undefined)return;
  setText('mqtt-conn',liveCache.mqtt_connected?'是':'否');
  setText('mqtt-queue',liveCache.mqtt_queue_bytes!=null?liveCache.mqtt_queue_bytes:'—');
  setText('can-tx-frames',liveCache.can_tx_frames!=null?liveCache.can_tx_frames:'—');
  setText('mqtt-tx',liveCache.mqtt_tx_msgs!=null?liveCache.mqtt_tx_msgs:'—');
  setText('mqtt-rx',liveCache.mqtt_rx_msgs!=null?liveCache.mqtt_rx_msgs:'—');
  setText('mqtt-drop',liveCache.dropped_lines!=null?liveCache.dropped_lines:'—');
  setText('ser-mirror-drop',liveCache.serial_mirror_queue_drops!=null?liveCache.serial_mirror_queue_drops:'—');
  setText('ser-mirror-depth',liveCache.serial_mirror_queue_depth!=null?liveCache.serial_mirror_queue_depth:'—');
}
function applyProvisioningUi(){
  var on=(liveCache.sta||'')==='connected';
  document.getElementById('ui-offline').style.display=on?'none':'block';
  document.getElementById('ui-online').style.display=on?'block':'none';
  applyCanStats();
  applyMqttStats();
  if(on){
    setText('disp-ssid',liveCache.sta_ssid);
    setText('disp-ip',liveCache.sta_ip);
    setText('disp-rssi',liveCache.sta_rssi);
    setText('disp-state',liveCache.system_state);
    setText('disp-epoch',liveCache.epoch_sec);
    setText('disp-wall',liveCache.wall_time);
    setText('disp-ntp',liveCache.ntp_synced?'已同步':'未同步');
    return;
  }
  var hh=document.getElementById('saved-hint');
  if(hh){hh.textContent=liveCache.saved_ssid?'已保存SSID: '+liveCache.saved_ssid:'未保存或未连接过外网SSID';}
}
async function scanWifi(){
  if((liveCache.sta||'')==='connected'){
    document.getElementById('scan').textContent='STA 已连通时无法进行周边扫描（请先到「已连接」面板断开 STA）。';return;}
  document.getElementById('scan').textContent='扫描中...';
  await fetch('/api/wifi/scan');
  for(var i=0;i<48;i++){
    await new Promise(r=>setTimeout(r,250));
    var r=await fetch('/api/wifi/scan');
    var t=await r.text();
    if(r.ok&&r.status===200){document.getElementById('scan').textContent=t;return;}
    if(r.status!==202)return;
  }
  document.getElementById('scan').textContent='轮询超时';
}
async function disconnectSta(){
  var r=await fetch('/api/wifi/disconnect',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});
  alert(await r.text());await pollLive();
}
async function pollLive(){
  try{
    var r=await fetch('/api/live_state');
    var t=await r.text();
    liveCache=JSON.parse(t);
    applyProvisioningUi();
  }catch(_){}
}
async function mqSubmit(e){
  e.preventDefault();
  var f=new FormData(e.target);
  var body=JSON.stringify({
    host:(f.get('host')||'').trim(),
    port:Number(f.get('port'))||1883,
    username:f.get('username')||'',
    password:f.get('password')||''
  });
  var r=await fetch('/api/mqtt/config',{method:'POST',headers:{'Content-Type':'application/json'},body:body});
  alert(await r.text());
  return false;
}
async function wfSubmit(e){
  e.preventDefault();
  await pollLive();
  if((liveCache.sta||'')==='connected')return false;
  var f=new FormData(e.target);
  var ss=(f.get('ssid')||'').trim();
  if(!ss){alert('请输入 SSID');return false;}
  var body=JSON.stringify({ssid:ss,password:f.get('password')||''});
  var r=await fetch('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:body});
  alert(await r.text());
  for(var i=0;i<200;i++){
    await new Promise(function(x){setTimeout(x,250);});
    await pollLive();
    if((liveCache.sta||'')==='connected')break;
  }
  return false;
}
pollLive();
setInterval(pollLive,1000);
</script></body></html>
)rawliteral";

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
    g_http.send_P(200, "text/html; charset=utf-8", kIndexHtml);
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
    const size_t n = serializeJson(out, stack, sizeof(stack));
    if (n >= sizeof(stack)) {
      g_http.send(500, "application/json; charset=utf-8", "{\"error\":\"status_truncated\"}");
      return;
    }
    g_http.send(200, "application/json; charset=utf-8", stack);
  });

  g_http.on("/api/live_state", HTTP_GET, handle_live_state);

  g_http.on("/api/wifi/connect", HTTP_POST, handle_wifi_connect);
  g_http.on("/api/wifi/disconnect", HTTP_POST, handle_wifi_disconnect);
  g_http.on("/api/wifi/scan", HTTP_GET, handle_wifi_scan);

  g_http.on("/api/mqtt/config", HTTP_POST, handle_mqtt_config);

  g_http.on("/api/factory_reset", HTTP_POST, handle_factory_reset);

  g_http.on("/api/dev/provision_outcome", HTTP_POST, handle_dev_provision);

  g_http.onNotFound([]() { g_http.send(404, "text/plain", "Not Found"); });

  g_http.begin();
  sse_tick();
  Serial.printf("[HTTP] Arduino WebServer (sync) :%u\n", static_cast<unsigned>(kHttpListenPort));
}

void poll() { g_http.handleClient(); }

void sse_tick() { refresh_live_json_snapshot(); }

} // namespace gateway::web_server
