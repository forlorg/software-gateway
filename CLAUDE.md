# CLAUDE.md — AI Agent 项目指令

---

## 1. 环境约束

- OS: Windows
- Editor: VS Code + Claude Code
- 工具链: PlatformIO Core 6.1.19（通过 `python -m platformio` 调用）
- Python: 系统已安装 `pyserial`（用于串口侦测）
- 目标硬件: ESP32-S3-DevKitC-1 (QFN56), 240MHz, 320KB RAM, 8MB Flash, PSRAM 8MB
- 串口芯片: CH343 USB-to-Serial（设备插入后通常映射为 COMx）

---

## 2. 构建命令

### 2.1 编译（不烧录）

```bat
cd <项目根目录>
python -m platformio run
```

### 2.2 侦测串口

```bat
python -m serial.tools.list_ports -v
```

输出示例：
```
COM17
    desc: USB-Enhanced-SERIAL CH343 (COM17)
    hwid: USB VID:PID=1A86:55D3 SER=5C38144399 LOCATION=1-1.2
```

### 2.3 编译并烧录

```bat
python -m platformio run --target upload --upload-port COM<端口号>
```

> `--upload-port` 为命令行覆写，优先级高于 `platformio.ini` 中的 `upload_port`（当前已注释掉，按需指定端口）。

### 2.4 读取设备日志（串口监控）

```bat
python -m platformio device monitor --port COM<端口号> --baud 115200
```

> 默认波特率 `monitor_speed = 115200` 已在 `platformio.ini` 中设定，与设备固件一致。
>
> 退出监控：`Ctrl+C`；菜单：`Ctrl+T`。

### 2.5 一键编译+烧录+监控（常用完整流程）

```bat
python -m platformio run --target upload --upload-port COM<端口号> && python -m platformio device monitor --port COM<端口号> --baud 115200
```

---

## 3. 项目结构

| 目录/文件 | 说明 |
|-----------|------|
| `platformio.ini` | PlatformIO 项目配置（板卡、框架、依赖、编译标志） |
| `src/main.cpp` | 固件入口 |
| `src/can/` | CAN 驱动、收发、流量统计 |
| `src/config/` | 配置存储（Preferences/NVS） |
| `src/network/` | WiFi 管理、MQTT 管理、配网 FSM、Web 服务器、SSE |
| `src/protocol/` | AT 协议处理 |
| `src/system/` | 网关上下文、状态机、时间同步、统计 |
| `src/task/` | FreeRTOS 任务：CAN 环形缓冲刷新、心跳 LED、MQTT 上行、UART2 测试、Web 服务、WiFi AP |
| `src/utils/` | 数据包环形缓冲区 |
| `.pio/` | 构建产物与缓存 |

### 3.1 任务/核分配

| 任务 | 核心 | 备注 |
|------|------|------|
| WiFi AP 任务 | Core 0 | 管理 SoftAP 配网 |
| Web Server 任务 | Core 0 | Arduino 同步 WebServer，端口 80 |
| 心跳 LED 任务 | Core 1 | GPIO 1，500ms 半周期 |
| UART2 测试任务 | Core 1 | Serial2 230400 baud，RX=18 TX=17 |

### 3.2 关键运行时参数

| 参数 | 值 |
|------|-----|
| CPU 频率 | 240 MHz |
| SDK 版本 | v4.4.7-dirty |
| Flash 大小 | 16 MB |
| 堆空闲 (启动时) | ~279 KB |
| RAM 使用率 | ~30% (98456 / 327680 B) |
| Flash 使用率 | ~24.7% (826873 / 3342336 B) |
| SoftAP SSID | PSPRO-xxxx (MAC 后 4 位) |
| 配网地址 | http://192.168.4.1:80/ |
| STA 获取 IP | 192.168.1.x（DHCP） |

---

## 4. 依赖库

| 库 | 版本 | 用途 |
|----|------|------|
| `bblanchon/ArduinoJson` | 7.4.3 | JSON 序列化/反序列化 |
| `knolleary/PubSubClient` | 2.8.0 | MQTT 客户端 |
| `Preferences` | 2.0.0 (内置) | NVS 持久化存储 |
| `WiFi` | 2.0.0 (内置) | WiFi STA/AP |
| `WebServer` | 2.0.0 (内置) | HTTP 配网页 |

---

## 5. AI 生成工件范围

以下目录和文件由 AI 生成和维护：

- `src/`（C++ 源码）
- `tests/`（QTest 测试）
- `tools/`（验证脚本和辅助工具）
- `context-docs/`（规格化文档）

**AI 不得修改 `request-docs/` 下的任何文件。**

---

## 6. 工作流规则

### 6.1 阶段顺序

1. **理解需求** → 阅读 `request-docs/` 中的需求文档
2. **设计** → 在 `context-docs/` 中产出设计文档
3. **实现** → 在 `src/` 中编写代码
4. **验证** → 编译 → 烧录 → 监控日志确认功能正常

### 6.2 代码风格

- C++17 标准 (`-std=gnu++17`)
- 遵循项目现有命名约定和代码结构
- 使用 Arduino 框架 API（勿直接调用 ESP-IDF 底层 API，除非必要）
- 日志前缀使用 `[模块名]` 格式（如 `[GW_SYS]`, `[WIFI_STA]`, `[HB_LED]`）
