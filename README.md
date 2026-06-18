# software-gateway

`software-gateway` 是一个基于 **ESP32-S3 + Arduino Framework + FreeRTOS** 的 CAN ↔ MQTT 网关项目。项目使用 PlatformIO 构建，面向长期运行场景，实现 Wi-Fi AP/STA 配网、CAN 总线接入、AT 协议编码、MQTT 上下行桥接、Web 配置与状态查询等功能。

## 1. 项目目标

在 ESP32-S3 上实现一个稳定运行的 CAN ↔ MQTT 软件网关：

```text
CAN Bus  <->  ESP32-S3 Gateway  <->  Wi-Fi / MQTT Broker
```

主要目标：

- 通过 ESP32-S3 采集 CAN 2.0 帧。
- 将 CAN 数据编码为 AT 格式数据行。
- 将数据聚合后通过 MQTT 上报。
- 从 MQTT 下行 Topic 接收命令并转发到 CAN。
- 通过 SoftAP + Web 页面完成 Wi-Fi / MQTT 配置。
- 通过 NVS 保存配置，实现断电保持。
- 提供 HTTP API 查询设备状态、Wi-Fi 扫描、配置与恢复出厂。
- 后续扩展 OTA、Watchdog、健康检查和生产级远程运维能力。

## 2. 当前实现状态

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| PlatformIO 构建 | 已实现 | `platform = espressif32`，`framework = arduino` |
| Arduino + FreeRTOS | 已实现 | 业务主要运行在独立 FreeRTOS 任务中 |
| Wi-Fi AP/STA | 已实现 | AP + STA 共存，SoftAP 用于配网 |
| Web 配网 | 已实现 | Arduino 同步 `WebServer`，默认端口 80 |
| NVS 配置存储 | 已实现 | 使用 `Preferences` 保存 Wi-Fi / MQTT 配置 |
| MQTT 管理 | 已实现 | `PubSubClient + WiFiClient`，支持退避重连 |
| MQTT 上行聚合 | 已实现 | 48KB RingBuffer，批量 publish |
| CAN RX/TX | 已实现 | 使用 ESP-IDF TWAI 驱动 |
| CAN → AT 编码 | 已实现 | AT 行经 USB CDC 镜像，并按条件进入 MQTT 上行 |
| 时间同步 | 已实现 | STA 联网后 SNTP，默认 `pool.ntp.org` |
| OTA | 未实现 | 设计文档明确标注 OTA 未实现 |
| WDT 统一监控 | 未实现 | 后续建议补充 |
| 完整 Health 面板 | 未实现 | 当前有启动日志与堆内存巡检 |

## 3. 硬件与运行参数

| 参数 | 当前信息 |
| --- | --- |
| MCU | ESP32-S3 |
| 开发板 | ESP32-S3-DevKitC-1 |
| CPU | 240 MHz |
| RAM | 320 KB |
| PSRAM | 8 MB |
| Flash | `CLAUDE.md` 硬件约束写 8 MB，运行参数处写 16 MB；实际应以 `ESP.getFlashChipSize()` 启动日志为准 |
| USB-UART | CH343 USB-to-Serial，常见 Windows 端口为 `COMx` |
| 调试串口 | `Serial`，115200 baud，RX=44，TX=43 |
| USB CDC 镜像 | `USBSerial`，921600 baud，作为 AT 数据镜像 / 旁路 |
| 心跳 LED | GPIO 1，500 ms 半周期 |
| UART2 测试任务 | Serial2 230400 baud，RX=18，TX=17 |
| CAN 标准 | CAN 2.0，非 CAN-FD |
| CAN 驱动 | ESP-IDF TWAI：`driver/twai.h` |
| CAN 波特率 | 250 kbps |
| CAN TX/RX | TX = GPIO4，RX = GPIO5 |
| CAN 过滤器 | 接受全部 |

## 4. 技术栈

| 类型 | 技术 |
| --- | --- |
| 构建系统 | PlatformIO |
| 平台 | `espressif32` |
| Framework | Arduino |
| 语言标准 | C++17 |
| JSON | ArduinoJson 7.4.3 |
| MQTT | PubSubClient 2.8.0 + WiFiClient |
| 配置存储 | Preferences / NVS |
| HTTP 服务 | Arduino 内置同步 WebServer |
| CAN 驱动 | ESP-IDF TWAI |
| 多任务 | FreeRTOS |

当前项目显式避免使用 `ESPAsyncWebServer`、`AsyncTCP`、`AsyncMQTT_ESP32` 一类异步网络栈。设计文档说明 MQTT 与 Wi-Fi 轮询采用同步模式，目的是避免 AsyncTCP 类栈与 lwIP 跨核协调问题导致长时间挂起。

## 5. 仓库结构

