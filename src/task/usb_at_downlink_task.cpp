/**
 * @file usb_at_downlink_task.cpp
 * @brief 从 USB CDC 接收上位机下发的 AT Packet，解析为 CAN 帧并进入普通 CAN TX 队列。
 */

#include "task/usb_at_downlink_task.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "can/can_tx.h"
#include "protocol/at_protocol.h"

namespace gateway::usb_at_downlink_task {

    namespace {
        TaskHandle_t g_task{};
        volatile bool g_has_last_channel_no{false};
        volatile uint8_t g_last_channel_no{0};

        // AT 包最大长度仅约 268 B，512 B 足够容纳单包和少量半包重叠。
        constexpr size_t kRxBufferBytes = 512;
        constexpr size_t kReadBurstLimit = 128;
        constexpr uint32_t kIdleDelayMs = 2;
        constexpr uint32_t kActiveDelayMs = 1;
        constexpr uint32_t kStackReportIntervalMs = 5000;

        int usb_available() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
            return Serial.available();
#else
            return USBSerial.available();
#endif
        }

        int usb_read_one() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
            return Serial.read();
#else
            return USBSerial.read();
#endif
        }

        void preserve_possible_sync_tail(uint8_t *work, size_t *io_len) {
            if (!work || !io_len) {
                return;
            }
            if (*io_len > 0 && work[*io_len - 1] == 'A') {
                work[0] = 'A';
                *io_len = 1;
            } else {
                *io_len = 0;
            }
        }

        void on_decoded_at_frame(void *, const at_protocol::DecodedAtFrame &frame) {
            g_last_channel_no = frame.channel_no;
            g_has_last_channel_no = true;

            if (!can_tx::enqueue(frame.msg)) {
                Serial.printf("[USB dn] CAN TX normal queue full; drop id=0x%08lX dlc=%u ch=%u\r\n",
                    static_cast<unsigned long>(frame.msg.identifier & 0x1FFFFFFFu),
                    static_cast<unsigned>(frame.msg.data_length_code),
                    static_cast<unsigned>(frame.channel_no));
            }
        }

        void task_fn(void *) {
            static uint8_t work[kRxBufferBytes];
            size_t work_len = 0;
            uint32_t report_elapsed_ms = 0;

            for (;;) {
                bool got_any = false;
                size_t burst = 0;

                while (burst < kReadBurstLimit) {
                    const int available = usb_available();
                    if (available <= 0) {
                        break;
                    }

                    const int c = usb_read_one();
                    if (c < 0) {
                        break;
                    }
                    got_any = true;
                    ++burst;

                    if (work_len >= sizeof(work)) {
                        preserve_possible_sync_tail(work, &work_len);
                    }
                    work[work_len++] = static_cast<uint8_t>(c & 0xFF);
                }

                if (work_len > 0) {
                    at_protocol::consume_at_buffer_ex(work, &work_len, nullptr, on_decoded_at_frame);

                    // 连续噪声或异常长度半包导致缓冲接近满时，主动保留可能跨边界的 'A' 后重同步。
                    if (work_len > sizeof(work) - at_protocol::kAtLineMaxBytes) {
                        preserve_possible_sync_tail(work, &work_len);
                    }
                }

                const uint32_t delay_ms = got_any ? kActiveDelayMs : kIdleDelayMs;
                report_elapsed_ms += delay_ms;
                if (report_elapsed_ms >= kStackReportIntervalMs) {
                    report_elapsed_ms = 0;
                    const UBaseType_t stack_watermark = uxTaskGetStackHighWaterMark(nullptr);
                    Serial.printf("[USB dn] stack_hwm=%u words (%u B free)\r\n",
                        static_cast<unsigned>(stack_watermark),
                        static_cast<unsigned>(stack_watermark * sizeof(StackType_t)));
                }

                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }
    } // namespace

    void start() {
        if (g_task) {
            return;
        }

        const BaseType_t result = xTaskCreatePinnedToCore(
            task_fn, "usb_at_dn", kTaskStackBytes, nullptr,
            static_cast<UBaseType_t>(kTaskPriority), &g_task,
            static_cast<BaseType_t>(kTaskCore));

        if (result != pdPASS) {
            g_task = nullptr;
            Serial.println("[USB dn] FAIL create USB AT downlink task");
            return;
        }

        Serial.printf("[USB dn] ready core=%d priority=%u rxbuf=%u\r\n",
            kTaskCore, static_cast<unsigned>(kTaskPriority),
            static_cast<unsigned>(kRxBufferBytes));
    }

    bool has_last_channel_no() {
        return g_has_last_channel_no;
    }

    uint8_t last_channel_no() {
        return g_last_channel_no;
    }

} // namespace gateway::usb_at_downlink_task
