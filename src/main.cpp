/**
 * @file main.cpp
 * @brief 固件入口：初始化子系统、启动网络与 CAN；`loop` 仅占位让出 CPU。
 */

#include <Arduino.h>

#include "system/gateway_context.h"
#include "config/config_store.h"
#include "can/can_driver.h"
#include "task/can_ring_flush_task.h"
#include "task/heartbeat_led_task.h"
#include "system/time_sync.h"
#include "task/http_server_task.h"
#include "task/network_task.h"
#include "task/uart2_test_task.h"

#include "network/mqtt_manager.h"
#include "task/mqtt_uplink.h"
#include "task/ota_task.h"

static constexpr const char kSysTag[] = "GW_SYS";

/** UART0 调试日志：与 `variants/esp32s3/pins_arduino.h` 一致（RX=44, TX=43），经板载 USB-UART 桥。 */
static constexpr int kLogUartRx = 44;
static constexpr int kLogUartTx = 43;

/** Arduino 启动：按依赖顺序初始化 NVS、MQTT、配网任务、Web、CAN。 */
void setup() {
  Serial.begin(115200, SERIAL_8N1, kLogUartRx, kLogUartTx);
#if ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
  /* HWCDC：片内固定 USB 描述符，无法在固件中设置 iProduct/iInterface；见 docs/MQTT_DATA_PIPELINE.md §9。 */
  USBSerial.setTxBufferSize(4096);
  USBSerial.begin(921600);
#endif
  delay(200);
  Serial.println();
  Serial.printf("[%s] model=%s, cpu=%u MHz, flash=%u B, heap_free=%u B, sdk=%s\n", kSysTag,
                ESP.getChipModel(), static_cast<unsigned>(ESP.getCpuFreqMHz()),
                ESP.getFlashChipSize(), static_cast<unsigned>(ESP.getFreeHeap()), ESP.getSdkVersion());

  gateway::ctx::init();

  gateway::config_store::begin();
  gateway::time_sync::begin();

  gateway::mqtt_manager::init();

  gateway::mqtt_uplink::begin();

  gateway::network_task::start();
  delay(350);
  gateway::http_server_task::start();
  gateway::heartbeat_led::start();

  // gateway::uart2_test_task::start();

  gateway::can_driver::start();
  gateway::ota_task::start();
}

/** 主循环留空；业务在 FreeRTOS 任务与 `wifi_ap_task` 内轮询中运行。 */
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
