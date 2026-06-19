# software-gateway

`software-gateway` 是一个基于 **ESP32-S3 + Arduino Framework + FreeRTOS** 的 CAN ↔ MQTT 网关项目。项目使用 PlatformIO 构建，面向长期运行场景，实现 Wi-Fi AP/STA 配网、CAN 总线接入、ADS7924 压力采样、AT 协议编码、MQTT 上下行桥接、Web 控制台、HTTP API、状态灯与 OTA 升级等功能。

## 1. 项目目标

在 ESP32-S3 上实现一个稳定运行的车端软件网关：

```text
CAN Bus / ADS7924
        ↕
ESP32-S3 Gateway
        ↕
Wi-Fi / MQTT Broker / Web Console / OTA Server
```

主要目标：

- 通过 ESP-IDF TWAI 驱动收发 CAN 2.0 扩展帧。
- 将 CAN 数据解析、编码为 AT 行，并通过 USB CDC 镜像与 MQTT 上报。
- 从 MQTT 下行 Topic 接收命令并转发到 CAN。
- 通过 SoftAP + Web 页面完成 Wi-Fi / MQTT 配置。
- 通过 NVS 保存配置，实现断电保持。
- 通过 Web 控制台查看系统状态、标定页、流量统计、配网状态。
- 通过 HTTP API 查询版本、实时状态、OTA 状态，并触发 OTA 检查或升级。
- 通过 ADS7924 采集四路压力信号，滤波后封装为 PGN 0x1708 CAN 帧发送。

## 2. 当前实现状态

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| PlatformIO 构建 | 已实现 | `platform = espressif32`，`framework = arduino`，`board = esp32-s3-devkitc-1` |
| Arduino + FreeRTOS | 已实现 | 主要业务运行在独立 FreeRTOS 任务中 |
| Wi-Fi AP/STA | 已实现 | AP + STA 共存，SoftAP 用于配网 |
| Web 控制台 | 已实现 | `WebServer` 同步 HTTP 服务，页面资源内置在 `web_assets.cpp` |
| NVS 配置存储 | 已实现 | `Preferences` 保存 Wi-Fi / MQTT 配置 |
| MQTT 管理 | 已实现 | `PubSubClient + WiFiClient`，支持退避重连与上下行 Topic |
| MQTT 上行聚合 | 已实现 | 48 KB RingBuffer，按周期批量 publish |
| CAN RX/TX | 已实现 | 使用 ESP-IDF TWAI 驱动，TX/RX 各有独立任务 |
| CAN → AT 编码 | 已实现 | AT 行经 USB CDC 镜像，并按条件进入 MQTT 上行 |
| ADC 压力采样 | 已实现 | ADS7924 四通道采样、4 点块平均滤波、PGN 0x1708 打包发送 |
| Web 标定 / 刷写指令 | 已实现 | 支持离合器标定命令与 CAN Flash 指令下发 |
| 时间同步 | 已实现 | STA 联网后 SNTP，默认 `pool.ntp.org` |
| 状态灯 | 已实现 | GPIO1 心跳灯，GPIO2 网络/MQTT 状态灯 |
| OTA | 已实现 | HTTP Manifest 检查、固件下载、MD5 与 SHA-256 校验、`Update` 写入 OTA 分区 |
| WDT 统一监控 | 未实现 | 当前代码未看到统一任务级 Watchdog 管理 |
| 完整 Health 面板 | 部分实现 | Web/API 已有堆内存、状态、流量等字段，仍可继续扩展 |

## 3. 硬件与运行参数

| 参数 | 当前信息 |
| --- | --- |
| MCU | ESP32-S3 |
| 开发板 | ESP32-S3-DevKitC-1 |
| CPU | 240 MHz |
| Flash | `platformio.ini` 配置为 16 MB |
| PSRAM | OPI PSRAM，`BOARD_HAS_PSRAM` |
| 分区表 | `partitions_ota_16mb.csv`，包含 `app0` / `app1` 双 OTA 分区 |
| 调试串口 | `Serial`，115200 baud，RX=44，TX=43 |
| USB CDC 镜像 | `USBSerial`，921600 baud，在 `ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT` 时启用 |
| SoftAP | SSID 为 `PSPRO-xxxx`，默认密码 `12345678` |
| Web 入口 | SoftAP 默认 `http://192.168.4.1/` |
| CAN 标准 | CAN 2.0，非 CAN-FD |
| CAN 驱动 | ESP-IDF TWAI：`driver/twai.h` |
| CAN 波特率 | 250 kbps |
| CAN TX/RX | TX = GPIO4，RX = GPIO5 |
| CAN 过滤器 | 接受全部 |
| ADS7924 I2C | SDA=GPIO11，SCL=GPIO12，INT=GPIO13，RESET=GPIO14，地址 `0x48` |
| ADS7924 采样 | 5 ms 采样组周期，4 组平均后每 20 ms 发送一次压力 CAN 帧 |
| 心跳 LED | GPIO1，高电平有效，500 ms 半周期 |
| 网络 LED | GPIO2，低电平有效；Wi-Fi 半周期 1000 ms，MQTT 半周期 200 ms |
| UART2 测试任务 | 源码保留，但 `main.cpp` 中默认注释未启动 |

