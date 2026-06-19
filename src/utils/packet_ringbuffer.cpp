/**
 * @file packet_ringbuffer.cpp
 * @brief 定长字节环形容器：uint16 小端长度前缀 + 负载，互斥保护 push/pop。
 */

#include "packet_ringbuffer.h"
#include <cstring>

namespace gateway {

    PacketRingBuffer::PacketRingBuffer(uint8_t *storage, size_t byte_capacity, SemaphoreHandle_t mu)
    : buf_(storage), cap_(byte_capacity), mu_(mu) {}

    size_t PacketRingBuffer::bytes_used() const { return used_; }

    bool PacketRingBuffer::drop_oldest_packet_() {
        if (used_ < 2) {
            return false;
        }
        uint16_t len = static_cast<uint16_t>(buf_[head_]) | (static_cast<uint16_t>(buf_[(head_ + 1) % cap_]) << 8);
        size_t block = 2 + len;
        if (used_ < block) {
            used_ = 0;
            head_ = tail_ = 0;
            return false;
        }
        head_ = (head_ + block) % cap_;
        used_ -= block;
        ++dropped_;
        return true;
    }

    bool PacketRingBuffer::push(const uint8_t *data, uint16_t len) {
        if (!data || len == 0) {
            return false;
        }
        const size_t need = 2 + len;
        if (need > cap_) {
            return false;
        }
        if (xSemaphoreTake(mu_, pdMS_TO_TICKS(40)) != pdTRUE) {
            return false;
        }

        while (used_ + need > cap_) {
            if (!drop_oldest_packet_()) {
                ++dropped_;
                xSemaphoreGive(mu_);
                return false;
            }
        }

        auto write_byte = [&](size_t idx, uint8_t b) { buf_[idx % cap_] = b; };

        write_byte(tail_, static_cast<uint8_t>(len & 0xFF));
        tail_ = (tail_ + 1) % cap_;
        ++used_;

        write_byte(tail_, static_cast<uint8_t>((len >> 8) & 0xFF));
        tail_ = (tail_ + 1) % cap_;
        ++used_;

        for (uint16_t i = 0; i < len; ++i) {
            write_byte(tail_, data[i]);
            tail_ = (tail_ + 1) % cap_;
            ++used_;
        }

        xSemaphoreGive(mu_);
        return true;
    }

    uint16_t PacketRingBuffer::pop(uint8_t *dst, uint16_t dst_cap) {
        if (!dst || dst_cap == 0) {
            return 0;
        }
        if (xSemaphoreTake(mu_, pdMS_TO_TICKS(40)) != pdTRUE) {
            return 0;
        }

        if (used_ < 2) {
            xSemaphoreGive(mu_);
            return 0;
        }

        uint16_t len = static_cast<uint16_t>(buf_[head_]) | (static_cast<uint16_t>(buf_[(head_ + 1) % cap_]) << 8);
        if (2 + len > used_ || len > dst_cap) {
            xSemaphoreGive(mu_);
            return 0;
        }

        head_ = (head_ + 2) % cap_;
        used_ -= 2;

        for (uint16_t i = 0; i < len; ++i) {
            dst[i] = buf_[head_];
            head_ = (head_ + 1) % cap_;
            --used_;
        }

        xSemaphoreGive(mu_);
        return len;
    }

} // namespace gateway
