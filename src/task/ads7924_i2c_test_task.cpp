/**
 * @file ads7924_i2c_test_task.cpp
 * @brief ADS7924 INT 诊断与低 CPU 高速采样测试。
 */

#include "task/ads7924_i2c_test_task.h"

#include <Arduino.h>
#include <Wire.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "adc/ads7924.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gateway::ads7924_i2c_test_task {
namespace {

static constexpr const char kLogTag[] = "ADS7924_FAST";

static constexpr uint8_t kRegModeControl = 0x00;
static constexpr uint8_t kRegInterruptConfig = 0x12;
static constexpr uint8_t kRegPowerConfig = 0x15;

/*
 * INTCONFIG:
 * bits[4:2] = 110：四通道全部转换完成时 data-ready；
 * bit[1]    = 0：低有效；
 * bit[0]    = 0：静态电平。
 *
 * ALMCNT[7:5] 与 data-ready 无关，因此保持为 000。
 */
static constexpr uint8_t kIntDataReadyAllActiveLowStatic = 0x18;
static constexpr uint8_t kIntDataReadyAllActiveHighStatic = 0x1A;
static constexpr uint8_t kIntDefaultAlarmActiveLowStatic = 0x00;

static TaskHandle_t g_task_handle = nullptr;
static esp_timer_handle_t g_completion_timer = nullptr;
static adc::Ads7924 g_adc(Wire);

enum class CompletionSource : uint8_t {
  kInterrupt,
  kEspTimer,
};

struct RateStatistics {
  uint64_t period_started_us = 0;
  uint32_t scans = 0;
  uint32_t i2c_errors = 0;
  uint32_t completion_timeouts = 0;

  std::array<uint16_t, adc::Ads7924::kChannelCount> latest{};
  std::array<uint16_t, adc::Ads7924::kChannelCount> minimum{};
  std::array<uint16_t, adc::Ads7924::kChannelCount> maximum{};

  void resetPeriod(uint64_t now_us) {
    period_started_us = now_us;
    scans = 0;
    i2c_errors = 0;
    completion_timeouts = 0;
    minimum.fill(std::numeric_limits<uint16_t>::max());
    maximum.fill(0);
  }

