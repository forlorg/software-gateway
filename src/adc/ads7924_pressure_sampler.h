#pragma once
/**
 * @file ads7924_pressure_sampler.h
 * @brief 基于 ADS7924 Manual-Scan 的四通道压力采样器。
 */
#include <array>
#include <cstdint>

#include <Wire.h>

#include "adc/ads7924.h"
#include "adc/pressure_can_frame.h"

namespace gateway::adc {

class Ads7924PressureSampler final {
 public:
  struct Config {
    int sda_pin = 11;
    int scl_pin = 12;
    int int_pin = 13;
    int reset_pin = 14;
    uint8_t address = Ads7924::kDefaultAddress;
    uint32_t i2c_clock_hz = 100000;
    uint8_t acquisition_time_code = 0;
    uint16_t i2c_timeout_ms = 50;
    uint8_t retry_count = 3;
    uint16_t retry_delay_ms = 1;
  };

  explicit Ads7924PressureSampler(TwoWire &wire);

  bool begin(const Config &config);
  bool prime();
  bool readPreviousAndStartNext(
      std::array<uint16_t, pressure_can::kChannelCount> &raw);
  bool recover();

  Ads7924::Error lastError() const { return adc_.lastError(); }
  const char *lastErrorName() const;
  uint8_t lastWireStatus() const { return adc_.lastWireStatus(); }
  uint8_t lastDeviceId() const { return last_device_id_; }
  bool deviceIdOk() const { return device_id_ok_; }

 private:
  static constexpr uint8_t kRegInterruptConfig = 0x12;
  static constexpr uint8_t kRegPowerConfig = 0x15;
  static constexpr uint8_t kIntConfigDisabled = 0x00;

  bool configureAfterReset();

  Ads7924 adc_;
  Config config_{};
  bool configured_ = false;
  bool initialized_ = false;
  bool scan_in_flight_ = false;
  uint8_t last_device_id_ = 0;
  bool device_id_ok_ = false;
};

} // namespace gateway::adc
