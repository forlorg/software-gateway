/**
 * @file uart2_test_task.cpp
 * @brief 临时任务：`Serial2` 定时发送测试数据；收到数据时用 `Serial` 打印日志。
 */

#include "uart2_test_task.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway::uart2_test_task {

namespace {

static constexpr const char kLogTag[] = "UART2_TST";
static constexpr size_t kRxChunkMax = 256;

void task_uart2_test(void *) {
  Serial2.begin(kBaud, SERIAL_8N1, kUart2RxPin, kUart2TxPin);
  Serial.printf("[%s] Serial2 %u baud RX=%d TX=%d, task core %d\n", kLogTag,
                static_cast<unsigned>(kBaud), kUart2RxPin, kUart2TxPin,
                static_cast<int>(xPortGetCoreID()));

  uint32_t seq = 0;
  uint8_t rxbuf[kRxChunkMax];

  for (;;) {
    for (;;) {
      const int n = Serial2.read(rxbuf, static_cast<int>(sizeof(rxbuf)));
      if (n <= 0) {
        break;
      }
      Serial.printf("[%s] RX %d bytes:", kLogTag, n);
      for (int i = 0; i < n; ++i) {
        Serial.printf(" %02X", rxbuf[static_cast<size_t>(i)]);
      }
      Serial.print(" | ");
      for (int i = 0; i < n; ++i) {
        const uint8_t c = rxbuf[static_cast<size_t>(i)];
        Serial.print((c >= 32 && c < 127) ? static_cast<char>(c) : '.');
      }
      Serial.println();
    }

    Serial2.printf("GW_UART2_TEST seq=%lu tick=%lu\n", static_cast<unsigned long>(++seq),
                   static_cast<unsigned long>(millis()));
    vTaskDelay(pdMS_TO_TICKS(kSendPeriodMs));
  }
}

} // namespace

void start() {
  Serial.printf("[%s] starting pinned core=%d stack=%lu\n", kLogTag, kPinnedCore,
                static_cast<unsigned long>(kTaskStackBytes));
  xTaskCreatePinnedToCore(task_uart2_test, "uart2_test", kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), nullptr,
                          static_cast<BaseType_t>(kPinnedCore));
}

} // namespace gateway::uart2_test_task
