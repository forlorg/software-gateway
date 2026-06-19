#pragma once

/**
 * @file can_traffic_stats.h
 * @brief CAN 接收与 MQTT 上行流量统计接口及网页快照结构体。
 */

#include <cstdint>

namespace gateway::can_traffic_stats {

    /** 每收到一帧 TWAI 成功读出的报文调用一次（含后续因 NTP 等丢弃的帧）。 */
    void record_rx_frame(uint8_t dlc);

    /** 聚合后拟上行（MQTT）的字节数，每发出一批加一次。 */
    void record_uplink_bytes(uint32_t n);

    /** 每个上行聚合周期结束时调用，用于刷新近 5s 速率样本。 */
    void on_aggregate_tick();

    struct WebSnapshot {
        uint64_t rx_frames{};
        uint64_t rx_payload_bytes{};
        uint64_t uplink_bytes{};
        float rx_fps_5s{};
        float rx_payload_bytes_s_5s{};
        float uplink_bytes_s_5s{};
    };

    WebSnapshot get_web_snapshot();

} // namespace gateway::can_traffic_stats
