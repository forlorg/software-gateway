# software_gateway：CAN 数据经 MQTT 上行的数据通路说明

本文描述当前固件中，从 TWAI 总线到 MQTT Broker 的完整路径、涉及任务与配置项，便于联调与改参。

## 1. 总览（端到端）

```
CAN 帧 (TWAI)
    → can_rx 任务 (Core 1)
    → 时间戳 + AT 行编码 (at_protocol::encode_at_from_twai)
    → mqtt_uplink::offer_at_binary (完整一行二进制，含 \r\n)
    → RingBuffer 排队（`mqtt_uplink` 内 `PacketRingBuffer`）
    → mqtt_agg 任务按周期/水位弹出多行，拼成一批
    → mqtt_manager::publish_vehicle_upload (PubSubClient::publish)
    → MQTT Broker，Topic: topic_ps_pro/vehicle_upload/<product_id_hex>
```

下行（云端 → 设备）：

```
MQTT Broker → topic_ps_pro/vehicle_download/<product_id_hex>
    → PubSubClient 回调
    → at_protocol::consume_at_buffer 解析 AT
    → can_tx 队列 → TWAI 发送
```

**前置条件（否则不上传或不建连）：**

- STA 已连接路由器（`WiFi.isConnected()`）。
- 总线已解析出 **product_id**（扩展帧宣告）；用于生成上下行 Topic 与 MQTT ClientId。
- **MQTT 上行**：仅当 NTP 墙钟可用（`time_sync::can_use_wall_timestamp_for_upload`）时才 `offer_at_binary`；否则不计入 MQTT，但 **仍会** 用 `time_sync::pack_boot_relative_ms27()` 编码 AT 并由 **原生 USB CDC（`USBSerial`，D+/D-）镜像** 输出（经队列 + 独立任务 `usb_cdc_mirr`）。调试日志走 **UART `Serial`（默认 RX=44 / TX=43）**，与镜像物理分离。队列满丢弃计 `statistics::serial_mirror_queue_drops`，`/api/live_state` 可查；可调 `can_rx::kSerialMirrorQueueDepth`。
- 墙钟可用但打包异常时仍丢弃上行并计 `statistics::dropped`（与旧行为一致）。

## 2. 源码与职责对照

| 模块 | 路径 | 作用 |
|------|------|------|
| CAN 接收 | `src/can/can_rx.cpp` | 收 TWAI、统计、识别 product 宣告、编码 AT 行、USB CDC 镜像队列、条件送入 uplink |
| 设备上下文 | `src/system/gateway_context.{h,cpp}` | 保存 `product_id` 十六进制字符串；**仅当 id 变化**时返回 true，触发 MQTT topic 重绑 |
| MQTT 客户端 | `src/network/mqtt_manager.cpp` | WiFiClient + PubSubClient：连接、订阅、发布、LWT；在 `wifi_ap_task` 中与 `WiFi.loop()` 同核轮询 |
| 上行缓冲/聚合 | `src/task/mqtt_uplink.cpp` | Ring + 聚合任务 `mqtt_agg`，批发到 `publish_vehicle_upload` |
| NVS 配置 | `src/config/config_store.cpp` | WiFi/MQTT 凭据；缺键时用 `isKey` 避免无意义 NVS 读与错误日志 |
| 统计与网页 | `src/can/can_traffic_stats.cpp`, `src/network/web_server.cpp` | 帧率、上行字节、`/api/live_state` 等 |

## 3. MQTT Topic 与 ClientId

- **上行发布**：`topic_ps_pro/vehicle_upload/<8位小写十六进制 product_id>`
- **下行订阅**：`topic_ps_pro/vehicle_download/<同上>`
- **遗嘱**：`topic_ps_pro/will/<同上>`，payload `offline`，QoS0，retain
- **ClientId**：`pspro-<8位 hex>`

Broker 主机/端口/用户名/密码：优先 NVS（网页 `POST /api/mqtt/config` 写入）；若从未配置 host，则使用代码中的默认 `broker.hivemq.com:1883` 与空用户/密码。

## 4. 上行载荷格式（MQTT 里是什么）

每一批 `publish` 的 **payload 为二进制拼接**：多条 **AT 协议行** 首尾相接。单行由 `at_protocol::encode_at_from_twai` 生成，为固定格式的二进制结构（非 JSON 文本），以 **`\r\n`** 结尾。多条行依次拼进同一 payload，以减少 MQTT 消息数量。

