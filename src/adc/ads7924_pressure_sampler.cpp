/**
 * @file ads7924_pressure_sampler.cpp
 * @brief 基于 ADS7924 Manual-Scan 的四通道压力采样器实现。
 */
#include "adc/ads7924_pressure_sampler.h"

#include <Arduino.h>

#include "driver/gpio.h"

namespace gateway::adc {

Ads7924PressureSampler::Ads7924PressureSampler(TwoWire &wire) : adc_(wire) {}

bool Ads7924PressureSampler::begin(const Config &config) {
  config_ = config;
  configured_ = true;
  initialized_ = false;
  scan_in_flight_ = false;
  last_device_id_ = 0;
  device_id_ok_ = false;

  Ads7924::Config driver_config;
  driver_config.sda_pin = config_.sda_pin;
  driver_config.scl_pin = config_.scl_pin;
  driver_config.int_pin = config_.int_pin;
  driver_config.reset_pin = config_.reset_pin;
  driver_config.address = config_.address;
  driver_config.clock_hz = config_.i2c_clock_hz;
  driver_config.i2c_timeout_ms = config_.i2c_timeout_ms;
  driver_config.retry_count = config_.retry_count;
  driver_config.retry_delay_ms = config_.retry_delay_ms;

  if (!adc_.begin(driver_config)) {
    return false;
  }

  gpio_pullup_dis(static_cast<gpio_num_t>(config_.int_pin));
  gpio_pulldown_dis(static_cast<gpio_num_t>(config_.int_pin));

  if (!configureAfterReset()) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool Ads7924PressureSampler::configureAfterReset() {
  scan_in_flight_ = false;
  device_id_ok_ = false;

  if (!adc_.probe()) {
    return false;
  }

  if (!adc_.readDeviceId(last_device_id_)) {
    return false;
  }

  device_id_ok_ = last_device_id_ == Ads7924::kExpectedDeviceId;
  if (!device_id_ok_) {
    return false;
  }

  // 当前硬件不依赖 GPIO14 硬件复位，使用软件复位让寄存器回到确定状态。
  if (!adc_.softwareReset()) {
    return false;
  }

  // 直连 MUXOUT/ADCIN，不需要外部放大器上电等待。
  if (!adc_.writeRegisterVerified(kRegPowerConfig, 0x00, 0xFF)) {
    return false;
  }

  if (!adc_.setIdle()) {
    return false;
  }

  if (!adc_.configureAcquisitionTime(config_.acquisition_time_code)) {
    return false;
  }

  // INT 引脚测试已确认不可靠，正式任务不依赖 data-ready INT。
  if (!adc_.writeRegister(kRegInterruptConfig, kIntConfigDisabled)) {
    return false;
  }

  std::array<uint16_t, pressure_can::kChannelCount> discard{};
  if (!adc_.readAllChannels(discard)) {
    return false;
  }

  return true;
}

bool Ads7924PressureSampler::prime() {
  if (!initialized_) {
    return false;
  }

  std::array<uint16_t, pressure_can::kChannelCount> discard{};
  if (!adc_.readAllChannels(discard)) {
    scan_in_flight_ = false;
    return false;
  }

  if (!adc_.startManualScan(0)) {
    scan_in_flight_ = false;
    return false;
  }

  scan_in_flight_ = true;
  return true;
}

bool Ads7924PressureSampler::readPreviousAndStartNext(
    std::array<uint16_t, pressure_can::kChannelCount> &raw) {
  if (!initialized_) {
    return false;
  }

  if (!scan_in_flight_) {
    if (!adc_.startManualScan(0)) {
      return false;
    }
    scan_in_flight_ = true;
    return false;
  }

  if (!adc_.readAllChannels(raw)) {
    scan_in_flight_ = false;
    adc_.setIdle();
    return false;
  }

  if (!adc_.startManualScan(0)) {
    scan_in_flight_ = false;
    adc_.setIdle();
    return false;
  }

  scan_in_flight_ = true;
  return true;
}

bool Ads7924PressureSampler::recover() {
  if (!configured_) {
    return false;
  }

  initialized_ = false;
  scan_in_flight_ = false;

  if (!adc_.recoverBus()) {
    return false;
  }

  if (!configureAfterReset()) {
    return false;
  }

  initialized_ = true;
  return true;
}

const char *Ads7924PressureSampler::lastErrorName() const {
  return Ads7924::errorToString(adc_.lastError());
}

} // namespace gateway::adc
