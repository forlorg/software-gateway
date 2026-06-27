/**
 * @file at_protocol.cpp
 * @brief AT Packet 编码、解码与字节流自同步解析实现。
 */

#include "protocol/at_protocol.h"

#include <cstring>

namespace gateway::at_protocol {

    namespace {
        constexpr uint8_t kSync0 = 'A';
        constexpr uint8_t kSync1 = 'T';

        size_t find_sync(const uint8_t *data, size_t len) {
            if (!data || len < 2) {
                return len;
            }
            for (size_t i = 0; i + 1 < len; ++i) {
                if (data[i] == kSync0 && data[i + 1] == kSync1) {
                    return i;
                }
            }
            return len;
        }

        void preserve_possible_sync_tail(uint8_t *work, size_t *io_len) {
            if (!work || !io_len) {
                return;
            }
            const size_t len = *io_len;
            if (len > 0 && work[len - 1] == kSync0) {
                work[0] = kSync0;
                *io_len = 1;
            } else {
                *io_len = 0;
            }
        }
    } // namespace

    uint32_t read_be_u32(const uint8_t *p) {
        if (!p) {
            return 0;
        }
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    void write_be_u32(uint8_t *p, uint32_t v) {
        if (!p) {
            return;
        }
        p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[3] = static_cast<uint8_t>(v & 0xFF);
    }

    uint32_t make_wired_id(const uint8_t frame_id_be[4], uint8_t channel_mask) {
        if (!frame_id_be) {
            return 0;
        }
        const uint32_t base = read_be_u32(frame_id_be) & 0x1FFFFFFFu;
        return (base << 3) | static_cast<uint32_t>(channel_mask & 0x07u);
    }

    uint8_t decode_channel_mask(uint32_t wired_id) {
        return static_cast<uint8_t>(wired_id & 0x07u);
    }

    uint8_t decode_channel_no(uint32_t wired_id) {
        return decode_channel_mask(wired_id) == kAtChannelMaskCan1 ? 1 : 0;
    }

    uint32_t decode_can_id(uint32_t wired_id) {
        return (wired_id >> 3) & 0x1FFFFFFFu;
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
        if (!out4) {
            return;
        }
        id29 &= 0x1FFFFFFFu;
        write_be_u32(out4, id29);
    }

    size_t encode_at_line(uint8_t *out, size_t out_cap, uint32_t ts_packed, uint32_t wired_id,
        const uint8_t *payload, uint8_t payload_len) {
        const size_t need = kAtFixedHeaderBytes + static_cast<size_t>(payload_len) + kAtTailBytes;
        if (!out || out_cap < need || (payload_len > 0 && !payload)) {
            return 0;
        }

        size_t o = 0;
        out[o++] = kSync0;
        out[o++] = kSync1;
        write_be_u32(out + o, ts_packed);
        o += 4;
        write_be_u32(out + o, wired_id);
        o += 4;
        out[o++] = payload_len;
        if (payload_len > 0) {
            memcpy(out + o, payload, payload_len);
            o += payload_len;
        }
        out[o++] = '\r';
        out[o++] = '\n';
        return o;
    }

    size_t encode_at_from_twai(uint8_t *out, size_t out_cap, uint32_t ts_packed, const twai_message_t &msg,
        uint8_t channel_mask) {
        if (msg.data_length_code > kCanPayloadMaxBytes) {
            return 0;
        }

        uint32_t id = msg.identifier & (msg.extd ? 0x1FFFFFFFu : 0x7FFu);
        uint8_t idbe[4];
        if (msg.extd) {
            can_id_to_bytes_be(id, idbe);
        } else {
            idbe[0] = 0;
            idbe[1] = 0;
            idbe[2] = static_cast<uint8_t>((id >> 8) & 0xFF);
            idbe[3] = static_cast<uint8_t>(id & 0xFF);
        }
        const uint32_t wired = make_wired_id(idbe, channel_mask);
        return encode_at_line(out, out_cap, ts_packed, wired, msg.data, msg.data_length_code);
    }

    bool wired_to_twai(uint32_t wired_id, uint8_t dlc, const uint8_t *payload, twai_message_t *msg) {
        if (!msg || dlc > kCanPayloadMaxBytes || (dlc > 0 && !payload)) {
            return false;
        }
        memset(msg, 0, sizeof(*msg));
        msg->extd = 1;
        msg->identifier = decode_can_id(wired_id);
        msg->data_length_code = dlc;
        if (dlc > 0) {
            memcpy(msg->data, payload, dlc);
        }
        msg->rtr = 0;
        return true;
    }

    bool decode_at_packet(const uint8_t *packet, size_t packet_len, DecodedAtFrame *out) {
        if (!packet || !out || packet_len < kAtFixedHeaderBytes + kAtTailBytes) {
            return false;
        }
        if (packet[0] != kSync0 || packet[1] != kSync1) {
            return false;
        }

        const uint8_t payload_len = packet[10];
        const size_t total_len = kAtFixedHeaderBytes + static_cast<size_t>(payload_len) + kAtTailBytes;
        if (packet_len < total_len) {
            return false;
        }
        if (packet[total_len - 2] != '\r' || packet[total_len - 1] != '\n') {
            return false;
        }
        if (payload_len > kCanPayloadMaxBytes) {
            return false;
        }

        DecodedAtFrame frame{};
        frame.ts_packed = read_be_u32(packet + 2);
        frame.wired_id = read_be_u32(packet + 6);
        frame.channel_mask = decode_channel_mask(frame.wired_id);
        frame.channel_no = decode_channel_no(frame.wired_id);
        frame.can_id = decode_can_id(frame.wired_id);
        frame.payload_length = payload_len;
        if (payload_len > 0) {
            memcpy(frame.payload, packet + kAtFixedHeaderBytes, payload_len);
        }
        if (!wired_to_twai(frame.wired_id, payload_len, frame.payload, &frame.msg)) {
            return false;
        }
        *out = frame;
        return true;
    }

    bool try_parse_one_at_message(const uint8_t *data, size_t len, size_t *consumed,
        uint32_t *ts_packed, uint32_t *wired_id, uint8_t *payload,
        uint8_t *dlc_max255, size_t payload_cap) {
        if (!data || !consumed || !ts_packed || !wired_id || !payload || !dlc_max255) {
            return false;
        }
        if (len < kAtFixedHeaderBytes + kAtTailBytes || data[0] != kSync0 || data[1] != kSync1) {
            return false;
        }
        const uint8_t payload_len = data[10];
        const size_t total_len = kAtFixedHeaderBytes + static_cast<size_t>(payload_len) + kAtTailBytes;
        if (len < total_len || static_cast<size_t>(payload_len) > payload_cap || payload_len > kCanPayloadMaxBytes) {
            return false;
        }
        if (data[total_len - 2] != '\r' || data[total_len - 1] != '\n') {
            return false;
        }

        *ts_packed = read_be_u32(data + 2);
        *wired_id = read_be_u32(data + 6);
        if (payload_len > 0) {
            memcpy(payload, data + kAtFixedHeaderBytes, payload_len);
        }
        *dlc_max255 = payload_len;
        *consumed = total_len;
        return true;
    }

    void consume_at_buffer_ex(uint8_t *work, size_t *io_len, void *userdata, OnAtDecodedEx cb) {
        if (!work || !io_len || !cb) {
            return;
        }

        size_t len = *io_len;
        for (;;) {
            if (len < 2) {
                if (len == 1 && work[0] != kSync0) {
                    len = 0;
                }
                break;
            }

            const size_t sync = find_sync(work, len);
            if (sync == len) {
                *io_len = len;
                preserve_possible_sync_tail(work, io_len);
                return;
            }
            if (sync > 0) {
                memmove(work, work + sync, len - sync);
                len -= sync;
            }

            if (len < kAtFixedHeaderBytes) {
                break;
            }

            const uint8_t payload_len = work[10];
            const size_t total_len = kAtFixedHeaderBytes + static_cast<size_t>(payload_len) + kAtTailBytes;
            if (len < total_len) {
                break;
            }

            if (work[total_len - 2] != '\r' || work[total_len - 1] != '\n') {
                // 误同步：只滑动 1 字节，允许 payload 内部的 "AT" 继续参与下一轮重同步。
                memmove(work, work + 1, len - 1);
                --len;
                continue;
            }

            DecodedAtFrame frame{};
            if (decode_at_packet(work, total_len, &frame)) {
                cb(userdata, frame);
            }

            if (len > total_len) {
                memmove(work, work + total_len, len - total_len);
            }
            len -= total_len;
        }

        *io_len = len;
    }

    namespace {
        struct LegacyCallbackContext {
            void *userdata{};
            OnAtDecoded cb{};
        };

        void legacy_bridge(void *userdata, const DecodedAtFrame &frame) {
            auto *ctx = static_cast<LegacyCallbackContext *>(userdata);
            if (!ctx || !ctx->cb) {
                return;
            }
            ctx->cb(ctx->userdata, frame.msg);
        }
    } // namespace

    void consume_at_buffer(uint8_t *work, size_t *io_len, void *userdata, OnAtDecoded cb) {
        LegacyCallbackContext ctx{userdata, cb};
        consume_at_buffer_ex(work, io_len, &ctx, legacy_bridge);
    }

} // namespace gateway::at_protocol
