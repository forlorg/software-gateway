/**
 * @file statistics.cpp
 * @brief 简单累计计数：CAN/MQTT 收发与丢弃帧，供调试与网页展示。
 */

#include "statistics.h"
#include <Arduino.h>

namespace gateway::statistics {

    namespace {
        uint32_t g_boot_ms{};
        uint32_t g_can_rx;
        uint32_t g_can_tx;
        uint32_t g_mqtt_tx;
        uint32_t g_mqtt_rx;
        uint32_t g_dropped;
        uint32_t g_serial_mirror_queue_drops;

        uint32_t g_can_tx_queue_drops;
        uint32_t g_can_tx_high_queue_drops;
        uint32_t g_can_tx_transmit_failures;
        uint32_t g_mqtt_uplink_queue_drops;
        uint32_t g_mqtt_publish_queue_drops;
        uint32_t g_mqtt_downlink_can_drops;
    } // namespace

    void add_can_rx(uint32_t n) { g_can_rx += n; }
    void add_can_tx(uint32_t n) { g_can_tx += n; }
    void add_mqtt_tx(uint32_t n) { g_mqtt_tx += n; }
    void add_mqtt_rx(uint32_t n) { g_mqtt_rx += n; }
    void add_dropped(uint32_t n) { g_dropped += n; }
    void add_serial_mirror_queue_drops(uint32_t n) { g_serial_mirror_queue_drops += n; }

    void add_can_tx_queue_drops(uint32_t n) {
        g_can_tx_queue_drops += n;
        add_dropped(n);
    }

    void add_can_tx_high_queue_drops(uint32_t n) { g_can_tx_high_queue_drops += n; }
    void add_can_tx_transmit_failures(uint32_t n) { g_can_tx_transmit_failures += n; }

    void add_mqtt_uplink_queue_drops(uint32_t n) {
        g_mqtt_uplink_queue_drops += n;
        add_dropped(n);
    }

    void add_mqtt_publish_queue_drops(uint32_t n) {
        g_mqtt_publish_queue_drops += n;
        add_dropped(n);
    }

    void add_mqtt_downlink_can_drops(uint32_t n) { g_mqtt_downlink_can_drops += n; }

    uint32_t can_rx() { return g_can_rx; }
    uint32_t can_tx() { return g_can_tx; }
    uint32_t mqtt_tx() { return g_mqtt_tx; }
    uint32_t mqtt_rx() { return g_mqtt_rx; }
    uint32_t dropped() { return g_dropped; }
    uint32_t serial_mirror_queue_drops() { return g_serial_mirror_queue_drops; }

    uint32_t can_tx_queue_drops() { return g_can_tx_queue_drops; }
    uint32_t can_tx_high_queue_drops() { return g_can_tx_high_queue_drops; }
    uint32_t can_tx_transmit_failures() { return g_can_tx_transmit_failures; }
    uint32_t mqtt_uplink_queue_drops() { return g_mqtt_uplink_queue_drops; }
    uint32_t mqtt_publish_queue_drops() { return g_mqtt_publish_queue_drops; }
    uint32_t mqtt_downlink_can_drops() { return g_mqtt_downlink_can_drops; }

    uint32_t uptime_ms() {
        if (g_boot_ms == 0) {
            g_boot_ms = millis();
            return 0;
        }
        return millis() - g_boot_ms;
    }

} // namespace gateway::statistics
