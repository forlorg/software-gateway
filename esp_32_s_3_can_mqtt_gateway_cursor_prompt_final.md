# ESP32S3 CAN ↔ MQTT Gateway 系统设计（Arduino + FreeRTOS）

> **文档与代码对齐说明（修订稿）**  
> 本文档已按当前仓库 `software_gateway` 已实现代码整理，作为「规格 + 实现对照」的单一事实来源的补充。  
> 上行/缓冲/Topic 等行为细节另见 `docs/MQTT_DATA_PIPELINE.md`。  
> **不包含**临时调试任务 `src/task/uart2_test_task.*`（固件入口可能挂载，但不纳入本方案描述）。

---

## 一、项目目标

基于 ESP32S3 + Arduino Framework，实现长期稳定运行的：

```txt
CAN <-> MQTT 网关系统
```

系统特性（与实现一致部分）：

- AP + STA 共存
- Web 配网（Arduino 同步 `WebServer`）
- REST API 配网 / 状态 / 恢复出厂等
- WIFI / MQTT 凭据 NVS 持久化与自动重连（MQTT 为指数退避轮询）
- MQTT ↔ CAN 双向桥接（上行聚合、下行 AT 解析）
- 配网页通过 **轮询 `/api/live_state`** 刷新状态（非浏览器 SSE `/events`）
- 多任务（FreeRTOS），CAN 与部分逻辑绑核
- RingBuffer（包队列）+ 首次 MQTT 成功前不入队策略
- MQTT Last Will（retain、payload `offline`）
- **AT 数据经队列异步输出至原生 USB CDC（`USBSerial`）** 作为镜像/旁路（非 `Serial1` 方案）
- 调试日志 UART：`Serial`（默认 RX=44 / TX=43，115200）
- 板载 LED 心跳任务（可选引脚）
- OTA：**未实现**（无独立 OTA 模块）
- 开机自检：**未实现**为文档原列的全套 Health 面板；仅有启动日志与堆巡检
- 长期稳定运行导向（WiFi/MQTT 与 TWAI 同官方驱动栈）

系统要求：

```txt
7x24 小时稳定运行（设计目标）
```

---

## 二、硬件配置

### MCU

```txt
ESP32S3
```

### CAN

- 标准：**CAN 2.0**（驱动：`driver/twai.h`，非独立 Arduino 库 `ESP32-TWAI-CAN` 包名依赖）
- 波特率：**250 kbps**
- 不支持 CAN-FD
- 引脚（`can_driver.h` / `can_hw`）：**TX = GPIO4，RX = GPIO5**
- 过滤器：**接受全部**（`TWAI_FILTER_CONFIG_ACCEPT_ALL()`）

### AT 串口镜像（当前实现）

- **物理接口**：芯片 **USB Serial/JTAG（`USBSerial` / HWCDC）**，在 `main` 中 `USBSerial.begin(921600)`（受 `ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` 影响；详见 `main.cpp` 与 `docs/MQTT_DATA_PIPELINE.md` §9）。
- **路径**：`can_rx` 编码 AT 行 → 队列 `MirrorLine` → 独立任务 `usb_cdc_mirr` 写 USB。
- **与 MQTT 解耦**：无 NTP 墙钟时仍镜像；MQTT 上行仅在 `time_sync::can_use_wall_timestamp_for_upload()` 为真时 `offer_at_binary`。
- **队列满**：计 `statistics::serial_mirror_queue_drops`，深度常量 `can_rx::kSerialMirrorQueueDepth`（默认 24）。

**已弃用（相对旧版方案文档）**：`Serial1` @ GPIO6/7 @ 230400 的镜像方案当前代码**未采用**。

---

## 三、核心架构（以代码为准）

### 设计动机（PlatformIO 注释摘要）

MQTT 使用 **PubSubClient + WiFiClient**，与 **`WiFi.loop()` 同在 `wifi_ap_task`（Core0）** 中轮询，避免 AsyncTCP 类栈与 lwIP 跨核不同步导致的长时间挂起。

### Core0（典型负载）

- `wifi_ap_task`：`wifi_manager::loop()`、`time_sync::loop_poll()`、`mqtt_manager::loop_poll()`
- `can_rx` 触发的 **`usb_cdc_mirr`** 镜像发送任务（`can_rx::kMirrorTxTaskCore = 0`）

### Core1（典型负载）

- **`can_rx`** TWAI 接收、product 宣告解析、AT 编码、入镜像队列、条件送入 `mqtt_uplink`
- **`heartbeat_led`**（若引脚有效）

### Arduino `loopTask` 核与 `mqtt_agg`