```text
software-gateway/
├── request-docs/
│   ├── 10_can_protocol_intro.yaml
│   ├── 11_can_protocol_enums.yaml
│   ├── 12_can_protocol_frames_15xx.yaml
│   ├── 12_can_protocol_frames_17xx.yaml
│   ├── 12_can_protocol_frames_18xx.yaml
│   ├── 12_can_protocol_frames_index.yaml
│   └── 12_can_protocol_frames_other.yaml
├── scripts/
├── src/
│   ├── can/
│   │   ├── can_driver.cpp / .h
│   │   ├── can_rx.cpp / .h
│   │   ├── can_tx.cpp / .h
│   │   └── can_traffic_stats.cpp / .h
│   ├── config/
│   │   └── config_store.cpp / .h
│   ├── network/
│   │   ├── wifi_manager.cpp / .h
│   │   ├── mqtt_manager.cpp / .h
│   │   ├── web_server.cpp / .h
│   │   ├── sse_manager.cpp / .h
│   │   └── provision_fsm.cpp / .h
│   ├── protocol/
│   │   └── at_protocol.cpp / .h
│   ├── system/
│   │   ├── gateway_context.cpp / .h
│   │   ├── state_machine.cpp / .h
│   │   ├── statistics.cpp / .h
│   │   ├── time_sync.cpp / .h
│   │   └── esp32_loop_core.h
│   ├── task/
│   │   ├── can_ring_flush_task.cpp / .h
│   │   ├── heartbeat_led_task.cpp / .h
│   │   ├── http_server_task.cpp / .h
│   │   ├── mqtt_uplink.cpp / .h
│   │   ├── network_task.cpp / .h
│   │   └── uart2_test_task.cpp / .h
│   ├── utils/
│   │   └── packet_ringbuffer.cpp / .h
│   └── main.cpp
├── CLAUDE.md
├── Prompt.md
├── esp_32_s_3_can_mqtt_gateway_cursor_prompt_final.md
├── platformio.ini
└── .gitignore
```

## 6. 主要任务与核分配

当前 `src/main.cpp` 中按顺序初始化：

1. 串口日志 `Serial`
2. USB CDC 镜像 `USBSerial`
3. `gateway::ctx::init()`
4. `gateway::config_store::begin()`
5. `gateway::time_sync::begin()`
6. `gateway::mqtt_manager::init()`
7. `gateway::mqtt_uplink::begin()`
8. `gateway::network_task::start()`
9. `gateway::http_server_task::start()`
10. `gateway::heartbeat_led::start()`
11. `gateway::uart2_test_task::start()`
12. `gateway::can_driver::start()`

| 任务 | 核心 | 说明 |
| --- | --- | --- |
| `NET_TASK` | Core 0 | Wi-Fi、SNTP、MQTT 主轮询 |
| `HTTP_SRV` | Core 0 | 同步 WebServer、配网 FSM、堆内存巡检 |
| `mqtt_agg` | Arduino loop 所在核，通常 Core 1 | MQTT 上行聚合 |
| `can_rx` | Core 1 | TWAI RX、AT 编码、USB CDC 镜像、MQTT offer |
| `usb_cdc_mirr` | Core 0 | USB CDC 镜像发送 |
| `HB_LED` | Core 1 | 心跳 LED |
| `UART2_TEST` | Core 1 | Serial2 测试任务 |

`loop()` 当前仅每秒 `vTaskDelay()`，实际业务都在 FreeRTOS 任务和 manager loop 中运行。

## 7. Wi-Fi 与配网

系统使用 AP + STA 共存：

- SoftAP SSID：`PSPRO-xxxx`，其中 `xxxx` 来自 MAC 后 4 位。
- 默认 AP 密码：设计文档中写为 `12345678`，最终以 `wifi_manager` 实现为准。
- 配网入口：`http://192.168.4.1:80/`
- STA 通过 DHCP 获取 IP，例如 `192.168.1.x`。
- Wi-Fi / MQTT 配置保存在 NVS。

