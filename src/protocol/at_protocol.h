#pragma once

/**
 * @file at_protocol.h
 * @brief AT Packet 线格式编解码与流式自同步解析。
 *
 * AT Packet 格式来自 request-docs/13_at_packet_format.yaml：
 *   AT + timestamp(4B BE) + wiredId(4B BE) + payloadLen(1B) + payload + CRLF
 *
 * 本协议层不依赖 MQTT/USB 传输，供 USB 下行、MQTT 下行、CAN 上行编码共用。
 */

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <driver/twai.h>

namespace gateway::at_protocol {

    constexpr uint8_t kAtChannelMaskCan0 = 0x04;
    constexpr uint8_t kAtChannelMaskCan1 = 0x05;

    /** 兼容既有上行编码默认值：当前网关镜像帧按 CAN1 掩码编码。 */
    constexpr uint8_t kAtWiredIdMask = kAtChannelMaskCan1;

    constexpr size_t kAtHeaderBytes = 2;
    constexpr size_t kAtTimestampBytes = 4;
    constexpr size_t kAtWiredIdBytes = 4;
    constexpr size_t kAtPayloadLengthBytes = 1;
    constexpr size_t kAtFixedHeaderBytes =
        kAtHeaderBytes + kAtTimestampBytes + kAtWiredIdBytes + kAtPayloadLengthBytes;
    constexpr size_t kAtTailBytes = 2;
    constexpr size_t kAtPayloadMaxBytes = 255;
    constexpr size_t kCanPayloadMaxBytes = 8;
    constexpr size_t kAtLineMaxBytes = kAtFixedHeaderBytes + kAtPayloadMaxBytes + kAtTailBytes;
    constexpr size_t kAtCanLineMaxBytes = kAtFixedHeaderBytes + kCanPayloadMaxBytes + kAtTailBytes;

    struct DecodedAtFrame {
        uint32_t ts_packed{};
        uint32_t wired_id{};
        uint8_t channel_mask{};
        uint8_t channel_no{};
        uint32_t can_id{};
        uint8_t payload_length{};
        uint8_t payload[kCanPayloadMaxBytes]{};
        twai_message_t msg{};
    };

    uint32_t read_be_u32(const uint8_t *p);
    void write_be_u32(uint8_t *p, uint32_t v);

    uint32_t make_wired_id(const uint8_t frame_id_be[4], uint8_t channel_mask);

    uint8_t decode_channel_mask(uint32_t wired_id);
    uint8_t decode_channel_no(uint32_t wired_id);
    uint32_t decode_can_id(uint32_t wired_id);

    uint32_t decode_product_id_le(const uint8_t *payload, size_t payload_len);

    void can_id_to_bytes_be(uint32_t id29, uint8_t out4[4]);

    size_t encode_at_line(uint8_t *out, size_t out_cap, uint32_t ts_packed, uint32_t wired_id,
        const uint8_t *payload, uint8_t payload_len);

    size_t encode_at_from_twai(uint8_t *out, size_t out_cap, uint32_t ts_packed, const twai_message_t &msg,
        uint8_t channel_mask = kAtWiredIdMask);

    bool decode_at_packet(const uint8_t *packet, size_t packet_len, DecodedAtFrame *out);

    bool try_parse_one_at_message(const uint8_t *data, size_t len, size_t *consumed,
        uint32_t *ts_packed, uint32_t *wired_id, uint8_t *payload,
        uint8_t *dlc_max255, size_t payload_cap);

    bool wired_to_twai(uint32_t wired_id, uint8_t dlc, const uint8_t *payload, twai_message_t *msg);

    typedef void (*OnAtDecodedEx)(void *userdata, const DecodedAtFrame &frame);
    typedef void (*OnAtDecoded)(void *userdata, const twai_message_t &msg);

    /**
     * @brief 从连续字节流缓冲区中自同步提取所有完整 AT 包。
     *
     * 解析策略与 request-docs/13_at_packet_format.yaml 对齐：搜索 "AT"，按 LEN 计算总长，
     * 校验末尾 CRLF；若校验失败，丢弃一个字节后滑动重试。函数返回时，work[0..*io_len)
     * 保留未完成的半包或可能跨包的末尾 'A'。
     */
    void consume_at_buffer_ex(uint8_t *work, size_t *io_len, void *userdata, OnAtDecodedEx cb);

    /**
     * @brief 兼容旧调用方：只回调 twai_message_t，不暴露时间戳/通道信息。
     */
    void consume_at_buffer(uint8_t *work, size_t *io_len, void *userdata, OnAtDecoded cb);

} // namespace gateway::at_protocol
