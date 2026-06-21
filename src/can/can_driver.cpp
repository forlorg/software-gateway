/**
 * @file can_driver.cpp
 * @brief TWAI 驱动安装、启动，并拉起 CAN 收/发任务与队列。
 */

#include "can_driver.h"
#include "can_rx.h"

#include <Arduino.h>
#include <driver/twai.h>

#include "can/can_tx.h"

namespace gateway::can_driver {

    void start() {
        twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
            static_cast<gpio_num_t>(can_hw::kTxPin), static_cast<gpio_num_t>(can_hw::kRxPin), TWAI_MODE_NORMAL);
        twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
        twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
        twai_driver_install(&g_config, &t_config, &f_config);
        twai_start();
        can_tx::init();
        can_rx_start_task();
    }

} // namespace gateway::can_driver
