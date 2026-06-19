#pragma once

/**
 * @file can_driver.h
 * @brief CAN 硬件引脚与驱动启动接口。
 */

namespace gateway::can_hw {

    constexpr int kRxPin = 5;
    constexpr int kTxPin = 4;

} // namespace gateway::can_hw

namespace gateway::can_driver {

    void start();

} // namespace gateway::can_driver
