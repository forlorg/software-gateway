#include "web_assets.h"

#include <Arduino.h>

namespace gateway::web_assets {

const char kIndexHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>PSPRO Gateway</title>
    <link rel="stylesheet" href="/style.css">
  </head>
  <body>
    <main class="shell">
      <header class="hero">
        <div>
          <p class="eyebrow">PSPRO CAN Gateway</p>
          <h1>网关控制台</h1>
          <p>
            请连接设备热点 <code>PSPRO-xxxx</code>（默认密码
            <code>12345678</code>）后访问本页面。
          </p>
        </div>
        <div class="status-pill" id="top-status">加载中</div>
      </header>

      <nav class="tabs">
        <button class="tab active" data-page="system">系统状态</button>
        <button class="tab" data-page="calibration">系统标定</button>
        <button class="tab" data-page="traffic">流量统计</button>
        <button class="tab" data-page="provision">网关配网</button>
      </nav>

      <section id="page-system" class="page active">
        <h2>系统状态</h2>
        <p class="muted">数据字段后续由 CAN 模块接口直接提供，网络层仅负责透传。</p>
        <div id="system-groups" class="group-grid"></div>
      </section>

      <section id="page-calibration" class="page">
        <h2>系统标定</h2>
        <p class="muted">
          表单当前占位；当前值由 CAN 模块接口提供，提交值后续直接注入 CAN 模块接口。
        </p>
        <div id="calibration-table" class="card"></div>
        <button class="primary" type="button" onclick="submitCalibration()">
          提交标定数据
        </button>
        <pre id="calibration-result" class="result"></pre>
      </section>

      <section id="page-traffic" class="page">
        <h2>流量统计</h2>
        <div class="cards two">
          <article class="card">
            <h3>CAN 流量统计</h3>
            <dl class="kv">
              <dt>总线设备编号</dt>
              <dd id="bus-product-id">—</dd>
              <dt>接收帧总数</dt>
              <dd id="can-rx-frames">—</dd>
              <dt>接收载荷字节累计</dt>
              <dd id="can-rx-payload">—</dd>
              <dt>上行字节累计</dt>
              <dd id="can-up-bytes">—</dd>
              <dt>近 5s 接收帧率</dt>
              <dd id="can-rx-fps">—</dd>
              <dt>近 5s 接收载荷速率</dt>
              <dd id="can-rx-bps">—</dd>
              <dt>近 5s 上行速率</dt>
              <dd id="can-up-bps">—</dd>
            </dl>
          </article>

          <article class="card">
            <h3>云端连接统计</h3>
            <dl class="kv">
              <dt>云端已连接</dt>
              <dd id="mqtt-conn">—</dd>
              <dt>上行队列占用</dt>
              <dd id="mqtt-queue">—</dd>
              <dt>CAN 下发帧</dt>
              <dd id="can-tx-frames">—</dd>
              <dt>云端上行条数</dt>
              <dd id="mqtt-tx">—</dd>
              <dt>云端下行条数</dt>
              <dd id="mqtt-rx">—</dd>
              <dt>丢弃行数</dt>
              <dd id="mqtt-drop">—</dd>
              <dt>串口镜像队列满丢弃</dt>
              <dd id="ser-mirror-drop">—</dd>
              <dt>串口镜像队列深度</dt>
              <dd id="ser-mirror-depth">—</dd>
            </dl>
          </article>
        </div>
      </section>

      <section id="page-provision" class="page">
        <h2>网关配网</h2>

        <div id="ui-online" class="card" style="display:none">
          <h3>已连接外网 STA</h3>
          <dl class="kv">
            <dt>SSID</dt>
            <dd id="disp-ssid"></dd>
            <dt>IP</dt>
            <dd id="disp-ip"></dd>
            <dt>RSSI</dt>
            <dd id="disp-rssi"></dd>
            <dt>系统状态</dt>
            <dd id="disp-state"></dd>
            <dt>UTC 纪元</dt>
            <dd id="disp-epoch"></dd>
            <dt>本地墙钟</dt>
            <dd id="disp-wall"></dd>
            <dt>NTP</dt>
            <dd id="disp-ntp"></dd>
          </dl>
          <button type="button" onclick="disconnectSta()">断开 WiFi（保留 NVS）</button>
        </div>

