/**
 * @file can_rx.cpp
 * @brief CAN 接收 FreeRTOS 任务：统计、业务解析，并将接收帧交给统一 AT 输出分发器。
 */

#include "can_rx.h"

#include <driver/twai.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "can/can_parsed_data.h"
#include "can/can_traffic_stats.h"
#include "system/statistics.h"
#include "transport/at_frame_dispatcher.h"

namespace {

    /**
     * CAN 接收任务：阻塞接收 TWAI 帧，更新流量/业务统计并刷新设备上下文，
     * 再将完整 CAN 格式帧交给 AT 分发器。USB 与 MQTT 的实际输出均不在本任务执行。
     */
    void rx_task(void *) {
        twai_message_t msg{};

        for (;;) {
            if (twai_receive(&msg, pdMS_TO_TICKS(200)) != ESP_OK) {
                continue;
            }

            gateway::can_traffic_stats::record_rx_frame(msg.data_length_code);
            gateway::statistics::add_can_rx(1);

            if (msg.extd) {
                const uint32_t can_id = msg.identifier & 0x1FFFFFFFu;
                gateway::can_parsed_data::process_frame(
                    can_id, msg.data, msg.data_length_code);
            }

            gateway::at_frame_dispatcher::dispatch(msg);
        }
    }

} // namespace

void can_rx_start_task() {
    gateway::can_parsed_data::init();
    xTaskCreatePinnedToCore(rx_task, "can_rx", gateway::can_rx::kTaskStackBytes, nullptr,
        static_cast<UBaseType_t>(gateway::can_rx::kTaskPriority), nullptr,
        static_cast<BaseType_t>(gateway::can_rx::kTaskCore));
}
