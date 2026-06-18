/**
 * @file ads7924.cpp
 * @brief ADS7924 I2C 驱动实现。
 */

#include "adc/ads7924.h"

#include <algorithm>

namespace gateway::adc {

Ads7924::Ads7924(TwoWire &wire) : wire_(wire) {}

Ads7924::~Ads7924() {
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

bool Ads7924::lock() {
  if (mutex_ == nullptr) {
    setError(Error::kMutexCreateFailed);
    return false;
  }

  const TickType_t wait_ticks = pdMS_TO_TICKS(config_.mutex_timeout_ms);
  if (xSemaphoreTake(mutex_, wait_ticks) != pdTRUE) {
    setError(Error::kMutexTimeout);
    return false;
  }

  return true;
}

void Ads7924::unlock() {
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}

bool Ads7924::begin(const Config &config) {
  if (config.sda_pin < 0 || config.scl_pin < 0 || config.reset_pin < 0 ||
      config.int_pin < 0 || config.address > 0x7F || config.clock_hz == 0 ||
      config.retry_count == 0) {
    setError(Error::kInvalidArgument);
    return false;
  }

  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      setError(Error::kMutexCreateFailed);
      return false;
    }
  }

  config_ = config;
  config_valid_ = true;

  if (!lock()) {
    return false;
  }

  // 先写输出锁存器再切换为输出，避免 RESET 出现不必要的低脉冲。
  digitalWrite(config_.reset_pin, HIGH);
  pinMode(config_.reset_pin, OUTPUT);
  pinMode(config_.int_pin, INPUT);  // ADS7924 INT 为推挽输出，不启用内部上下拉。

  if (wire_started_) {
    wire_.end();
    wire_started_ = false;
    initialized_ = false;
  }

  if (!recoverBusUnlocked()) {
    unlock();
    return false;
  }

  const bool started = wire_.begin(config_.sda_pin, config_.scl_pin, config_.clock_hz);
  if (!started) {
    setError(Error::kWireBeginFailed);
    unlock();
    return false;
  }

  wire_.setTimeOut(config_.i2c_timeout_ms);
  wire_started_ = true;
  initialized_ = true;
  last_wire_status_ = 0;
  setError(Error::kNone);

  unlock();
  return true;
}