        <div id="ui-offline" class="card">
          <h3>未连接外网 STA</h3>
          <p>
            <small>NVS：<span id="saved-hint"></span></small>
          </p>
          <p class="muted">
            STA 已连通路由器时，芯片侧无法再发起周边扫描；换热点请先断开或直接填写新 SSID。
          </p>
          <button type="button" onclick="scanWifi()">扫描周边热点（至多 5 个）</button>
          <pre id="scan" class="result"></pre>
          <form id="wf" onsubmit="return wfSubmit(event)">
            <label>SSID <input name="ssid"></label>
            <label>密码 <input name="password" type="password"></label>
            <button class="primary" type="submit">保存并连接</button>
          </form>
        </div>

        <div class="card">
          <h3>云端配置</h3>
          <p class="muted">配置写入 NVS，保存后自动应用；连接统计请查看“流量统计”。</p>
          <form id="mq" onsubmit="return mqSubmit(event)">
            <label>Host <input name="host" value="broker.hivemq.com"></label>
            <label>Port <input name="port" type="number" value="1883"></label>
            <label>User <input name="username"></label>
            <label>Pass <input name="password" type="password"></label>
            <button class="primary" type="submit">保存云端配置</button>
          </form>
        </div>
      </section>
    </main>

    <script src="/app.js"></script>
  </body>
</html>
)rawliteral";

const char kStyleCss[] PROGMEM = R"rawliteral(:root{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#182033;background:#eef3f8}body{margin:0}.shell{max-width:1080px;margin:0 auto;padding:24px}.hero{display:flex;justify-content:space-between;gap:16px;align-items:center;background:linear-gradient(135deg,#173b73,#2681c9);color:white;border-radius:20px;padding:24px;box-shadow:0 12px 36px #173b7333}.hero h1{margin:.2em 0}.eyebrow{letter-spacing:.12em;text-transform:uppercase;opacity:.75}.status-pill{background:#ffffff22;border:1px solid #ffffff55;border-radius:999px;padding:10px 14px;white-space:nowrap}.tabs{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin:18px 0}.tab{border:0;border-radius:14px;padding:14px 8px;background:white;color:#2d3b55;font-weight:700;box-shadow:0 4px 16px #1b355214}.tab.active{background:#1d75bd;color:white}.page{display:none}.page.active{display:block}.card{background:white;border-radius:18px;padding:18px;margin:14px 0;box-shadow:0 6px 22px #1b355214}.cards.two{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}.group-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px}.switch-group h3{margin-top:0}.switch-row{display:flex;justify-content:space-between;gap:12px;padding:11px 0;border-top:1px solid #edf1f5}.switch-row:first-of-type{border-top:0}.value{font-weight:800;color:#115c9c}.kv{display:grid;grid-template-columns:minmax(120px,1fr) 1.2fr;gap:8px 14px}.kv dt{color:#6e7788}.kv dd{margin:0;font-weight:700}.muted{color:#657084}label{display:block;margin:10px 0;color:#46536a}input{box-sizing:border-box;width:min(100%,320px);padding:10px;border:1px solid #cfd8e5;border-radius:10px}button{cursor:pointer;border:0;border-radius:12px;padding:10px 14px;background:#e7eef7;color:#1f3657;font-weight:700}.primary{background:#1d75bd;color:white}.result{white-space:pre-wrap;background:#101827;color:#dbeafe;border-radius:12px;padding:12px;min-height:24px;overflow:auto}.cal-row{display:grid;grid-template-columns:1.2fr 1fr 1fr;gap:10px;align-items:center;padding:10px 0;border-top:1px solid #edf1f5}.cal-row.header{font-weight:800;color:#657084;border-top:0}@media(max-width:720px){.shell{padding:12px}.hero{display:block}.tabs{grid-template-columns:repeat(2,1fr)}.cards.two{grid-template-columns:1fr}.kv{grid-template-columns:1fr}.cal-row{grid-template-columns:1fr}})rawliteral";