- `mqtt_uplink` 聚合任务 `mqtt_agg` 绑在 **`kArduinoLoopPinnedCore`**（`system/esp32_loop_core.h`，通常等于 `CONFIG_ARDUINO_RUNNING_CORE`，ESP32-S3 上多为 **1**），与 Arduino 主循环任务同核，便于与 Ring 批处理节奏一致。

### `web_server_task`

- 同步 **`WebServer::handleClient()`**、配网 FSM `provision::tick()`、周期性 `sse_manager::tick()`（实为刷新内部 JSON 快照，供 HTTP 查询；**非** `AsyncEventSource` 长连接）。
- 绑核：`web_server_task::kPinnedCpuCoreIndex`（当前为 **0**）。

**与旧文档差异摘要**：旧版将「Web + MQTT + SSE」全部压在 Core0 的叙述过于简化；当前实现将 **HTTP 服务** 与 **WiFi/MQTT 轮询** 分为两个 FreeRTOS 任务，且 `mqtt_agg` 常在 **另一核**。

---

## 四、技术栈（已实现）

必须使用 / 实际使用：

```txt
Arduino Framework
FreeRTOS
ArduinoJson（v7 风格 JsonDocument）
PubSubClient + WiFiClient（同步 TCP）
ESP-IDF TWAI 驱动（driver/twai.h）
Preferences（config_store）
```

HTTP：

```txt
Arduino 内置 WebServer（同步），非 ESPAsyncWebServer
```

**未使用**（与旧方案文档不同）：

```txt
ESPAsyncWebServer / AsyncTCP
AsyncMQTT_ESP32
AsyncTCP_SSL
```

---

## 五、Arduino Core / 平台版本

`platformio.ini` 使用：

```ini
platform = espressif32
framework = arduino
```

**未**在工程中固定写死 `ESP32 Arduino Core 2.0.17`；实际工具链版本随 `espressif32` platform 发布滚动。若需锁版本，应在 `platformio.ini` 中显式 `platform = …@x.y.z`。

---

## 六、编码约束（调整）

仍应避免：

```txt
在热路径大量 String 拼接
阻塞式无限重连（不含退避）
业务路径滥用 delay（启动阶段少量 delay 仍存在）
```

**原方案「禁止 PubSubClient」** 与当前实现**相反**——已实现且为刻意架构选择。

---

## 七、WIFI 工作模式

系统使用 **AP + STA 共存**（`wifi_manager` 内 `WiFi.mode(WIFI_AP_STA)` 语义）。

### AP

- SSID 形如 **`PSPRO-xxxx`**（`xxxx` 来自 MAC），默认密码 **`12345678`**（以 `wifi_manager` 实现为准）。
- AP 用于未上联路由器时的配网入口。

### STA

- 凭据存 NVS；支持用户 **`POST /api/wifi/disconnect`** 断开 STA 但保留 NVS，便于切换路由器。
- 周边扫描：`GET /api/wifi/scan`（异步扫描 + 202 pending 轮询模式）。

---

## 八、配置保存（NVS）

`config_store` 保存键（与实现一致，含先 `isKey` 再读以避免 NOT_FOUND 日志）：

```txt
wifi_ssid / wifi_password
mqtt_host / mqtt_port / mqtt_username / mqtt_password
```

---

## 九、MQTT 功能

### Broker

默认（未配置 host 时）：`broker.hivemq.com:1883`。

### Topic

- 上行：`topic_ps_pro/vehicle_upload/<8位小写十六进制 product_id>`
- 下行：`topic_ps_pro/vehicle_download/<同上>`
- LWT：`topic_ps_pro/will/<同上>`，payload `offline`，**QoS0**，**retain = true**

### ClientId

```txt
pspro-<8位hex>
```

### 上行 publish

- **retain = false**（`publish_vehicle_upload`）
- PubSubClient 默认 **QoS 0**

### 下行 subscribe

代码中为 **`subscribe(topic, 1)`**，即下行订阅 **QoS 1**（与旧文档「全局 QoS0」不一致，以代码为准）。

### 重连

- `mqtt_manager::loop_poll`：指数退避、抖动；连接成功后重置退避。
- **product_id 变更**时：`notify_product_id_changed()` → 断开并重绑 topic（且已修复「每帧宣告都重连」问题，仅在 ID 变化时触发）。

### MQTT 启动条件

- STA 已连接且已取得 **product_id** 后才进入有效连接/订阅流程（与 `mqtt_manager::loop_poll` 守卫一致）。

---

## 十、product_id 获取

- 参考扩展帧 ID **`0x16174B03`**；实现采用 **低 24 位匹配**：`(id29 & 0xFFFFFF) == (0x16174B03 & 0xFFFFFF)`（见 `can_rx::extended_id_is_product_announce`）。
- Payload 解析为 little-endian 的 32 位 product id（`at_protocol::decode_product_id_le`），保存为 **8 位小写十六进制字符串**（`gateway_context`）。