单条长度上限见 `at_protocol::kAtLineMaxBytes`；单包批大小上限见 **`mqtt_uplink::config::kMaxBatchBytes`（4096）**，与 PubSubClient 缓冲 **`mqtt_manager::config::kPubSubBufferBytes`（8192）** 匹配。

## 5. RingBuffer 聚合行为

常量定义在 **`src/task/mqtt_uplink.h`** 的 `mqtt_uplink::config`：

| 常量 | 默认 | 含义 |
|------|------|------|
| `kRingBufferBytes` | 48×1024 | 环形缓冲总字节 |
| `kAggregateMsNormal` | 500 ms | 队列低于高水位时的聚合周期 |
| `kAggregateMsFast` | 200 ms | 队列高于高水位时加快 flush |
| `kHighWaterBytes` | 20×1024 | 高水位阈值 |
| `kMaxBatchBytes` | 4096 | 单次 `publish` 最大批大小 |

`mqtt_agg` 任务跑在 `kArduinoLoopPinnedCore`（见 `system/esp32_loop_core.h`），循环：按周期从 Ring 中 `pop` 完整行，尽量装入 `kMaxBatchBytes`，满则先 `publish` 再续装；仅在 **`mqtt_manager::is_connected()`** 为真时发送。成功发送后调用 `can_traffic_stats::record_uplink_bytes` 累计网页展示的「上行字节」。

**首次 MQTT 成功前不入队**：`offer_at_binary` 在从未连上且当前未连接时会丢弃并计 dropped（与规格一致）。

## 6. `can_ring_flush` 模块说明

工程内仍保留 **`can_ring_flush`**（`src/task/can_ring_flush_task.*`）实现，与 Ring 聚合参数一致，可用于仅统计、不上 MQTT 的实验场景；**当前默认固件入口**（`main.cpp`）仅启动 **`mqtt_uplink`**，未挂载该任务。

## 7. 与日志相关的已修正问题

1. **`Preferences … NOT_FOUND`**：首次烧录或未保存过 MQTT 键时，旧实现直接 `getString` 会触发 NVS 错误日志。现对 `mqtt_host` / `wifi_ssid` 等先 **`prefs.isKey`**，缺键则返回默认且不再读字符串键。
2. **重复 `[MQTT] connect` / `connected + subscribed`**：原逻辑在**每一帧** product 宣告上都调用 `notify_product_id_changed()`，导致反复 `disconnect` + 重连。现仅在 **`set_product_id_from_payload_le` 返回 true（id 变化）** 时才通知 MQTT 模块重绑 topic。

## 8. 轮询位置（为何放在 WiFi 任务）

`mqtt_manager::loop_poll()` 在 **`wifi_ap_task`**（Core 0）中与 `wifi_manager::loop()`、`time_sync::loop_poll()` 同周期执行，保证 **PubSubClient + WiFiClient** 与 **WiFi 协议栈** 同上下文，避免跨核 Async TCP 类问题。

## 9. 原生 USB CDC（`USBSerial`）与 Windows 上的「描述 / 友好名称」

CAN 镜像使用的 **`USBSerial`（HWCDC）** 走芯片 **USB Serial/JTAG** 硬件通路。按 Espressif 说明（参见 [arduino-esp32#11394](https://github.com/espressif/arduino-esp32/issues/11394) 讨论）：**只有 TinyUSB 设备栈可自定义 USB 字符串描述符；HWCDC 为片内固定实现，固件无法在应用层设置 iProduct / iInterface 等以改变系统在设备列表里显示的默认名称。**

在 Windows 上若需「一眼可辨」的识别，可采用：

1. **按硬件实例区分**：设备管理器里同一 VID/PID 下，不同物理口对应不同 **位置信息 / 实例 ID**；上位机枚举串口时除 COM 号外可结合 **USB 端口路径** 或 **容器 ID** 固定绑定到某台设备。  
2. **自定义 INF / 友好名称**：为指定 VID/PID（及可选硬件 ID）安装带 **FriendlyName** 的驱动包或 OEM 信息（需自行维护签名与分发，且同一 VID/PID 下多台同类设备名称仍可能相同）。  
3. **改用 TinyUSB CDC 承载 CAN 镜像**：可在描述符中写入自定义字符串，但需较大架构调整，且与当前 HWCDC 方案不同栈，需单独评估。

---

修改聚合或缓冲参数时，请同时核对 **`mqtt_uplink::config`** 与 **`mqtt_manager::config::kPubSubBufferBytes`**，避免单包超过 MQTT 客户端缓冲导致发布失败。
