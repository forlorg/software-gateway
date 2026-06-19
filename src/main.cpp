/**
 * @file main.cpp
 * @brief 固件入口：初始化子系统、启动网络、状态灯、CAN 与 ADC 压力采样任务；`loop` 仅占位让出 CPU。
 */
#include <Arduino.h>

#include "can/can_driver.h"
#include "config/config_store.h"
#include "network/mqtt_manager.h"
#include "system/gateway_context.h"
#include "system/time_sync.h"
#include "task/adc_pressure_can_task.h"
#include "task/http_server_task.h"
#include "task/mqtt_uplink.h"
#include "task/network_task.h"
#include "task/ota_task.h"
#include "task/status_led_task.h"
#include "task/uart2_test_task.h"

/** UART0 调试日志：与 `variants/esp32s3/pins_arduino.h` 一致（RX=44, TX=43），经板载 USB-UART 桥。 */
static constexpr int kLogUartRx = 44;
static constexpr int kLogUartTx = 43;

/** Arduino 启动：按依赖顺序初始化 NVS、MQTT、网络任务、Web、状态灯、CAN 与 ADC 压力采样。 */
void setup() {
    Serial.begin(115200, SERIAL_8N1, kLogUartRx, kLogUartTx);
#if ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
    /* HWCDC：片内固定 USB 描述符，无法在固件中设置 iProduct/iInterface；见 docs/MQTT_DATA_PIPELINE.md §9。 */
    USBSerial.setTxBufferSize(4096);
    USBSerial.begin(921600);
#endif

    delay(200);
    Serial.print("\r\n");
    Serial.printf(
        "[%s] model=%s, cpu=%u MHz, flash=%u B, heap_free=%u B, sdk=%s\r\n",
        "MAIN", ESP.getChipModel(), static_cast<unsigned>(ESP.getCpuFreqMHz()),
        static_cast<unsigned>(ESP.getFlashChipSize()),
        static_cast<unsigned>(ESP.getFreeHeap()), ESP.getSdkVersion());

    gateway::ctx::init();
    gateway::config_store::begin();
    gateway::time_sync::begin();
    gateway::mqtt_manager::init();
    gateway::mqtt_uplink::begin();
    gateway::network_task::start();

    delay(350);

    gateway::http_server_task::start();
    gateway::status_led::start();

    // gateway::uart2_test_task::start();

    gateway::can_driver::start();
    gateway::adc_pressure_can_task::start();

    gateway::ota_task::start();
}

/** 主循环留空；业务在 FreeRTOS 任务与网络管理器轮询中运行。 */
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