  void addSample(
      const std::array<uint16_t, adc::Ads7924::kChannelCount> &sample) {
    latest = sample;
    ++scans;

    for (std::size_t channel = 0; channel < sample.size(); ++channel) {
      minimum[channel] = std::min(minimum[channel], sample[channel]);
      maximum[channel] = std::max(maximum[channel], sample[channel]);
    }
  }
};

void printDriverFailure(const char *step) {
  Serial.printf("[%s] FAIL step=%s error=%s wire_status=%u\r\n", kLogTag,
                step, adc::Ads7924::errorToString(g_adc.lastError()),
                static_cast<unsigned>(g_adc.lastWireStatus()));
}

const char *completionSourceName(CompletionSource source) {
  return source == CompletionSource::kInterrupt ? "GPIO13_INT"
                                                : "ESP_TIMER_FALLBACK";
}

void IRAM_ATTR interruptIsr(void *) {
  BaseType_t higher_priority_task_woken = pdFALSE;

  if (g_task_handle != nullptr) {
    vTaskNotifyGiveFromISR(g_task_handle, &higher_priority_task_woken);
  }

  if (higher_priority_task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

void completionTimerCallback(void *) {
  if (g_task_handle != nullptr) {
    xTaskNotifyGive(g_task_handle);
  }
}

bool createCompletionTimer() {
  if (g_completion_timer != nullptr) {
    return true;
  }

  esp_timer_create_args_t args = {};
  args.callback = completionTimerCallback;
  args.arg = nullptr;
  args.name = "ads7924_wait";

  const esp_err_t result = esp_timer_create(&args, &g_completion_timer);
  if (result != ESP_OK) {
    Serial.printf("[%s] FAIL esp_timer_create error=%d\r\n", kLogTag,
                  static_cast<int>(result));
    g_completion_timer = nullptr;
    return false;
  }

  return true;
}

void disableInterruptHandler() {
  detachInterrupt(digitalPinToInterrupt(kIntPin));
}

void enableInterruptHandler() {
  disableInterruptHandler();
  attachInterruptArg(digitalPinToInterrupt(kIntPin), interruptIsr, nullptr,
                     FALLING);
}

void drainTaskNotifications() {
  while (ulTaskNotifyTake(pdTRUE, 0) > 0) {
  }
}

bool waitForPinLevel(int expected_level, uint32_t timeout_ms) {
  const uint32_t started_ms = millis();

  do {
    if (digitalRead(kIntPin) == expected_level) {
      return true;
    }

    delayMicroseconds(50);
  } while (static_cast<uint32_t>(millis() - started_ms) < timeout_ms);

  return digitalRead(kIntPin) == expected_level;
}

bool writeAndVerifyInterruptConfig(uint8_t value) {
  if (!g_adc.writeRegisterVerified(kRegInterruptConfig, value, 0x1F)) {
    printDriverFailure("write_verify_intconfig");
    return false;
  }

  uint8_t actual = 0;
  if (!g_adc.readRegister(kRegInterruptConfig, actual)) {
    printDriverFailure("readback_intconfig");
    return false;
  }

  if ((actual & 0x1F) != (value & 0x1F)) {
    Serial.printf(
        "[%s] FAIL INTCONFIG expected_low5=0x%02X actual=0x%02X\r\n",
        kLogTag, static_cast<unsigned>(value & 0x1F),
        static_cast<unsigned>(actual));
    return false;
  }

  return true;
}

/**
 * 第一级 INT 测试：
 *
 * 在没有转换事件时切换 INTPOL。正常情况下：
 *   active-low  + inactive source => INT 为 HIGH；
 *   active-high + inactive source => INT 为 LOW。
 *
 * 该测试不依赖 ADC 转换时序，可直接区分 GPIO13/PCB 连接问题与 data-ready
 * 事件配置问题。
 */
bool testInterruptElectricalPath() {
  Serial.printf("[%s] INT stage1: polarity/electrical-path test begin\r\n",
                kLogTag);

  if (!g_adc.setIdle()) {
    printDriverFailure("int_stage1_set_idle");
    return false;
  }

  std::array<uint16_t, adc::Ads7924::kChannelCount> discard{};

  if (!writeAndVerifyInterruptConfig(kIntDataReadyAllActiveLowStatic)) {
    return false;
  }

  // 读取四个低位数据寄存器，清除可能遗留的 data-ready 静态状态。
  if (!g_adc.readAllChannels(discard)) {
    printDriverFailure("int_stage1_clear_stale_data_ready");
    return false;
  }

  if (!waitForPinLevel(HIGH, 5)) {
    Serial.printf(
        "[%s] INT STAGE1 FAIL active-low inactive level expected=HIGH "
        "actual=%d\r\n",
        kLogTag, digitalRead(kIntPin));
    return false;
  }

  if (!writeAndVerifyInterruptConfig(kIntDataReadyAllActiveHighStatic)) {
    return false;
  }

  if (!waitForPinLevel(LOW, 5)) {
    Serial.printf(
        "[%s] INT STAGE1 FAIL polarity switched in register but GPIO13 did "
        "not go LOW actual=%d; inspect ADS7924 INT pin, PCB trace and MCU "
        "GPIO number\r\n",
        kLogTag, digitalRead(kIntPin));
    return false;
  }

  if (!writeAndVerifyInterruptConfig(kIntDataReadyAllActiveLowStatic)) {
    return false;
  }

  if (!waitForPinLevel(HIGH, 5)) {
    Serial.printf(
        "[%s] INT STAGE1 FAIL polarity restored but GPIO13 did not return "
        "HIGH actual=%d\r\n",
        kLogTag, digitalRead(kIntPin));
    return false;
  }

  Serial.printf(
      "[%s] INT STAGE1 PASS: GPIO13 followed ADS7924 INTPOL HIGH->LOW->HIGH\r\n",
      kLogTag);
  return true;
}

/**
 * 第二级 INT 测试：Manual-Scan 完成后检查下降沿与静态低电平，
 * 读取数据后检查恢复高电平。
 */
bool testInterruptDataReadyEvent() {
  Serial.printf("[%s] INT stage2: four-channel data-ready test begin\r\n",
                kLogTag);

  if (!g_adc.configureAcquisitionTime(kIntTestAcquisitionCode)) {
    printDriverFailure("int_stage2_set_acquisition_time");
    return false;
  }

  if (!writeAndVerifyInterruptConfig(kIntDataReadyAllActiveLowStatic)) {
    return false;
  }

  std::array<uint16_t, adc::Ads7924::kChannelCount> samples{};

  if (!g_adc.readAllChannels(samples)) {
    printDriverFailure("int_stage2_clear_stale_data_ready");
    return false;
  }

  if (!waitForPinLevel(HIGH, 5)) {
    Serial.printf(
        "[%s] INT STAGE2 FAIL before scan expected=HIGH actual=%d\r\n",
        kLogTag, digitalRead(kIntPin));
    return false;
  }

  enableInterruptHandler();
  drainTaskNotifications();

  const int64_t started_us = esp_timer_get_time();

  if (!g_adc.startManualScan(0)) {
    disableInterruptHandler();
    printDriverFailure("int_stage2_start_manual_scan");
    return false;
  }

  const uint32_t notified =
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kCompletionTimeoutMs));
  const int64_t elapsed_us = esp_timer_get_time() - started_us;
  const int level_after_wait = digitalRead(kIntPin);

  if (notified == 0) {
    disableInterruptHandler();

    if (level_after_wait == LOW) {
      Serial.printf(
          "[%s] INT STAGE2 FAIL: ADS7924 INT reached LOW, but MCU falling-edge "
          "interrupt was not delivered; check GPIO13 interrupt configuration\r\n",
          kLogTag);
    } else {
      Serial.printf(
          "[%s] INT STAGE2 FAIL: data-ready never asserted LOW, actual=%d\r\n",
          kLogTag, level_after_wait);
    }

    g_adc.setIdle();
    return false;
  }

  if (level_after_wait != LOW) {
    disableInterruptHandler();
    Serial.printf(
        "[%s] INT STAGE2 FAIL: falling-edge notification received but static "
        "INT level is not LOW actual=%d\r\n",
        kLogTag, level_after_wait);
    g_adc.setIdle();
    return false;
  }

  if (!g_adc.readAllChannels(samples)) {
    disableInterruptHandler();
    printDriverFailure("int_stage2_read_channels");
    g_adc.setIdle();
    return false;
  }

  if (!waitForPinLevel(HIGH, 5)) {
    disableInterruptHandler();
    Serial.printf(
        "[%s] INT STAGE2 FAIL after data read expected=HIGH actual=%d\r\n",
        kLogTag, digitalRead(kIntPin));
    g_adc.setIdle();
    return false;
  }

  disableInterruptHandler();

  if (!g_adc.setIdle()) {
    printDriverFailure("int_stage2_set_idle");
    return false;
  }

  Serial.printf(
      "[%s] INT STAGE2 PASS: falling edge received in %lld us; data read "
      "released GPIO13 HIGH; sample=%u,%u,%u,%u\r\n",
      kLogTag, static_cast<long long>(elapsed_us),
      static_cast<unsigned>(samples[0]), static_cast<unsigned>(samples[1]),
      static_cast<unsigned>(samples[2]), static_cast<unsigned>(samples[3]));

  return true;
}

bool initializeAdc() {
  adc::Ads7924::Config config;
  config.sda_pin = kSdaPin;
  config.scl_pin = kSclPin;
  config.int_pin = kIntPin;

  /*
   * 不测试、不脉冲 GPIO14。由于当前硬件为直连且未确认有外部上拉，
   * 驱动仍将 GPIO14 保持为 HIGH，避免 RESET 输入悬空。
   */
  config.reset_pin = kResetPin;

  config.address = kAddress;
  config.clock_hz = kI2cClockHz;
  config.i2c_timeout_ms = 50;
  config.retry_count = 3;
  config.retry_delay_ms = 1;

  if (!g_adc.begin(config)) {
    printDriverFailure("driver_begin");
    return false;
  }

  gpio_pullup_dis(static_cast<gpio_num_t>(kIntPin));
  gpio_pulldown_dis(static_cast<gpio_num_t>(kIntPin));

  if (!g_adc.probe()) {
    printDriverFailure("address_ack");
    return false;
  }

  uint8_t device_id = 0;
  if (!g_adc.readDeviceId(device_id)) {
    printDriverFailure("read_device_id");
    return false;
  }

  if (device_id != adc::Ads7924::kExpectedDeviceId) {
    Serial.printf("[%s] FAIL device_id expected=0x%02X actual=0x%02X\r\n",
                  kLogTag,
                  static_cast<unsigned>(adc::Ads7924::kExpectedDeviceId),
                  static_cast<unsigned>(device_id));
    return false;
  }

  // 只使用 I2C 软件复位，不使用 GPIO14 RESET。
  if (!g_adc.softwareReset()) {
    printDriverFailure("software_reset");
    return false;
  }

  /*
   * 禁用 PWRCON 并将 PWRUPTIME 设为 0。直连 MUXOUT/ADCIN 时不需要为外部
   * 放大器预留上电时间。
   */
  if (!g_adc.writeRegisterVerified(kRegPowerConfig, 0x00, 0xFF)) {
    printDriverFailure("configure_power_time");
    return false;
  }

  Serial.printf(
      "[%s] PASS init address=0x%02X id=0x%02X I2C=%luHz; GPIO14 held HIGH "
      "only, hardware RESET unused\r\n",
      kLogTag, static_cast<unsigned>(kAddress),
      static_cast<unsigned>(device_id),
      static_cast<unsigned long>(kI2cClockHz));

  return true;
}

bool configureHighRateSampling(CompletionSource source) {
  if (!g_adc.setIdle()) {
    printDriverFailure("high_rate_set_idle");
    return false;
  }

  if (!g_adc.configureAcquisitionTime(kHighRateAcquisitionCode)) {
    printDriverFailure("high_rate_set_acquisition_time");
    return false;
  }

  if (source == CompletionSource::kInterrupt) {
    if (!writeAndVerifyInterruptConfig(kIntDataReadyAllActiveLowStatic)) {
      return false;
    }

    std::array<uint16_t, adc::Ads7924::kChannelCount> discard{};
    if (!g_adc.readAllChannels(discard)) {
      printDriverFailure("high_rate_clear_stale_data_ready");
      return false;
    }

    if (!waitForPinLevel(HIGH, 5)) {
      Serial.printf(
          "[%s] FAIL high-rate INT idle expected=HIGH actual=%d\r\n",
          kLogTag, digitalRead(kIntPin));
      return false;
    }

    enableInterruptHandler();
  } else {
    disableInterruptHandler();

    if (!g_adc.writeRegister(kRegInterruptConfig,
                             kIntDefaultAlarmActiveLowStatic)) {
      printDriverFailure("disable_data_ready_int_for_timer_mode");
      return false;
    }

    if (!createCompletionTimer()) {
      return false;
    }
  }

  drainTaskNotifications();

  Serial.printf(
      "[%s] high-rate sampling source=%s ACQTIME=%u "
      "(nominal acquisition=%u us)\r\n",
      kLogTag, completionSourceName(source),
      static_cast<unsigned>(kHighRateAcquisitionCode),
      static_cast<unsigned>(kHighRateAcquisitionCode * 2U + 6U));

  return true;
}

bool waitForConversionCompletion(CompletionSource source) {
  if (source == CompletionSource::kEspTimer) {
    if (g_completion_timer == nullptr) {
      return false;
    }

    const esp_err_t timer_result =
        esp_timer_start_once(g_completion_timer, kTimerFallbackWaitUs);

    if (timer_result != ESP_OK) {
      Serial.printf("[%s] FAIL esp_timer_start_once error=%d\r\n", kLogTag,
                    static_cast<int>(timer_result));
      return false;
    }
  }

  return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kCompletionTimeoutMs)) > 0;
}

