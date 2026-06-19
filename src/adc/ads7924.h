#pragma once

/**
 * @file ads7924.h
 * @brief ADS7924 I2C 驱动。封装总线初始化、恢复、寄存器访问、复位、INT 和四通道采样。
 */

#include <Arduino.h>
#include <Wire.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace gateway::adc {

    class Ads7924 final {
    public:
        static constexpr uint8_t kDefaultAddress = 0x48;
        static constexpr uint8_t kExpectedDeviceId = 0x18;
        static constexpr std::size_t kChannelCount = 4;

        enum class Error : uint8_t {
            kNone = 0,
            kInvalidArgument,
            kNotInitialized,
            kMutexCreateFailed,
            kMutexTimeout,
            kBusStuck,
            kWireBeginFailed,
            kWireBufferOverflow,
            kAddressNack,
            kDataNack,
            kWireTimeout,
            kWireOther,
            kShortRead,
            kVerificationFailed,
            kInterruptTimeout,
        };

        struct Config {
            int sda_pin = 11;
            int scl_pin = 12;
            int int_pin = 13;
            int reset_pin = 14;

            uint8_t address = kDefaultAddress;
            uint32_t clock_hz = 100000;
            uint16_t i2c_timeout_ms = 50;

            uint8_t retry_count = 3;
            uint16_t retry_delay_ms = 2;
            uint16_t mutex_timeout_ms = 100;

            uint16_t reset_low_us = 100;
            uint16_t reset_recovery_ms = 2;
            uint16_t bus_release_timeout_us = 1000;
        };

        explicit Ads7924(TwoWire &wire);
        ~Ads7924();

        Ads7924(const Ads7924 &) = delete;
        Ads7924 &operator=(const Ads7924 &) = delete;

        /**
         * @brief 配置 RESET/INT 引脚、恢复 I2C 总线并启动 TwoWire。
         *
         * 该实例被视为所传入 TwoWire 总线的初始化方。共享同一 I2C 总线的其它设备
         * 必须使用相同 SDA/SCL/频率，并在更高层统一进行并发访问管理。
         */
        bool begin(const Config &config);

        /** 释放被从设备卡住的 SDA，并重新初始化 TwoWire。 */
        bool recoverBus();

        /** 仅检查固定地址是否收到 ACK。 */
        bool probe();

        /** GPIO RESET 硬件复位，低有效。 */
        bool hardwareReset();

        /** 向 0x16 写入 0xAA，执行 ADS7924 软件复位。 */
        bool softwareReset();

        bool readDeviceId(uint8_t &device_id);

        bool readRegister(uint8_t reg, uint8_t &value);
        bool readRegisters(uint8_t first_reg, uint8_t *data, std::size_t length);

        bool writeRegister(uint8_t reg, uint8_t value);
        bool writeRegisters(uint8_t first_reg, const uint8_t *data, std::size_t length);

        /**
         * @brief 写入后读回校验。
         * @param verify_mask 仅比较掩码覆盖的位，适合含只读/动态位的寄存器。
         */
        bool writeRegisterVerified(uint8_t reg, uint8_t value, uint8_t verify_mask = 0xFF);

        /** ACQTIME[4:0]，实际采集时间为 code * 2 us + 6 us。 */
        bool configureAcquisitionTime(uint8_t code);

        /**
         * @brief INT 配置为：四通道均转换完成、低有效、静态电平。
         *
         * 数据寄存器读取完成后，ADS7924 会释放 INT。
         */
        bool configureDataReadyInterruptAll();

        /** 启动一次 Manual-Scan，从 first_channel 开始依次转换四路。 */
        bool startManualScan(uint8_t first_channel = 0);

        bool setIdle();

        /** 连续读取 0x02~0x09，并解析四个 12 位结果。 */
        bool readAllChannels(std::array<uint16_t, kChannelCount> &samples);

        int readInterruptLevel() const;

        /**
         * @brief 等待 INT 状态。
         * @param asserted true 等待低电平；false 等待高电平。
         */
        bool waitForInterrupt(bool asserted, uint32_t timeout_ms);

        Error lastError() const { return last_error_; }
        uint8_t lastWireStatus() const { return last_wire_status_; }
        const Config &config() const { return config_; }

        static const char *errorToString(Error error);
        static uint32_t rawToMicrovolts(uint16_t raw, uint32_t reference_microvolts);

    private:
        static constexpr uint8_t kRegModeControl = 0x00;
        static constexpr uint8_t kRegData0Upper = 0x02;
        static constexpr uint8_t kRegInterruptConfig = 0x12;
        static constexpr uint8_t kRegAcquireConfig = 0x14;
        static constexpr uint8_t kRegResetAndId = 0x16;

        static constexpr uint8_t kPointerIncrement = 0x80;
        static constexpr uint8_t kPointerAddressMask = 0x1F;

        static constexpr uint8_t kModeIdle = 0x00;
        static constexpr uint8_t kModeManualScan = 0xC8;
        static constexpr uint8_t kInterruptDataReadyAllActiveLowStatic = 0xF8;
        static constexpr uint8_t kSoftwareResetValue = 0xAA;

        bool lock();
        void unlock();

        bool validateRegisterAccess(uint8_t first_reg, std::size_t length) const;
        bool recoverBusUnlocked();
        bool restartWireUnlocked();

        bool probeUnlocked();
        bool readRegistersUnlocked(uint8_t first_reg, uint8_t *data, std::size_t length);
        bool writeRegistersUnlocked(uint8_t first_reg, const uint8_t *data, std::size_t length);
        bool writeRegisterVerifiedUnlocked(uint8_t reg, uint8_t value, uint8_t verify_mask);

        bool waitPinHigh(int pin, uint32_t timeout_us) const;
        void setWireError(uint8_t status);
        void setError(Error error) { last_error_ = error; }

        TwoWire &wire_;
        Config config_{};
        SemaphoreHandle_t mutex_ = nullptr;

        bool config_valid_ = false;
        bool wire_started_ = false;
        bool initialized_ = false;

        Error last_error_ = Error::kNone;
        uint8_t last_wire_status_ = 0;
    };

}  // namespace gateway::adc