## 4. 技术栈

| 类型 | 技术 |
| --- | --- |
| 构建系统 | PlatformIO |
| 平台 | `espressif32` |
| Framework | Arduino |
| 语言标准 | C++17 |
| JSON | ArduinoJson |
| MQTT | PubSubClient + WiFiClient |
| 配置存储 | Preferences / NVS |
| HTTP 服务 | Arduino 内置同步 WebServer |
| CAN 驱动 | ESP-IDF TWAI |
| OTA | Arduino `Update` + `HTTPClient` + `mbedtls` SHA-256 |
| 多任务 | FreeRTOS |

当前项目显式使用同步 WebServer 与 PubSubClient，避免引入 `ESPAsyncWebServer`、`AsyncTCP`、`AsyncMQTT_ESP32` 一类异步网络栈。

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
│   ├── rest_api_test.py
│   ├── serial_ports_enumeration_test.py
│   └── requirements-serial-test.txt
├── src/
│   ├── adc/
│   │   ├── ads7924.cpp / .h
│   │   ├── ads7924_pressure_sampler.cpp / .h
│   │   └── pressure_can_frame.cpp / .h
│   ├── can/
│   │   ├── can_driver.cpp / .h
│   │   ├── can_parsed_data.cpp / .h
│   │   ├── can_rx.cpp / .h
│   │   ├── can_traffic_stats.cpp / .h
│   │   └── can_tx.cpp / .h
│   ├── config/
│   │   ├── config_store.cpp / .h
│   │   └── ota_config.h
│   ├── network/
│   │   ├── mqtt_manager.cpp / .h
│   │   ├── provision_fsm.cpp / .h
│   │   ├── web_assets.cpp / .h
│   │   ├── web_server.cpp / .h
│   │   └── wifi_manager.cpp / .h
│   ├── protocol/
│   │   └── at_protocol.cpp / .h
│   ├── system/
│   │   ├── esp32_loop_core.h
│   │   ├── gateway_context.cpp / .h
│   │   ├── ota_manager.cpp / .h
│   │   ├── state_machine.cpp / .h
│   │   ├── statistics.cpp / .h
│   │   ├── time_sync.cpp / .h
│   │   └── version.cpp / .h
│   ├── task/
│   │   ├── adc_pressure_can_task.cpp / .h
│   │   ├── can_ring_flush_task.cpp / .h
│   │   ├── http_server_task.cpp / .h
│   │   ├── mqtt_uplink.cpp / .h
│   │   ├── network_task.cpp / .h
│   │   ├── ota_task.cpp / .h
│   │   ├── status_led_task.cpp / .h
│   │   └── uart2_test_task.cpp / .h
│   ├── utils/
│   │   └── packet_ringbuffer.cpp / .h
│   └── main.cpp
├── tools/
├── .gitignore
├── CLAUDE.md
├── Prompt.md
├── partitions_ota_16mb.csv
├── platformio.ini
└── README.md
```

## 6. 启动流程与任务

`src/main.cpp` 当前按以下顺序初始化和启动：

1. 初始化 UART0 调试串口 `Serial`。
2. 在条件满足时初始化 USB CDC 镜像口 `USBSerial`。
3. 初始化全局上下文 `gateway::ctx::init()`。
4. 初始化 NVS 配置 `gateway::config_store::begin()`。
5. 初始化 SNTP 时间模块 `gateway::time_sync::begin()`。
6. 初始化 MQTT 管理器 `gateway::mqtt_manager::init()`。
7. 启动 MQTT 上行聚合 `gateway::mqtt_uplink::begin()`。
8. 启动网络任务 `gateway::network_task::start()`。
9. 启动 HTTP 服务任务 `gateway::http_server_task::start()`。
10. 启动状态灯任务 `gateway::status_led::start()`。
11. 启动 CAN 驱动、CAN TX/RX 任务与 USB CDC 镜像任务 `gateway::can_driver::start()`。
12. 启动 ADS7924 压力采样与压力 CAN 帧发送任务 `gateway::adc_pressure_can_task::start()`。
13. 启动 OTA 任务 `gateway::ota_task::start()`。

`loop()` 当前仅每秒 `vTaskDelay()`，实际业务在 FreeRTOS 任务和 manager loop 中运行。

| 任务 | 核心 | 说明 |
| --- | --- | --- |
| `NET_TASK` | Core 0 | Wi-Fi、SNTP、MQTT 主轮询 |
| `HTTP_SRV` | Core 0 | 同步 WebServer、配网 FSM、堆内存巡检 |
| `mqtt_agg` | Arduino loop 所在核，通常 Core 1 | MQTT 上行聚合发布 |
| `can_rx` | Core 1 | TWAI RX、AT 编码、USB CDC 镜像入队、MQTT offer |
| `can_tx` | Core 1 | TWAI TX 队列发送 |
| `usb_cdc_mirr` | Core 0 | USB CDC 镜像发送 |
| `adc_pressure_can` | Core 1 | ADS7924 压力采样、滤波、PGN 0x1708 发送 |
| `OTA_TASK` | Core 0 | OTA manifest 检查、下载、校验与升级 |
| `HB_LED` | Core 1 | GPIO1 心跳灯 |
| 网络 LED 任务 | Core 0 | GPIO2 Wi-Fi/MQTT 状态灯 |
| `sys_load_mon` | Core 1 | UART2/系统负载测试任务，默认未启动 |

## 7. Wi-Fi、Web 与 HTTP API

系统使用 AP + STA 共存：

- SoftAP SSID：`PSPRO-xxxx`，其中 `xxxx` 来自 MAC 后 4 位。
- SoftAP 默认密码：`12345678`。
- 配网入口：`http://192.168.4.1/`。
- STA 通过 DHCP 获取 IP。
- Wi-Fi / MQTT 配置保存在 NVS。