bool acquireOneScan(
    CompletionSource source,
    std::array<uint16_t, adc::Ads7924::kChannelCount> &samples,
    bool &completion_timeout) {
  completion_timeout = false;
  drainTaskNotifications();

  if (!g_adc.startManualScan(0)) {
    printDriverFailure("start_manual_scan");
    return false;
  }

  if (!waitForConversionCompletion(source)) {
    completion_timeout = true;
    g_adc.setIdle();
    return false;
  }

  if (source == CompletionSource::kInterrupt &&
      digitalRead(kIntPin) != LOW) {
    Serial.printf(
        "[%s] FAIL completion notification received but GPIO13 is not LOW\r\n",
        kLogTag);
    g_adc.setIdle();
    return false;
  }

  /*
   * 一次连续读取 0x02~0x09，共 8 字节。
   * 该读取同时取得四通道结果，并清除静态 data-ready INT。
   */
  if (!g_adc.readAllChannels(samples)) {
    printDriverFailure("read_all_channels");
    g_adc.setIdle();
    return false;
  }

  /*
   * Manual-Scan 完成后允许直接再次写 Manual-Scan，不需要每次切换 Idle/Awake。
   * 因此成功路径不执行额外寄存器写入。
   */
  return true;
}

void printStatistics(const RateStatistics &stats, uint64_t now_us,
                     CompletionSource source) {
  const uint64_t elapsed_us = now_us - stats.period_started_us;
  if (elapsed_us == 0) {
    return;
  }

  const uint64_t scans_per_second_x10 =
      (static_cast<uint64_t>(stats.scans) * 10000000ULL) / elapsed_us;
  const uint64_t channel_samples_per_second_x10 =
      scans_per_second_x10 * adc::Ads7924::kChannelCount;

  Serial.printf(
      "[%s] RATE source=%s scans=%lu rate=%llu.%llu scans/s "
      "channels=%llu.%llu samples/s i2c_errors=%lu timeouts=%lu\r\n",
      kLogTag, completionSourceName(source),
      static_cast<unsigned long>(stats.scans),
      static_cast<unsigned long long>(scans_per_second_x10 / 10ULL),
      static_cast<unsigned long long>(scans_per_second_x10 % 10ULL),
      static_cast<unsigned long long>(channel_samples_per_second_x10 / 10ULL),
      static_cast<unsigned long long>(
          channel_samples_per_second_x10 % 10ULL),
      static_cast<unsigned long>(stats.i2c_errors),
      static_cast<unsigned long>(stats.completion_timeouts));

  Serial.printf(
      "[%s] DATA latest=%u,%u,%u,%u min=%u,%u,%u,%u max=%u,%u,%u,%u\r\n",
      kLogTag, static_cast<unsigned>(stats.latest[0]),
      static_cast<unsigned>(stats.latest[1]),
      static_cast<unsigned>(stats.latest[2]),
      static_cast<unsigned>(stats.latest[3]),
      static_cast<unsigned>(stats.minimum[0]),
      static_cast<unsigned>(stats.minimum[1]),
      static_cast<unsigned>(stats.minimum[2]),
      static_cast<unsigned>(stats.minimum[3]),
      static_cast<unsigned>(stats.maximum[0]),
      static_cast<unsigned>(stats.maximum[1]),
      static_cast<unsigned>(stats.maximum[2]),
      static_cast<unsigned>(stats.maximum[3]));
}

