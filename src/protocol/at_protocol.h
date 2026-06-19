#pragma once

/**
 * @file at_protocol.h
 * @brief 网关 AT 报文格式常量与编解码函数声明。
 */

#include <Arduino.h>
#include <cstddef>
#include <cstdint>
#include <driver/twai.h>

namespace gateway::at_protocol {

    constexpr uint8_t kAtWiredIdMask = 0x05;

    constexpr size_t kAtLineMaxBytes = 2 + 4 + 4 + 1 + 256 + 2;

    uint32_t make_wired_id(const uint8_t frame_id_be[4], uint8_t channel_mask);

    uint32_t decode_product_id_le(const uint8_t *payload, size_t payload_len);

    void can_id_to_bytes_be(uint32_t id29, uint8_t out4[4]);

    size_t encode_at_line(uint8_t *out, size_t out_cap, uint32_t ts_packed, uint32_t wired_id,
        const uint8_t *payload, uint8_t dlc);

    size_t encode_at_from_twai(uint8_t *out, size_t out_cap, uint32_t ts_packed, const twai_message_t &msg);

    bool try_parse_one_at_message(const uint8_t *data, size_t len, size_t *consumed,
        uint32_t *ts_packed, uint32_t *wired_id, uint8_t *payload,
        uint8_t *dlc_max255, size_t payload_cap);

    bool wired_to_twai(uint32_t wired_id, uint8_t dlc, const uint8_t *payload, twai_message_t *msg);

    typedef void (*OnAtDecoded)(void *userdata, const twai_message_t &msg);

    void consume_at_buffer(uint8_t *work, size_t *io_len, void *userdata, OnAtDecoded cb);

} // namespace gateway::at_protocol