---

## 十一、AT 协议

固定二进制行格式（实现于 `at_protocol`；含 `\r\n` 结尾），与旧方案「AT + 时间戳 + CAN_ID + 长度 + payload + \r\n」一致；**禁止随意改字段布局**以免上下位机不兼容。

---

## 十二、时间同步

- STA 联网后 **SNTP**（`pool.ntp.org`），TZ 默认 **`CST-8`**。
- MQTT 上行时间戳：需 **墙钟可用**（`ntp_has_sync` 等条件，见 `time_sync::can_use_wall_timestamp_for_upload`）；否则仅镜像、不上 MQTT（并可能计 `dropped` 若打包失败）。

时间戳打包细节见 `time_sync.cpp`（与旧文档「高 5bit 时区 + 低 27bit 当天毫秒」设计对齐方向一致，以代码为准）。

---

## 十三、CAN → MQTT 聚合发送

常量命名空间 **`mqtt_uplink::config`**（与 `mqtt_uplink.h` 一致）：

| 项 | 值 |
|----|-----|
| Ring 总字节 | 48×1024 |
| 正常聚合周期 | 500 ms |
| 高水位加速周期 | 200 ms |
| 高水位阈值 | 20×1024 字节 |
| 单批最大 | 4096 字节 |

Ring 实现类：**`PacketRingBuffer`**（`utils/packet_ringbuffer.*`），互斥保护。

**首次 MQTT 成功连接前**：`offer_at_binary` **不入队**，丢弃并计 `statistics::dropped`（`had_successful_connection()` 守卫）。

**断网后**：Ring 内已入队数据在恢复连接后由 `mqtt_agg` 继续尝试发送（聚合任务在未连接时不消费 Ring，与「断网缓存」语义一致）。

**Ring 满**：`PacketRingBuffer::push` 在空间不足时 **循环丢弃最旧完整包**（`drop_oldest_packet_`）直至能写入新包；与旧方案「丢最旧」一致。

`PubSubClient` 缓冲：**8192** 字节（`mqtt_manager::config::kPubSubBufferBytes`），须与批大小匹配。

---

## 十四、MQTT download

- Broker 侧应保证完整 AT 批；固件 `consume_at_buffer` 解析后 **`can_tx::enqueue`**。
- 校验失败丢弃并统计（见 `statistics` / 下行处理路径）。

---