bool recoverWithoutHardwareReset(CompletionSource source) {
  disableInterruptHandler();

  Serial.printf("[%s] recovering I2C/ADC without GPIO14 RESET\r\n", kLogTag);

  if (!g_adc.recoverBus()) {
    printDriverFailure("recover_i2c_bus");
    return false;
  }

  if (!g_adc.probe()) {
    printDriverFailure("recover_probe");
    return false;
  }

  if (!g_adc.softwareReset()) {
    printDriverFailure("recover_software_reset");
    return false;
  }

  if (!g_adc.writeRegisterVerified(kRegPowerConfig, 0x00, 0xFF)) {
    printDriverFailure("recover_power_config");
    return false;
  }

  return configureHighRateSampling(source);
}

void runHighRateLoop(CompletionSource initial_source) {
  CompletionSource source = initial_source;

  if (!configureHighRateSampling(source)) {
    if (source == CompletionSource::kInterrupt && kEnableTimerFallback) {
      Serial.printf(
          "[%s] INT high-rate setup failed; switching to esp_timer fallback\r\n",
          kLogTag);
      source = CompletionSource::kEspTimer;

      if (!configureHighRateSampling(source)) {
        Serial.printf("[%s] FATAL unable to configure fallback sampling\r\n",
                      kLogTag);
        return;
      }
    } else {
      return;
    }
  }

  RateStatistics stats;
  stats.resetPeriod(static_cast<uint64_t>(esp_timer_get_time()));

  uint8_t consecutive_completion_timeouts = 0;

  for (;;) {
    std::array<uint16_t, adc::Ads7924::kChannelCount> samples{};
    bool completion_timeout = false;

    const bool success =
        acquireOneScan(source, samples, completion_timeout);

    if (success) {
      consecutive_completion_timeouts = 0;
      stats.addSample(samples);
    } else {
      if (completion_timeout) {
        ++stats.completion_timeouts;
        ++consecutive_completion_timeouts;
      } else {
        ++stats.i2c_errors;
        consecutive_completion_timeouts = 0;
      }

      if (source == CompletionSource::kInterrupt &&
          kEnableTimerFallback &&
          consecutive_completion_timeouts >=
              kRuntimeTimeoutsBeforeFallback) {
        Serial.printf(
            "[%s] GPIO13 INT timed out %u consecutive times; switching to "
            "esp_timer fallback without stopping ADC service\r\n",
            kLogTag, static_cast<unsigned>(consecutive_completion_timeouts));

        disableInterruptHandler();
        source = CompletionSource::kEspTimer;
        consecutive_completion_timeouts = 0;

        if (!configureHighRateSampling(source)) {
          vTaskDelay(pdMS_TO_TICKS(100));
          recoverWithoutHardwareReset(source);
        }
      } else if (!completion_timeout) {
        if (!recoverWithoutHardwareReset(source)) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
    }

    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    if (now_us - stats.period_started_us >=
        static_cast<uint64_t>(kStatisticsPeriodMs) * 1000ULL) {
      printStatistics(stats, now_us, source);
      stats.resetPeriod(now_us);
    }
  }
}

void taskMain(void *) {
  Serial.printf(
      "[%s] task core=%d I2C=%luHz RESET-test=disabled "
      "INT-test=two-stage timer-fallback=%s\r\n",
      kLogTag, kPinnedCore, static_cast<unsigned long>(kI2cClockHz),
      kEnableTimerFallback ? "enabled" : "disabled");

  while (!initializeAdc()) {
    Serial.printf("[%s] init failed; retrying in 2 s\r\n", kLogTag);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  const bool electrical_path_ok = testInterruptElectricalPath();
  const bool data_ready_ok =
      electrical_path_ok && testInterruptDataReadyEvent();
  const bool interrupt_ok = electrical_path_ok && data_ready_ok;

  CompletionSource source = CompletionSource::kInterrupt;

  if (interrupt_ok) {
    Serial.printf(
        "[%s] INT FINAL PASS: GPIO13 electrical path and four-channel "
        "data-ready event are both operational\r\n",
        kLogTag);
  } else if (kEnableTimerFallback) {
    source = CompletionSource::kEspTimer;
    Serial.printf(
        "[%s] INT FINAL FAIL: high-rate ADC will continue with esp_timer; "
        "GPIO13 is not required for fallback mode\r\n",
        kLogTag);
  } else {
    Serial.printf(
        "[%s] INT FINAL FAIL and timer fallback disabled; task stopped\r\n",
        kLogTag);
    vTaskDelete(nullptr);
    return;
  }

  runHighRateLoop(source);

  Serial.printf("[%s] high-rate loop exited unexpectedly\r\n", kLogTag);
  vTaskDelete(nullptr);
}

}  // namespace

void start() {
  if (g_task_handle != nullptr) {
    Serial.printf("[%s] already started\r\n", kLogTag);
    return;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      taskMain, "ads7924_fast", kTaskStackBytes, nullptr,
      static_cast<UBaseType_t>(kTaskPriority), &g_task_handle, kPinnedCore);

  if (result != pdPASS) {
    g_task_handle = nullptr;
    Serial.printf("[%s] FAIL create task\r\n", kLogTag);
  }
}

}  // namespace gateway::ads7924_i2c_test_task