const char kAppJs[] PROGMEM = R"rawliteral(
const POLL_MS = 2000;

let activePage = 'system';
let pollTimer = 0;
let provisionCache = { sta: 'disconnected' };

function $(id) {
  return document.getElementById(id);
}

function setText(id, s) {
  const e = $(id);
  if (e) {
    e.textContent = s == null ? '' : String(s);
  }
}

function fmtRate(n) {
  return n == null || Number.isNaN(Number(n)) ? '—' : Number(n).toFixed(2);
}

function setTop(s) {
  setText('top-status', s);
}

function stopPoll() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = 0;
  }
}

function startPoll() {
  stopPoll();
  pollCurrent();
  pollTimer = setInterval(pollCurrent, POLL_MS);
}

document.querySelectorAll('.tab').forEach((button) => {
  button.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach((tab) => tab.classList.remove('active'));
    document.querySelectorAll('.page').forEach((page) => page.classList.remove('active'));

    button.classList.add('active');
    activePage = button.dataset.page;
    $('page-' + activePage).classList.add('active');
    startPoll();
  });
});

async function getJson(url) {
  const response = await fetch(url);
  const text = await response.text();

  if (!response.ok) {
    throw new Error(text);
  }

  return JSON.parse(text);
}

async function pollCurrent() {
  try {
    if (activePage === 'system') {
      return applySystem(await getJson('/api/page/system_status'));
    }

    if (activePage === 'calibration') {
      return applyCalibration(await getJson('/api/page/calibration'));
    }

    if (activePage === 'traffic') {
      return applyTraffic(await getJson('/api/page/traffic_stats'));
    }

    if (activePage === 'provision') {
      return applyProvision(await getJson('/api/page/provision'));
    }
  } catch (e) {
    setTop('接口异常');
  }
}

function applySystem(d) {
  setTop('系统状态 ' + (d.system_state || ''));

  const root = $('system-groups');
  if (!root) {
    return;
  }

  root.innerHTML = '';

  (d.groups || []).forEach((group) => {
    const card = document.createElement('article');
    card.className = 'card switch-group';
    card.innerHTML = '<h3></h3>';
    card.querySelector('h3').textContent = group.name || '关联开关组';

    (group.items || []).forEach((item) => {
      const row = document.createElement('div');
      row.className = 'switch-row';
      row.innerHTML = '<span></span><span class="value"></span>';
      row.children[0].textContent = item.name || '';
      row.children[1].textContent = item.value || '—';
      card.appendChild(row);
    });

    root.appendChild(card);
  });

  if (!root.children.length) {
    root.innerHTML = '<article class="card"><p>CAN 状态数据待接入。</p></article>';
  }
}

function applyCalibration(d) {
  setTop('系统标定');

  const root = $('calibration-table');
  if (!root) {
    return;
  }

  let html = '<div class="cal-row header">'
    + '<span>数据名称</span>'
    + '<span>数据当前值</span>'
    + '<span>数据值更改</span>'
    + '</div>';

  (d.items || []).forEach((item, index) => {
    html += '<div class="cal-row">'
      + '<span>' + esc(item.name || '标定项') + '</span>'
      + '<span class="value">' + esc(item.current || '—') + '</span>'
      + '<input data-cal-idx="' + index + '" placeholder="待输入">'
      + '</div>';
  });

  root.innerHTML = html;
}