当前 Web/API 路由：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | Web 控制台首页 |
| GET | `/style.css` | Web 控制台样式 |
| GET | `/app.js` | Web 控制台脚本 |
| GET | `/api/wifi/provision` | 查询配网状态 |
| GET | `/api/status` | 查询基础状态 |
| GET | `/api/live_state` | 查询实时状态快照 |
| GET | `/api/version` | 查询固件版本、构建号、硬件、Flash/PSRAM 与堆内存 |
| GET | `/api/ota/status` | 查询 OTA 状态 |
| POST | `/api/ota/check` | 触发 OTA manifest 检查 |
| POST | `/api/ota/upgrade` | 触发 OTA 升级 |
| GET | `/api/page/system_status` | Web 系统状态页数据 |
| GET | `/api/page/calibration` | Web 标定页数据 |
| GET | `/api/page/traffic_stats` | Web 流量统计页数据 |
| GET | `/api/page/provision` | Web 配网页数据 |
| POST | `/api/calibration/submit` | 下发离合器标定 CAN 命令 |
| POST | `/api/flash_firmware` | 下发 CAN Flash 刷写命令 |
| POST | `/api/wifi/connect` | 配置并连接 Wi-Fi |
| POST | `/api/wifi/disconnect` | 断开 STA，保留 NVS 配置 |
| GET | `/api/wifi/scan` | 异步扫描 Wi-Fi |
| POST | `/api/mqtt/config` | 配置 MQTT host / port / username / password |
| POST | `/api/factory_reset` | 清空配置并重启 |
| POST | `/api/dev/provision_outcome` | 开发/联调用配网结果注入 |

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

- ClientId：`pspro-<8位hex>`。
- LWT payload：`offline`。
- LWT retain：`true`。
- 上行 publish：QoS 0，retain false。
- 下行 subscribe：QoS 1。
- MQTT 启动条件：STA 已连接且已取得 `product_id`。
- `product_id` 变化时断开并重绑 topic。

## 9. CAN、AT 与压力采样数据链路

### 9.1 product_id 获取

product 宣告帧参考扩展帧 ID：

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

