#pragma once

/**
 * @file packet_ringbuffer.h
 * @brief 线程安全的变长包环形队列（FreeRTOS 互斥量由调用方传入）。
 */

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace gateway {

/** 长度前缀环形缓冲：每包为 uint16 小端长度 + 负载；空间不足时丢弃最旧完整包。 */
class PacketRingBuffer {
public:
  PacketRingBuffer(uint8_t *storage, size_t byte_capacity, SemaphoreHandle_t mu);

  size_t bytes_used() const;
  uint32_t dropped_packets() const { return dropped_; }

  bool push(const uint8_t *data, uint16_t len);
  uint16_t pop(uint8_t *dst, uint16_t dst_cap);

private:
  bool drop_oldest_packet_();

  uint8_t *buf_;
  size_t cap_;
  size_t head_{0};
  size_t tail_{0};
  size_t used_{0};
  uint32_t dropped_{0};
  SemaphoreHandle_t mu_;
};

} // namespace gateway
