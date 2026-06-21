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
- 将 CAN 接收帧和 ADC 生成的 CAN 格式压力帧统一编码为 AT 行，并通过 USB CDC 镜像与 MQTT 上报。
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
| CAN/ADC → AT 编码 | 已实现 | 接收帧与 ADC 压力帧共用 AT 分发器，AT 行经 USB CDC 镜像并按条件进入 MQTT 上行 |
| ADC 压力采样 | 已实现 | ADS7924 四通道采样、4 点块平均滤波、PGN 0x1708 打包发送 |
| Web 标定 / 刷写指令 | 已实现 | 支持离合器标定命令与 CAN Flash 指令下发 |
| 时间同步 | 已实现 | STA 联网后 SNTP，默认 `pool.ntp.org` |
| 状态灯 | 已实现 | GPIO1 心跳灯，GPIO2 网络/MQTT 状态灯 |
| OTA | 已实现 | 已具备 Manifest 检查、下载、MD5/SHA-256 校验和 OTA 分区写入，并已实现域名优先、单 IPv4 缓存与局域网兜底 |
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
│   ├── transport/
│   │   └── at_frame_dispatcher.cpp / .h
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
8. 初始化统一 AT 分发器及 USB CDC 镜像任务 `gateway::at_frame_dispatcher::begin()`。
9. 启动网络任务 `gateway::network_task::start()`。
10. 启动 HTTP 服务任务 `gateway::http_server_task::start()`。
11. 启动状态灯任务 `gateway::status_led::start()`。
12. 启动 CAN 驱动与 CAN TX/RX 任务 `gateway::can_driver::start()`。
13. 启动 ADS7924 压力采样、CAN 发送及 AT 非阻塞分发任务 `gateway::adc_pressure_can_task::start()`。
14. 启动 OTA 任务 `gateway::ota_task::start()`。

`loop()` 当前仅每秒 `vTaskDelay()`，实际业务在 FreeRTOS 任务和 manager loop 中运行。

| 任务 | 核心 | 说明 |
| --- | --- | --- |
| `NET_TASK` | Core 0 | Wi-Fi、SNTP、MQTT 主轮询 |
| `HTTP_SRV` | Core 0 | 同步 WebServer、配网 FSM、堆内存巡检 |
| `mqtt_agg` | Core 0 | MQTT 上行聚合发布 |
| `can_rx` | Core 1 | TWAI RX、业务解析，并将接收帧交给统一 AT 分发器 |
| `can_tx` | Core 1 | TWAI TX 队列发送 |
| `usb_cdc_mirr` | Core 0 | 发送 AT 分发器投递的 USB CDC 镜像 |
| `adc_pressure_can` | Core 1 | ADS7924 压力采样、滤波、PGN 0x1708 CAN 入队及 AT 非阻塞分发 |
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

### 9.2 CAN/ADC → AT → USB/MQTT

接收帧与 ADC 压力帧共用同一个 AT 编码和输出入口：

```text
TWAI RX -> can_rx ----------------------┐
                                        ├-> at_frame_dispatcher
ADS7924 -> PGN 0x1708 -> can_tx --------┘      ├-> USBSerial 镜像队列
                                               └-> mqtt_uplink::offer_at_binary()
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
  -> can_tx 高优先级队列 -> TWAI transmit
  -> at_frame_dispatcher -> USB CDC 镜像 + MQTT 上行 RingBuffer
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

OTA 由 `src/system/ota_manager.*` 和 `src/task/ota_task.*` 实现，配置位于 `src/config/ota_config.h` 与 `platformio.ini`。当前代码已具备 manifest 检查、固件下载、MD5/SHA-256 校验、OTA 分区写入、Web 触发和失败退避。

> 实施状态说明：当前源码已经实现“缓存域名 IP → 域名 DNS → 局域网备用”的候选链，并实现缓存签名校验、HTTP Host 保持、manifest 来源绑定及下载失败后下一端点重新 check。当前阶段仍使用 HTTP；HTTPS 50443 仅保留配置与客户端抽象，尚未启用。

当前 `platformio.ini` 中的固件标识：

| 字段 | 当前值 |
| --- | --- |
| Project | `ps-pro-gateway` |
| Version | `1.0.0` |
| Build | `2026061803` |
| Hardware | `1.1` |
| Channel | `stable` |

### 10.1 已确认的 OTA 地址

当前阶段使用 HTTP：

| 类型 | 地址 |
| --- | --- |
| 主域名 check | `http://do-update.top:50080/api/v1/ota/check` |
| 局域网备用 check | `http://192.168.1.101:50080/api/v1/ota/check` |

未来启用 TLS 时使用：

```text
https://do-update.top:50443/api/v1/ota/check
```

未来 HTTPS 端口为 `50443`，不是 `443`。当前阶段不启用 TLS。

### 10.2 当前访问顺序

没有缓存域名 IP：

```text
DNS 解析 do-update.top
  -> 域名 check
  -> 失败时访问 192.168.1.101
```