无 NTP 墙钟时，系统仍会进行 USB CDC 镜像；MQTT 上行需满足 `time_sync::can_use_wall_timestamp_for_upload()`。

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

### 9.4 ADS7924 压力采样 → CAN

压力采样链路：

```text
ADS7924 Manual-Scan
  -> 4 通道 raw
  -> 4 点块平均滤波
  -> mV / CAN raw / kPa 换算
  -> PGN 0x1708 扩展帧
  -> can_tx 队列
  -> TWAI transmit
```

关键参数：

| 参数 | 值 |
| --- | --- |
| ADC 通道数 | 4 |
| ADC 参考电压 | 5000 mV |
| 采样组周期 | 5 ms |
| 滤波窗口 | 4 组 |
| CAN 发送周期 | 20 ms |
| 压力 CAN ID | `0x16170803` |
| GPIO3 | 作为稳定输入采样，用于任务启动日志与状态观察 |

## 10. OTA 设计

OTA 由 `src/system/ota_manager.*` 和 `src/task/ota_task.*` 实现，配置在 `src/config/ota_config.h` 与 `platformio.ini` 的 build flags 中。

当前 `platformio.ini` 中的固件标识：

| 字段 | 当前值 |
| --- | --- |
| Project | `ps-pro-gateway` |
| Version | `1.0.0` |
| Build | `2026061803` |
| Hardware | `1.1` |
| Channel | `stable` |

默认 OTA API 配置：

| 参数 | 默认值 |
| --- | --- |
| Scheme | `http` |
| Host | `192.168.1.101` |
| Port | `50080` |
| Check path | `/api/v1/ota/check` |
| Firmware file | `firmware.bin` |
| 首次自动检查 | 开机 60 s 后 |
| 周期检查 | 6 h |
| 手动检查最小间隔 | 10 s |
| OTA 任务栈 | 12288 bytes |
| OTA 任务核心 | Core 0 |

升级过程：

1. Wi-Fi 连接后进入 OTA 可检查状态。
2. 拉取 manifest，并校验 project、hw、channel、version、build、size、md5、sha256、firmware path/url。
3. 校验固件 URL 是否符合预期 OTA 地址。
4. 下载固件并写入 OTA 分区。
5. 使用 MD5 与 SHA-256 校验固件内容。
6. `Update.end(true)` 完成后重启。

## 11. 构建与烧录

### 11.1 安装 PlatformIO

```bash
python -m pip install platformio
```

或使用 VS Code + PlatformIO 插件。

### 11.2 编译

```bash
python -m platformio run
```

### 11.3 侦测串口

```bash
python -m serial.tools.list_ports -v
```

### 11.4 烧录

```bash
python -m platformio run --target upload --upload-port COM<端口号>
```

macOS/Linux 示例：

```bash
python -m platformio run --target upload --upload-port /dev/ttyUSB0
```

### 11.5 串口监控

```bash
python -m platformio device monitor --port COM<端口号> --baud 115200
```

macOS/Linux 示例：

```bash
python -m platformio device monitor --port /dev/ttyUSB0 --baud 115200
```

## 12. 开发规则

根据 `CLAUDE.md` 与当前项目结构：

- AI 生成和维护范围主要包括：
  - `src/`
  - `tests/`
  - `tools/`
  - `context-docs/`
- 不应修改 `request-docs/` 下的 CAN 协议定义文件。
- 代码风格：
  - C++17。
  - 代码缩进统一使用 4 个空格，不使用 Tab。
  - 遵循现有命名空间、文件划分与模块边界。
  - 优先使用 Arduino Framework API。
  - 日志前缀统一使用 `[模块名]`。
- Web 页面资源当前以内置 raw literal 形式放在 `src/network/web_assets.cpp`；修改时应避免破坏 HTML/CSS/JS 字符串内容。

## 13. 后续建议

- 补充统一任务级 Watchdog 注册与健康检查。
- 将 `/api/live_state` 扩展为更完整的 Health 面板。
- 为 OTA 新固件启动后的 pending verify / 回滚确认补充显式逻辑。
- 为 `request-docs/*.yaml` 增加自动生成脚本，例如：
  - YAML → C++ 解析表。
  - YAML → Markdown 协议文档。
  - YAML → MQTT payload schema。
- 增加 CI 格式检查，确保 `.h/.cpp` 始终保持 4 空格缩进。

## 14. License

当前仓库未看到明确的 License 文件；使用、分发或商用前应确认项目作者授权以及第三方依赖许可证。