常见 HTTP API：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/api/wifi/connect` | 配置 Wi-Fi SSID / password |
| POST | `/api/wifi/disconnect` | 断开 STA，保留 NVS 配置 |
| GET | `/api/wifi/scan` | 异步扫描 Wi-Fi |
| GET | `/api/wifi/provision` | 查询配网状态 |
| POST | `/api/mqtt/config` | 配置 MQTT host / port / username / password |
| GET | `/api/status` | 查询设备状态 |
| GET | `/api/live_state` | Web 页面轮询使用的实时状态 |
| POST | `/api/factory_reset` | 清空配置并重启 |
| POST | `/api/dev/provision_outcome` | 开发 / 联调用配网结果注入 |

## 8. MQTT 设计

默认 Broker：

```text
broker.hivemq.com:1883
```

Topic 规则：

| 方向 | Topic |
| --- | --- |
| 上行 | `topic_ps_pro/vehicle_upload/<8位小写十六进制 product_id>` |
| 下行 | `topic_ps_pro/vehicle_download/<8位小写十六进制 product_id>` |
| LWT | `topic_ps_pro/will/<8位小写十六进制 product_id>` |

其他规则：

- ClientId：`pspro-<8位hex>`
- LWT payload：`offline`
- LWT retain：`true`
- 上行 publish：QoS 0，retain false
- 下行 subscribe：QoS 1
- MQTT 启动条件：STA 已连接且已取得 `product_id`
- `product_id` 变化时断开并重绑 topic

## 9. CAN 与 AT 数据链路

### 9.1 product_id 获取

设计文档说明，product 宣告帧参考扩展帧 ID：

```text
0x16174B03
```

当前实现采用低 24 位匹配：

```cpp
(id29 & 0xFFFFFF) == (0x16174B03 & 0xFFFFFF)
```

Payload 按 little-endian 解析为 32 位 product id，并转成 8 位小写十六进制字符串保存到 `gateway_context`。

### 9.2 CAN → AT → MQTT

典型路径：

```text
TWAI RX
  -> can_rx
  -> at_protocol 编码 AT 行
  -> USBSerial 镜像队列
  -> mqtt_uplink::offer_at_binary()
  -> PacketRingBuffer
  -> mqtt_agg 聚合
  -> mqtt_manager publish
```

无 NTP 墙钟时，系统仍会进行 USB CDC 镜像，但 MQTT 上行需满足 `time_sync::can_use_wall_timestamp_for_upload()`。

### 9.3 MQTT 上行聚合参数

| 参数 | 值 |
| --- | --- |
| Ring 总字节 | 48 × 1024 |
| 正常聚合周期 | 500 ms |
| 高水位加速周期 | 200 ms |
| 高水位阈值 | 20 × 1024 bytes |
| 单批最大 | 4096 bytes |
| PubSubClient buffer | 8192 bytes |
| 首次 MQTT 成功前 | 不入队，丢弃并计数 |
| Ring 满 | 丢弃最旧完整包，给新包腾空间 |

## 10. 构建与烧录

### 10.1 安装 PlatformIO

```bash
python -m pip install platformio
```

或使用 VS Code + PlatformIO 插件。

### 10.2 克隆仓库

```bash
git clone https://github.com/forlorg/software-gateway.git
cd software-gateway
```

### 10.3 编译

```bat
python -m platformio run
```

### 10.4 侦测串口

```bat
python -m serial.tools.list_ports -v
```

示例：

```text
COM17
    desc: USB-Enhanced-SERIAL CH343 (COM17)
    hwid: USB VID:PID=1A86:55D3 SER=5C38144399 LOCATION=1-1.2
```

### 10.5 烧录

```bat
python -m platformio run --target upload --upload-port COM<端口号>
```

### 10.6 串口监控

```bat
python -m platformio device monitor --port COM<端口号> --baud 115200
```

### 10.7 常用完整流程

```bat
python -m platformio run --target upload --upload-port COM<端口号> && python -m platformio device monitor --port COM<端口号> --baud 115200
```

## 11. 开发规则

根据 `CLAUDE.md`：

- AI 生成和维护范围包括：
  - `src/`
  - `tests/`
  - `tools/`
  - `context-docs/`
- AI 不得修改 `request-docs/` 下的任何文件。
- 工作流顺序：
  1. 理解需求：阅读 `request-docs/`
  2. 设计：在 `context-docs/` 产出设计文档
  3. 实现：在 `src/` 编写代码
  4. 验证：编译、烧录、监控日志
- 代码风格：
  - C++17
  - 遵循现有命名和模块结构
  - 优先使用 Arduino Framework API
  - 日志前缀统一使用 `[模块名]`

## 12. 后续建议

### 12.1 OTA

当前 OTA 未实现，建议新增：

```text
src/system/version.h
src/system/version.cpp
src/system/ota_manager.h
src/system/ota_manager.cpp
src/config/ota_config.h
partitions_ota_8mb.csv 或 partitions_ota_16mb.csv
```

并提供：

- `/api/version`
- `/api/ota/status`
- `/api/ota/check`
- MQTT OTA 状态 Topic
- 远程 manifest 检查
- HTTPS 固件下载
- SHA-256 / 签名校验
- 双 OTA 分区回滚保护

### 12.2 Watchdog 与健康检查

建议补充：

- 任务级 Watchdog 注册
- CAN / Wi-Fi / MQTT / HTTP / NVS 健康状态
- OTA 新固件启动后的 pending verify 检查
- 低堆内存告警字段进入 `/api/live_state`

### 12.3 文档自动生成

`request-docs/*.yaml` 是 CAN 协议定义来源。建议后续增加脚本：

```text
YAML -> C++ 解析表
YAML -> Markdown 协议文档
YAML -> MQTT payload schema
```

## 13. License

当前 GitHub 仓库页面显示未提供描述、主题与 Release 信息；使用或分发前应确认项目作者授权以及第三方依赖许可证。
