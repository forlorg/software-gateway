#pragma once

/**
 * @file statistics.h
 * @brief 全局计数器累加与查询声明。
 */

#include <cstdint>

namespace gateway::statistics {

void add_can_rx(uint32_t n);
void add_can_tx(uint32_t n);
void add_mqtt_tx(uint32_t n);
void add_mqtt_rx(uint32_t n);
void add_dropped(uint32_t n);
void add_serial_mirror_queue_drops(uint32_t n);

uint32_t can_rx();
uint32_t can_tx();
uint32_t mqtt_tx();
uint32_t mqtt_rx();
uint32_t dropped();
uint32_t serial_mirror_queue_drops();

uint32_t uptime_ms();

} // namespace gateway::statistics
