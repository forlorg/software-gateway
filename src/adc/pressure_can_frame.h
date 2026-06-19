#pragma once
/**
 * @file pressure_can_frame.h
 * @brief ADS7924 四通道压力数据滤波、换算与 PGN 0x1708 CAN 帧打包。
 */
#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/twai.h"

namespace gateway::adc::pressure_can {

    constexpr std::size_t kChannelCount = 4;
    constexpr uint32_t kAdcReferenceMv = 5000;
    constexpr uint16_t kCanRawMax = 5000;
    constexpr uint32_t kCanIdPgn1708 = 0x16170803u;

    constexpr uint32_t kSampleGroupPeriodMs = 5;
    constexpr uint32_t kCanFramePeriodMs = 20;
    constexpr std::size_t kFilterSampleCount = 4;

    struct PressureFrameData {
        std::array<uint16_t, kChannelCount> averaged_raw{};
        std::array<uint16_t, kChannelCount> millivolts{};
        std::array<uint16_t, kChannelCount> can_raw{};
        std::array<int16_t, kChannelCount> pressure_kpa{};
    };

    /**
     * @brief 4 点块平均滤波器。
     *
     * 每 5 ms 加入一组 ADS7924 四通道数据，累计 4 组后输出一次平均结果，
     * 与 20 ms / 50 Hz 的 PGN 0x1708 发送周期严格对齐。
     */
    class BlockAverageFilter final {
    public:
        void reset();
        bool addRawGroup(const std::array<uint16_t, kChannelCount> &raw);
        bool ready() const { return count_ >= kFilterSampleCount; }
        std::array<uint16_t, kChannelCount> consumeAveragedRaw();
        std::size_t count() const { return count_; }

    private:
        std::array<uint32_t, kChannelCount> sums_{};
        std::size_t count_ = 0;
    };

    uint16_t rawToMillivolts(uint16_t raw,
        uint32_t reference_mv = kAdcReferenceMv);
    uint16_t millivoltsToCanRaw(uint32_t millivolts);
    int16_t canRawToPressureKpa(uint16_t can_raw);

    PressureFrameData convertAveragedRaw(
        const std::array<uint16_t, kChannelCount> &averaged_raw);

    twai_message_t buildPgn1708Frame(const PressureFrameData &data);

} // namespace gateway::adc::pressure_can
