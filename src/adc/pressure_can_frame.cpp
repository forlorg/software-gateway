/**
 * @file pressure_can_frame.cpp
 * @brief ADS7924 四通道压力数据滤波、换算与 PGN 0x1708 CAN 帧打包实现。
 */
#include "adc/pressure_can_frame.h"

#include <algorithm>

namespace gateway::adc::pressure_can {

void BlockAverageFilter::reset() {
  sums_.fill(0);
  count_ = 0;
}

bool BlockAverageFilter::addRawGroup(
    const std::array<uint16_t, kChannelCount> &raw) {
  if (count_ >= kFilterSampleCount) {
    return false;
  }

  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    sums_[channel] += raw[channel];
  }
  ++count_;
  return true;
}

std::array<uint16_t, kChannelCount> BlockAverageFilter::consumeAveragedRaw() {
  std::array<uint16_t, kChannelCount> averaged{};

  if (count_ == 0) {
    return averaged;
  }

  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    averaged[channel] =
        static_cast<uint16_t>((sums_[channel] + count_ / 2U) / count_);
  }

  reset();
  return averaged;
}

uint16_t rawToMillivolts(uint16_t raw, uint32_t reference_mv) {
  const uint16_t limited_raw = std::min<uint16_t>(raw, 4095U);
  const uint64_t scaled =
      static_cast<uint64_t>(limited_raw) * static_cast<uint64_t>(reference_mv);
  return static_cast<uint16_t>((scaled + 2047ULL) / 4095ULL);
}

uint16_t millivoltsToCanRaw(uint32_t millivolts) {
  return static_cast<uint16_t>(
      std::min<uint32_t>(millivolts, static_cast<uint32_t>(kCanRawMax)));
}

int16_t canRawToPressureKpa(uint16_t can_raw) {
  return static_cast<int16_t>(static_cast<int32_t>(can_raw) - 500);
}

PressureFrameData convertAveragedRaw(
    const std::array<uint16_t, kChannelCount> &averaged_raw) {
  PressureFrameData data;
  data.averaged_raw = averaged_raw;

  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    data.millivolts[channel] = rawToMillivolts(averaged_raw[channel]);
    data.can_raw[channel] = millivoltsToCanRaw(data.millivolts[channel]);
    data.pressure_kpa[channel] = canRawToPressureKpa(data.can_raw[channel]);
  }

  return data;
}

twai_message_t buildPgn1708Frame(const PressureFrameData &data) {
  twai_message_t msg{};
  msg.extd = true;
  msg.rtr = false;
  msg.identifier = kCanIdPgn1708;
  msg.data_length_code = 8;

  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const uint16_t value = data.can_raw[channel];
    msg.data[channel * 2U] = static_cast<uint8_t>(value & 0xFFU);
    msg.data[channel * 2U + 1U] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  }

  return msg;
}

} // namespace gateway::adc::pressure_can