bool Ads7924::recoverBus() {
  if (!config_valid_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  if (wire_started_) {
    wire_.end();
    wire_started_ = false;
    initialized_ = false;
  }

  const bool released = recoverBusUnlocked();
  const bool restarted = released && restartWireUnlocked();

  unlock();
  return restarted;
}

bool Ads7924::restartWireUnlocked() {
  if (!wire_.begin(config_.sda_pin, config_.scl_pin, config_.clock_hz)) {
    setError(Error::kWireBeginFailed);
    return false;
  }

  wire_.setTimeOut(config_.i2c_timeout_ms);
  wire_started_ = true;
  initialized_ = true;
  last_wire_status_ = 0;
  setError(Error::kNone);
  return true;
}

bool Ads7924::waitPinHigh(int pin, uint32_t timeout_us) const {
  const uint32_t started_us = micros();
  do {
    if (digitalRead(pin) == HIGH) {
      return true;
    }
    delayMicroseconds(2);
  } while (static_cast<uint32_t>(micros() - started_us) < timeout_us);

  return digitalRead(pin) == HIGH;
}

bool Ads7924::recoverBusUnlocked() {
  // OUTPUT_OPEN_DRAIN 写 HIGH 表示释放，由板上的 2.2 kΩ 外部电阻拉高。
  digitalWrite(config_.sda_pin, HIGH);
  digitalWrite(config_.scl_pin, HIGH);
  pinMode(config_.sda_pin, OUTPUT_OPEN_DRAIN);
  pinMode(config_.scl_pin, OUTPUT_OPEN_DRAIN);

  const auto release_pins = [this]() {
    digitalWrite(config_.sda_pin, HIGH);
    digitalWrite(config_.scl_pin, HIGH);
    pinMode(config_.sda_pin, INPUT);
    pinMode(config_.scl_pin, INPUT);
  };

  if (!waitPinHigh(config_.scl_pin, config_.bus_release_timeout_us)) {
    release_pins();
    setError(Error::kBusStuck);
    return false;
  }

  if (digitalRead(config_.sda_pin) == LOW) {
    // I2C 规范中的常用恢复方式：最多提供 9 个 SCL 脉冲，让从设备退出未完成字节。
    for (uint8_t pulse = 0; pulse < 9 && digitalRead(config_.sda_pin) == LOW; ++pulse) {
      digitalWrite(config_.scl_pin, LOW);
      delayMicroseconds(5);
      digitalWrite(config_.scl_pin, HIGH);

      if (!waitPinHigh(config_.scl_pin, config_.bus_release_timeout_us)) {
        release_pins();
        setError(Error::kBusStuck);
        return false;
      }
      delayMicroseconds(5);
    }

    // 生成 STOP：SCL 为高时，SDA 从低变高。
    digitalWrite(config_.sda_pin, LOW);
    delayMicroseconds(5);
    digitalWrite(config_.scl_pin, HIGH);

    if (!waitPinHigh(config_.scl_pin, config_.bus_release_timeout_us)) {
      release_pins();
      setError(Error::kBusStuck);
      return false;
    }

    delayMicroseconds(5);
    digitalWrite(config_.sda_pin, HIGH);
    delayMicroseconds(5);
  }

  const bool bus_free =
      digitalRead(config_.sda_pin) == HIGH && digitalRead(config_.scl_pin) == HIGH;
  release_pins();

  if (!bus_free) {
    setError(Error::kBusStuck);
    return false;
  }

  setError(Error::kNone);
  return true;
}

bool Ads7924::validateRegisterAccess(uint8_t first_reg, std::size_t length) const {
  if (length == 0 || length > 32) {
    return false;
  }

  if ((first_reg & static_cast<uint8_t>(~kPointerAddressMask)) != 0) {
    return false;
  }

  const std::size_t last_reg = static_cast<std::size_t>(first_reg) + length - 1;
  return last_reg <= kRegResetAndId;
}

void Ads7924::setWireError(uint8_t status) {
  last_wire_status_ = status;

  switch (status) {
    case 0:
      setError(Error::kNone);
      break;
    case 1:
      setError(Error::kWireBufferOverflow);
      break;
    case 2:
      setError(Error::kAddressNack);
      break;
    case 3:
      setError(Error::kDataNack);
      break;
    case 5:
      setError(Error::kWireTimeout);
      break;
    default:
      setError(Error::kWireOther);
      break;
  }
}

bool Ads7924::probeUnlocked() {
  for (uint8_t attempt = 0; attempt < config_.retry_count; ++attempt) {
    wire_.beginTransmission(config_.address);
    const uint8_t status = wire_.endTransmission(true);
    setWireError(status);

    if (status == 0) {
      return true;
    }

    if (attempt + 1 < config_.retry_count) {
      vTaskDelay(pdMS_TO_TICKS(config_.retry_delay_ms));
    }
  }

  return false;
}

bool Ads7924::probe() {
  if (!initialized_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  const bool ok = probeUnlocked();
  unlock();
  return ok;
}

bool Ads7924::hardwareReset() {
  if (!config_valid_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  digitalWrite(config_.reset_pin, LOW);
  delayMicroseconds(config_.reset_low_us);
  digitalWrite(config_.reset_pin, HIGH);
  vTaskDelay(pdMS_TO_TICKS(config_.reset_recovery_ms));

  setError(Error::kNone);
  unlock();
  return true;
}

bool Ads7924::softwareReset() {
  if (!initialized_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  const uint8_t value = kSoftwareResetValue;
  const bool ok = writeRegistersUnlocked(kRegResetAndId, &value, 1);
  if (ok) {
    vTaskDelay(pdMS_TO_TICKS(config_.reset_recovery_ms));
  }

  unlock();
  return ok;
}

bool Ads7924::readRegistersUnlocked(uint8_t first_reg, uint8_t *data,
                                    std::size_t length) {
  if (data == nullptr || !validateRegisterAccess(first_reg, length)) {
    setError(Error::kInvalidArgument);
    return false;
  }

  const uint8_t pointer =
      static_cast<uint8_t>((first_reg & kPointerAddressMask) |
                           (length > 1 ? kPointerIncrement : 0));

  for (uint8_t attempt = 0; attempt < config_.retry_count; ++attempt) {
    while (wire_.available() > 0) {
      wire_.read();
    }

    wire_.beginTransmission(config_.address);
    if (wire_.write(pointer) != 1) {
      wire_.endTransmission(true);
      setError(Error::kWireBufferOverflow);
    } else {
      // ADS7924 允许写指针后使用 STOP，再开始读事务；这比依赖重复 START 更易恢复。
      const uint8_t pointer_status = wire_.endTransmission(true);
      setWireError(pointer_status);

      if (pointer_status == 0) {
        const uint8_t requested = static_cast<uint8_t>(length);
        const uint8_t received =
            wire_.requestFrom(static_cast<uint16_t>(config_.address), requested, true);

        if (received == requested) {
          bool complete = true;
          for (std::size_t index = 0; index < length; ++index) {
            if (wire_.available() <= 0) {
              complete = false;
              break;
            }

            const int value = wire_.read();
            if (value < 0) {
              complete = false;
              break;
            }
            data[index] = static_cast<uint8_t>(value);
          }

          while (wire_.available() > 0) {
            wire_.read();
          }

          if (complete) {
            last_wire_status_ = 0;
            setError(Error::kNone);
            return true;
          }
        }

        while (wire_.available() > 0) {
          wire_.read();
        }
        setError(Error::kShortRead);
      }
    }

    if (attempt + 1 < config_.retry_count) {
      vTaskDelay(pdMS_TO_TICKS(config_.retry_delay_ms));
    }
  }

  return false;
}

bool Ads7924::readRegisters(uint8_t first_reg, uint8_t *data, std::size_t length) {
  if (!initialized_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  const bool ok = readRegistersUnlocked(first_reg, data, length);
  unlock();
  return ok;
}

bool Ads7924::readRegister(uint8_t reg, uint8_t &value) {
  return readRegisters(reg, &value, 1);
}

bool Ads7924::writeRegistersUnlocked(uint8_t first_reg, const uint8_t *data,
                                     std::size_t length) {
  if (data == nullptr || !validateRegisterAccess(first_reg, length)) {
    setError(Error::kInvalidArgument);
    return false;
  }

  const uint8_t pointer =
      static_cast<uint8_t>((first_reg & kPointerAddressMask) |
                           (length > 1 ? kPointerIncrement : 0));

  for (uint8_t attempt = 0; attempt < config_.retry_count; ++attempt) {
    wire_.beginTransmission(config_.address);

    const bool pointer_queued = wire_.write(pointer) == 1;
    const bool data_queued = pointer_queued && wire_.write(data, length) == length;

    if (!data_queued) {
      wire_.endTransmission(true);
      setError(Error::kWireBufferOverflow);
    } else {
      const uint8_t status = wire_.endTransmission(true);
      setWireError(status);

      if (status == 0) {
        return true;
      }
    }

    if (attempt + 1 < config_.retry_count) {
      vTaskDelay(pdMS_TO_TICKS(config_.retry_delay_ms));
    }
  }

  return false;
}

bool Ads7924::writeRegisters(uint8_t first_reg, const uint8_t *data,
                             std::size_t length) {
  if (!initialized_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  const bool ok = writeRegistersUnlocked(first_reg, data, length);
  unlock();
  return ok;
}

bool Ads7924::writeRegister(uint8_t reg, uint8_t value) {
  return writeRegisters(reg, &value, 1);
}

bool Ads7924::writeRegisterVerifiedUnlocked(uint8_t reg, uint8_t value,
                                            uint8_t verify_mask) {
  if (!writeRegistersUnlocked(reg, &value, 1)) {
    return false;
  }

  uint8_t actual = 0;
  if (!readRegistersUnlocked(reg, &actual, 1)) {
    return false;
  }

  if ((actual & verify_mask) != (value & verify_mask)) {
    setError(Error::kVerificationFailed);
    return false;
  }

  setError(Error::kNone);
  return true;
}

bool Ads7924::writeRegisterVerified(uint8_t reg, uint8_t value,
                                    uint8_t verify_mask) {
  if (!initialized_) {
    setError(Error::kNotInitialized);
    return false;
  }

  if (!lock()) {
    return false;
  }

  const bool ok = writeRegisterVerifiedUnlocked(reg, value, verify_mask);
  unlock();
  return ok;
}

bool Ads7924::readDeviceId(uint8_t &device_id) {
  return readRegister(kRegResetAndId, device_id);
}

bool Ads7924::configureAcquisitionTime(uint8_t code) {
  if ((code & 0xE0) != 0) {
    setError(Error::kInvalidArgument);
    return false;
  }

  return writeRegisterVerified(kRegAcquireConfig, code, 0x1F);
}

bool Ads7924::configureDataReadyInterruptAll() {
  return writeRegisterVerified(kRegInterruptConfig,
                               kInterruptDataReadyAllActiveLowStatic, 0xFF);
}

bool Ads7924::startManualScan(uint8_t first_channel) {
  if (first_channel >= kChannelCount) {
    setError(Error::kInvalidArgument);
    return false;
  }

  const uint8_t mode =
      static_cast<uint8_t>(kModeManualScan | (first_channel & 0x03));
  return writeRegister(kRegModeControl, mode);
}

bool Ads7924::setIdle() {
  return writeRegister(kRegModeControl, kModeIdle);
}

bool Ads7924::readAllChannels(
    std::array<uint16_t, kChannelCount> &samples) {
  uint8_t raw_bytes[kChannelCount * 2] = {};

  if (!readRegisters(kRegData0Upper, raw_bytes, sizeof(raw_bytes))) {
    return false;
  }

  for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
    const uint8_t upper = raw_bytes[channel * 2];
    const uint8_t lower = raw_bytes[channel * 2 + 1];
    samples[channel] =
        static_cast<uint16_t>((static_cast<uint16_t>(upper) << 4) |
                              (static_cast<uint16_t>(lower) >> 4));
  }

  return true;
}

int Ads7924::readInterruptLevel() const {
  if (!config_valid_ || config_.int_pin < 0) {
    return -1;
  }

  return digitalRead(config_.int_pin);
}

bool Ads7924::waitForInterrupt(bool asserted, uint32_t timeout_ms) {
  if (!config_valid_) {
    setError(Error::kNotInitialized);
    return false;
  }

  const int expected_level = asserted ? LOW : HIGH;
  const uint32_t started_ms = millis();

  do {
    if (digitalRead(config_.int_pin) == expected_level) {
      setError(Error::kNone);
      return true;
    }
    delayMicroseconds(50);
  } while (static_cast<uint32_t>(millis() - started_ms) < timeout_ms);

  if (digitalRead(config_.int_pin) == expected_level) {
    setError(Error::kNone);
    return true;
  }

  setError(Error::kInterruptTimeout);
  return false;
}

uint32_t Ads7924::rawToMicrovolts(uint16_t raw,
                                  uint32_t reference_microvolts) {
  const uint16_t limited = std::min<uint16_t>(raw, 4095);
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(limited) * reference_microvolts + 2047ULL) /
      4095ULL);
}

const char *Ads7924::errorToString(Error error) {
  switch (error) {
    case Error::kNone:
      return "none";
    case Error::kInvalidArgument:
      return "invalid_argument";
    case Error::kNotInitialized:
      return "not_initialized";
    case Error::kMutexCreateFailed:
      return "mutex_create_failed";
    case Error::kMutexTimeout:
      return "mutex_timeout";
    case Error::kBusStuck:
      return "i2c_bus_stuck";
    case Error::kWireBeginFailed:
      return "wire_begin_failed";
    case Error::kWireBufferOverflow:
      return "wire_buffer_overflow";
    case Error::kAddressNack:
      return "address_nack";
    case Error::kDataNack:
      return "data_nack";
    case Error::kWireTimeout:
      return "wire_timeout";
    case Error::kWireOther:
      return "wire_other";
    case Error::kShortRead:
      return "short_read";
    case Error::kVerificationFailed:
      return "verification_failed";
    case Error::kInterruptTimeout:
      return "interrupt_timeout";
    default:
      return "unknown";
  }
}

}  // namespace gateway::adc