已有缓存域名 IP：

```text
缓存 IP check
  -> 失败则删除缓存
  -> 重新 DNS 解析 do-update.top
  -> 仍失败则访问 192.168.1.101
```

固定优先级：

```text
有缓存：缓存 IP -> 域名 -> 局域网
无缓存：域名 -> 局域网
```

所有候选均失败后才进入现有 Backoff。

### 10.3 域名 IP 缓存

- 域名只使用并缓存一个 IPv4。
- 只有域名解析、连接和有效 check 全部成功后才保存 IP。
- `update_available=false` 也属于有效 check 成功，可以保存 IP。
- 只有 DNS 成功或 TCP 成功不能保存 IP。
- 局域网备用服务器成功不得写入域名缓存。
- 缓存 IP 的 check 或固件下载失败时立即删除缓存。
- 当前不设置固定 TTL；缓存可用就继续使用，不可用就删除并重新解析。
- factory reset 应同步清除该缓存。
- 缓存应与 `do-update.top`、scheme 和端口绑定，配置变化时旧缓存失效。

### 10.4 缓存 IP 的 HTTP Host

缓存 IP 和本次 DNS 解析 IP 只是 TCP 传输地址，逻辑 OTA 源仍是：

```text
http://do-update.top:50080
```

通过裸 IP 连接时，HTTP Host 必须保持：

```text
do-update.top:50080
```

局域网备用连接的逻辑源和 Host 才是：

```text
192.168.1.101:50080
```

这一区分用于支持域名虚拟主机、反向代理和严格同源校验。

### 10.5 check 成功标准

一次 check 只有在以下条件满足时才算成功：

1. TCP/HTTP 请求成功；
2. HTTP 状态码为 `200`；
3. 不发生、不跟随重定向；
4. manifest 未超过大小限制；
5. JSON 解析成功；
6. `ok == true`；
7. 无更新时直接成功；
8. 有更新时，project、hw、channel、version、build、size、MD5、SHA-256 和固件 path/URL 全部校验通过。

当前候选失败时继续下一候选，不立即进入全局 Backoff。

### 10.6 固件下载与端点回退

每个 manifest 必须绑定其来源端点：

```text
同一端点 check
  -> 同一端点 manifest
  -> 同一端点下载
```

固件下载失败时：

1. 中止当前下载和 OTA 写入；
2. 切换到下一候选；
3. 在下一候选重新执行 check；
4. 使用下一候选新返回的 manifest 下载。

不得拿域名 manifest 直接去局域网下载，也不得拿局域网 manifest 直接去域名下载。

典型回退：

```text
缓存 IP 下载失败
  -> 删除缓存
  -> 域名重新 check

域名下载失败
  -> 局域网重新 check

局域网下载失败
  -> Backoff
```

### 10.7 固件 URL 与重定向

继续优先使用服务端返回的 `firmware_path`：

```text
/api/v1/ota/ps-pro-gateway/{version}/firmware.bin
```

- 域名和缓存 IP 候选的逻辑同源为 `http://do-update.top:50080`。
- 局域网候选的逻辑同源为 `http://192.168.1.101:50080`。
- 不允许跨域、跨端口或跨 scheme。
- HTTP 301/302 等重定向继续拒绝，不跟随。

### 10.8 保留的升级校验

现有升级校验继续保留：

1. manifest 响应大小限制；
2. 固件 size 不超过 OTA app 分区；
3. `Content-Length == manifest.size`；
4. MD5；
5. SHA-256；
6. `Update.begin/write/end`；
7. 下载空闲超时；
8. 写入失败中止；
9. 成功后重启；
10. OTA 下载继续在独立低优先级任务中运行。

### 10.9 未来 HTTPS

切换到 `https://do-update.top:50443` 时，需要额外实现：

- `WiFiClientSecure`；
- Root CA；
- 系统时间和证书有效期校验；
- 禁止默认 `setInsecure()`；
- 缓存 IP 直连时仍使用 `do-update.top` 作为 TLS SNI 和证书主机名；
- HTTP Host 使用 `do-update.top:50443`。

不能把 HTTPS URL 简单替换为裸 IP 后继续连接。

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
- 为 OTA 域名优先、单 IPv4 缓存和局域网兜底流程补充设备实机与故障注入测试。
- 为 OTA 新固件启动后的 pending verify / 回滚确认补充显式逻辑。
- 后续将 HTTP 主域名切换到 `https://do-update.top:50443`，并补充 CA、SNI 与证书主机名校验。
- 为 `request-docs/*.yaml` 增加自动生成脚本，例如：
  - YAML → C++ 解析表。
  - YAML → Markdown 协议文档。
  - YAML → MQTT payload schema。
- 增加 CI 格式检查，确保 `.h/.cpp` 始终保持 4 空格缩进。

## 14. License

当前仓库未看到明确的 License 文件；使用、分发或商用前应确认项目作者授权以及第三方依赖许可证。