## 十五、REST API（当前路由）

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/wifi/connect` | JSON：`ssid` / `password`，写 NVS 并排程 STA |
| POST | `/api/wifi/disconnect` | 断开 STA，保留 NVS |
| GET | `/api/wifi/scan` | 异步扫描；可能 202 pending |
| GET | `/api/wifi/provision` | 配网状态 JSON |
| POST | `/api/mqtt/config` | JSON：`host` / `port` / `username` / `password` |
| GET | `/api/status` | 在 live_state 基础上附加 `heap_free` 等 |
| GET | `/api/live_state` | 配网页与上位机轮询用 JSON 快照 |
| POST | `/api/factory_reset` | 清空配置并重启 |
| POST | `/api/dev/provision_outcome` | 开发/联调用配网结果注入 |

---

## 十六、Web 页面

- 内嵌于 `web_server.cpp` 的 `kIndexHtml`：未连 STA 时显示扫描与表单；已连时显示 RSSI/IP/NTP/状态等。
- **刷新机制**：页面脚本 **`setInterval(pollLive, 1000)`** 拉取 `/api/live_state`。
- CAN/MQTT 统计字段名与旧版 SSE JSON 示例**不完全相同**（含 `can_rx_frames`、`mqtt_queue_bytes`、`dropped_lines` 等）。

---

## 十七、「SSE」与实时推送

- 模块名 **`sse_manager`** 仅为 **快照刷新钩子**（`tick()` → `web_server::sse_tick()`），**未实现** `/events` 上的 **`AsyncEventSource` 多客户端 SSE**。
- 若将来需真正 SSE，应新增路由与事件源实现；当前以 HTTP 轮询为准。

---

## 十八、系统状态机

`gateway::state_machine::SystemState`：

```cpp
Boot,
WifiApMode,
WifiStaConnecting,
WifiStaConnected,
MqttConnecting,
MqttReady,
WifiLost,
MqttLost,
```

---

## 十九、任务划分（实现名）

| 任务名（约） | 职责 | 绑核（默认/常量） |
|--------------|------|-------------------|
| `WiFi_AP` | SoftAP/STA、`WiFi.loop`、SNTP/MQTT 轮询 | Core0 |
| `WEB_SRV` | 同步 WebServer、provision tick、堆告警日志 | 见 `web_server_task.h` |
| `mqtt_agg` | Ring 弹出、聚合批、`publish_vehicle_upload` | `kArduinoLoopPinnedCore` |
| `can_rx` | TWAI RX、AT、镜像队列、上行 offer | Core1 |
| （内部）`usb_cdc_mirr` | 从队列写 USB | Core0 |
| `HB_LED` | LED 心跳 | Core1 |

**未在 `main` 启动**：`can_ring_flush_task`（保留作实验/仅统计场景；默认固件只用 `mqtt_uplink`）。

---

## 二十、线程安全

使用 **互斥量、队列、信号量** 等（如 `gateway_context`、`PacketRingBuffer`、`can_tx` 队列）；避免无锁跨任务共享大块状态。

---

## 二十一、内存与 JSON

- Web/MQTT 配置解析使用 **`JsonDocument`**（ArduinoJson 7），配合栈缓冲或固定缓冲序列化。
- 仍应避免在 ISR 或极高频路径动态分配大段 `String`。

---

## 二十二、Watchdog

**未**集成文档所述的 `esp_task_wdt` 统一监控模块；若需启用需在独立模块中补充。

---

## 二十三、堆内存巡检

`web_server_task`：若 `ESP.getFreeHeap() < 30*1024`，周期性 **Serial 警告日志**。  
**未**实现原文所述的「SSE 与 API 同步告警字段」全套联动（`/api/live_state` 已含多项状态，可无单独 heap 告警字段）。

---

## 二十四、统计项（与 API 字段对应）

核心计数器见 `statistics` 与 `can_traffic_stats`，包括但不限于：

```txt
can_rx / can_tx / mqtt_tx / mqtt_rx / dropped / serial_mirror_queue_drops
uptime_ms（JSON 中）
```

网页 JSON 另含 CAN 帧率、字节速率等（`can_traffic_stats` 快照）。

---

## 二十五、错误恢复

- **WIFI**：重连、用户断开、扫描超时等（`wifi_manager`）。
- **MQTT**：退避重连；product_id 变更重绑。
- **NTP**：随 STA 与 SNTP 初始化重试。
- **CAN bus-off**：若需显式恢复，应在 TWAI 层补充（以当前 `can_*` 代码为准）。

---

## 二十六、目录结构（当前）

```txt
src/
├── main.cpp
├── config/
│   ├── config_store.cpp
│   └── config_store.h
├── network/
│   ├── wifi_manager.cpp / .h
│   ├── mqtt_manager.cpp / .h
│   ├── web_server.cpp / .h
│   ├── sse_manager.cpp / .h
│   └── provision_fsm.cpp / .h
├── can/
│   ├── can_driver.cpp / .h
│   ├── can_rx.cpp / .h
│   ├── can_tx.cpp / .h
│   └── can_traffic_stats.cpp / .h
├── protocol/
│   ├── at_protocol.cpp / .h
├── system/
│   ├── gateway_context.cpp / .h
│   ├── state_machine.cpp / .h
│   ├── statistics.cpp / .h
│   ├── time_sync.cpp / .h
│   └── esp32_loop_core.h
├── utils/
│   ├── packet_ringbuffer.cpp
│   └── packet_ringbuffer.h
├── task/
│   ├── wifi_ap_task.cpp / .h
│   ├── web_server_task.cpp / .h
│   ├── mqtt_uplink.cpp / .h
│   ├── can_ring_flush_task.cpp / .h
│   └── heartbeat_led_task.cpp / .h
docs/
└── MQTT_DATA_PIPELINE.md
```

（`uart2_test_task.*` 为临时任务，不列入上表。）

---

## 二十七、编码要求

- 模块化、`namespace gateway::…`、统一日志前缀（大量 `Serial.printf`）。
- `main.cpp` 保持精简，业务在任务与 manager 中。

---

## 二十八、开发阶段（对照当前进度）

- **Phase1**：AP/STA、NVS、REST、Web 轮询状态、状态机 — **基本完成**
- **Phase2**：CAN RX/TX、AT、Ring、USB 镜像 — **基本完成**
- **Phase3**：MQTT 聚合、重连、统计 — **基本完成**；**OTA / WDT / 完整开机自检 / 真 SSE** — **未按原文实现**

---

## 二十九、特别注意

- **异步 MQTT / Async Web** 不是本仓库路径；后续若迁移需整体替换网络栈并重新验证 lwIP 线程模型。
- 修改聚合或缓冲时同步核对 **`mqtt_uplink::config`** 与 **`mqtt_manager::config::kPubSubBufferBytes`**。
