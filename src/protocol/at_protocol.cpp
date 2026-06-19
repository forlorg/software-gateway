/**
 * @file at_protocol.cpp
 * @brief AT 二进制行编解码：时间戳打包、CAN ID、下行缓冲消费与 TWAI 互转。
 */

#include "at_protocol.h"

namespace gateway::at_protocol {

    uint32_t make_wired_id(const uint8_t frame_id_be[4], uint8_t channel_mask) {
        uint32_t base = (static_cast<uint32_t>(frame_id_be[0]) << 24) |
        (static_cast<uint32_t>(frame_id_be[1]) << 16) |
        (static_cast<uint32_t>(frame_id_be[2]) << 8) |
        (static_cast<uint32_t>(frame_id_be[3]));
        return (base << 3) | static_cast<uint32_t>(channel_mask);
    }

    uint32_t decode_product_id_le(const uint8_t *payload, size_t payload_len) {
        const size_t need = 4 + 4;
        if (!payload || payload_len < need) {
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(payload[4 + i]) << (i * 8);
        }
        return v;
    }

    void can_id_to_bytes_be(uint32_t id29, uint8_t out4[4]) {
        id29 &= 0x1FFFFFFFu;
        out4[0] = static_cast<uint8_t>((id29 >> 24) & 0xFF);
        out4[1] = static_cast<uint8_t>((id29 >> 16) & 0xFF);
        out4[2] = static_cast<uint8_t>((id29 >> 8) & 0xFF);
        out4[3] = static_cast<uint8_t>(id29 & 0xFF);
    }

    size_t encode_at_line(uint8_t *out, size_t out_cap, uint32_t ts_packed, uint32_t wired_id,
        const uint8_t *payload, uint8_t dlc) {
        const size_t need = 2 + 4 + 4 + 1 + dlc + 2;
        if (!out || out_cap < need) {
            return 0;
        }
        size_t o = 0;
        out[o++] = 'A';
        out[o++] = 'T';
        out[o++] = static_cast<uint8_t>((ts_packed >> 24) & 0xFF);
        out[o++] = static_cast<uint8_t>((ts_packed >> 16) & 0xFF);
        out[o++] = static_cast<uint8_t>((ts_packed >> 8) & 0xFF);
        out[o++] = static_cast<uint8_t>(ts_packed & 0xFF);
        out[o++] = static_cast<uint8_t>((wired_id >> 24) & 0xFF);
        out[o++] = static_cast<uint8_t>((wired_id >> 16) & 0xFF);
        out[o++] = static_cast<uint8_t>((wired_id >> 8) & 0xFF);
        out[o++] = static_cast<uint8_t>(wired_id & 0xFF);
        out[o++] = dlc;
        for (uint8_t i = 0; i < dlc; ++i) {
            out[o++] = payload[i];
        }
        out[o++] = '\r';
        out[o++] = '\n';
        return o;
    }

    size_t encode_at_from_twai(uint8_t *out, size_t out_cap, uint32_t ts_packed, const twai_message_t &msg) {
        uint32_t id29 = msg.identifier & (msg.extd ? 0x1FFFFFFFu : 0x7FFu);
        uint8_t idbe[4];
        if (msg.extd) {
            can_id_to_bytes_be(id29, idbe);
        } else {
            idbe[0] = idbe[1] = idbe[2] = 0;
            idbe[3] = static_cast<uint8_t>(id29 & 0xFF);
        }
        uint32_t wired = make_wired_id(idbe, kAtWiredIdMask);
        return encode_at_line(out, out_cap, ts_packed, wired, msg.data, msg.data_length_code);
    }

    bool try_parse_one_at_message(const uint8_t *data, size_t len, size_t *consumed,
        uint32_t *ts_packed, uint32_t *wired_id, uint8_t *payload,
        uint8_t *dlc_max255, size_t payload_cap) {
        if (!data || len < static_cast<size_t>(2 + 4 + 4 + 1 + 2) || !consumed || !ts_packed ||
            !wired_id || !payload || !dlc_max255) {
            return false;
        }
        if (data[0] != 'A' || data[1] != 'T') {
            return false;
        }
        size_t idx = 2;
        auto need = [&](size_t n) -> bool { return idx + n <= len; };

        if (!need(4 + 4 + 1)) {
            return false;
        }

        *ts_packed =
        (static_cast<uint32_t>(data[idx]) << 24) | (static_cast<uint32_t>(data[idx + 1]) << 16) |
        (static_cast<uint32_t>(data[idx + 2]) << 8) | static_cast<uint32_t>(data[idx + 3]);
        idx += 4;

        *wired_id = (static_cast<uint32_t>(data[idx]) << 24) | (static_cast<uint32_t>(data[idx + 1]) << 16) |
        (static_cast<uint32_t>(data[idx + 2]) << 8) | static_cast<uint32_t>(data[idx + 3]);
        idx += 4;

        uint8_t dlc = data[idx++];
        /** payload_cap 常为 256；若强转为 uint8_t 会溢出为 0，导致任意 dlc≥1 均被拒。 */
        if (dlc > 8 || static_cast<size_t>(dlc) > payload_cap) {
            return false;
        }
        if (!need(dlc + 2)) {
            return false;
        }
        memcpy(payload, data + idx, dlc);
        idx += dlc;
        if (data[idx] != '\r' || data[idx + 1] != '\n') {
            return false;
        }
        idx += 2;
        *dlc_max255 = dlc;
        *consumed = idx;
        return true;
    }

    bool wired_to_twai(uint32_t wired_id, uint8_t dlc, const uint8_t *payload, twai_message_t *msg) {
        if (!msg || !payload || dlc > 8) {
            return false;
        }
        memset(msg, 0, sizeof(*msg));
        uint32_t base = (wired_id >> 3) & 0x1FFFFFFFu;
        msg->extd = 1;
        msg->identifier = base;
        msg->data_length_code = dlc;
        memcpy(msg->data, payload, dlc);
        msg->rtr = 0;
        return true;
    }

    void consume_at_buffer(uint8_t *work, size_t *io_len, void *userdata, OnAtDecoded cb) {
        if (!work || !io_len || !cb) {
            return;
        }
        size_t len = *io_len;
        size_t consumed_total = 0;
        while (len >= 13) {
            size_t pkt = 0;
            uint32_t tsp, wired;
            uint8_t payload[256];
            uint8_t dlc = 0;
            if (!try_parse_one_at_message(work + consumed_total, len, &pkt, &tsp, &wired, payload, &dlc,
                sizeof(payload))) {
                break;
            }
            twai_message_t tm{};
            if (wired_to_twai(wired, dlc, payload, &tm)) {
                cb(userdata, tm);
            }
            consumed_total += pkt;
            len -= pkt;
        }
        memmove(work, work + consumed_total, len);
        *io_len = len;
    }

} // namespace gateway::at_protocol