function applyTraffic(d) {
  setTop('流量统计');

  const can = d.can || {};
  const cloud = d.cloud || {};

  setText('bus-product-id', can.bus_product_id_hex || '（尚未收到）');
  setText('can-rx-frames', can.rx_frames);
  setText('can-rx-payload', can.rx_payload_bytes);
  setText('can-up-bytes', can.uplink_bytes);
  setText('can-rx-fps', fmtRate(can.rx_fps_5s) + ' 帧/s');
  setText('can-rx-bps', fmtRate(can.rx_payload_bytes_s_5s) + ' 字节/s');
  setText('can-up-bps', fmtRate(can.uplink_bytes_s_5s) + ' 字节/s');

  setText('mqtt-conn', cloud.mqtt_connected ? '是' : '否');
  setText('mqtt-queue', cloud.mqtt_queue_bytes);
  setText('can-tx-frames', cloud.can_tx_frames);
  setText('mqtt-tx', cloud.mqtt_tx_msgs);
  setText('mqtt-rx', cloud.mqtt_rx_msgs);
  setText('mqtt-drop', cloud.dropped_lines);
  setText('ser-mirror-drop', cloud.serial_mirror_queue_drops);
  setText('ser-mirror-depth', cloud.serial_mirror_queue_depth);
}

function applyProvision(d) {
  provisionCache = d;
  setTop(d.sta === 'connected' ? 'STA 已连接' : 'STA 未连接');

  const isOnline = d.sta === 'connected';
  $('ui-offline').style.display = isOnline ? 'none' : 'block';
  $('ui-online').style.display = isOnline ? 'block' : 'none';

  setText('disp-ssid', d.sta_ssid);
  setText('disp-ip', d.sta_ip);
  setText('disp-rssi', d.sta_rssi);
  setText('disp-state', d.system_state);
  setText('disp-epoch', d.epoch_sec);
  setText('disp-wall', d.wall_time);
  setText('disp-ntp', d.ntp_synced ? '已同步' : '未同步');
  setText('saved-hint', d.saved_ssid ? '已保存SSID: ' + d.saved_ssid : '未保存或未连接过外网SSID');
}

async function scanWifi() {
  if ((provisionCache.sta || '') === 'connected') {
    setText('scan', 'STA 已连通时无法进行周边扫描（请先断开 STA）。');
    return;
  }

  setText('scan', '扫描中...');
  await fetch('/api/wifi/scan');

  for (let i = 0; i < 48; i++) {
    await new Promise((resolve) => setTimeout(resolve, 250));

    const response = await fetch('/api/wifi/scan');
    const text = await response.text();

    if (response.ok && response.status === 200) {
      setText('scan', text);
      return;
    }

    if (response.status !== 202) {
      return;
    }
  }

  setText('scan', '轮询超时');
}

async function disconnectSta() {
  const response = await fetch('/api/wifi/disconnect', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: '{}',
  });

  alert(await response.text());
  pollCurrent();
}

async function mqSubmit(e) {
  e.preventDefault();

  const formData = new FormData(e.target);
  const body = JSON.stringify({
    host: (formData.get('host') || '').trim(),
    port: Number(formData.get('port')) || 1883,
    username: formData.get('username') || '',
    password: formData.get('password') || '',
  });

  const response = await fetch('/api/mqtt/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body,
  });

  alert(await response.text());
  return false;
}

async function wfSubmit(e) {
  e.preventDefault();

  const formData = new FormData(e.target);
  const ssid = (formData.get('ssid') || '').trim();

  if (!ssid) {
    alert('请输入 SSID');
    return false;
  }

  const body = JSON.stringify({
    ssid,
    password: formData.get('password') || '',
  });

  const response = await fetch('/api/wifi/connect', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body,
  });

  alert(await response.text());
  return false;
}

async function submitCalibration() {
  const values = [];
  document.querySelectorAll('[data-cal-idx]').forEach((input) => values.push(input.value));

  const response = await fetch('/api/calibration/submit', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ placeholder: true, values }),
  });

  setText('calibration-result', await response.text());
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  }[c]));
}

startPoll();
)rawliteral";

} // namespace gateway::web_assets
