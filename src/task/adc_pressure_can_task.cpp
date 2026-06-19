/**
 * @file adc_pressure_can_task.cpp
 * @brief ADS7924 四通道压力采样、4 点块平均滤波、PGN 0x1708 发送任务。
 */
#include "task/adc_pressure_can_task.h"

#include <Arduino.h>
#include <Wire.h>

#include <array>
#include <cstdint>

#include "adc/ads7924_pressure_sampler.h"
#include "adc/pressure_can_frame.h"
#include "can/can_tx.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace gateway::adc_pressure_can_task {

namespace {

using gateway::adc::Ads7924PressureSampler;
using gateway::adc::pressure_can::BlockAverageFilter;
using gateway::adc::pressure_can::PressureFrameData;
using gateway::adc::pressure_can::buildPgn1708Frame;
using gateway::adc::pressure_can::convertAveragedRaw;
using gateway::adc::pressure_can::kCanFramePeriodMs;
using gateway::adc::pressure_can::kCanIdPgn1708;
using gateway::adc::pressure_can::kChannelCount;
using gateway::adc::pressure_can::kFilterSampleCount;
using gateway::adc::pressure_can::kSampleGroupPeriodMs;

static constexpr const char kLogTag[] = "ADC_PRESS";

static constexpr int kSdaPin = 11;
static constexpr int kSclPin = 12;
static constexpr int kIntPin = 13;
static constexpr int kResetPin = 14;
static constexpr uint8_t kAddress = 0x48;
static constexpr uint32_t kI2cClockHz = 100000;
static constexpr uint8_t kAcquisitionTimeCode = 0;

static constexpr uint32_t kGpio3PollPeriodMs = 20;
static constexpr uint8_t kGpio3StableSampleCount = 3;
static constexpr uint32_t kInitRetryMs = 2000;
static constexpr uint32_t kStatisticsPeriodMs = 1000;
static constexpr uint8_t kConsecutiveErrorsBeforeRecover = 3;

static TaskHandle_t g_task_handle = nullptr;
static Ads7924PressureSampler g_sampler(Wire);

const char *levelName(int level) {
  return level == HIGH ? "HIGH" : "LOW";
}

void configureGpio3Input() {
  pinMode(kGpio3InputPin, INPUT);
  gpio_pullup_dis(static_cast<gpio_num_t>(kGpio3InputPin));
  gpio_pulldown_dis(static_cast<gpio_num_t>(kGpio3InputPin));
}

int readStableGpio3Level() {
  configureGpio3Input();

  int candidate = digitalRead(kGpio3InputPin);
  uint8_t stable_count = 1;

  while (stable_count < kGpio3StableSampleCount) {
    delay(kGpio3PollPeriodMs);
    const int sampled = digitalRead(kGpio3InputPin);
    if (sampled == candidate) {
      ++stable_count;
    } else {
      candidate = sampled;
      stable_count = 1;
    }
  }

  return candidate;
}

void printSamplerFailure(const char *step) {
  Serial.printf("[%s] FAIL step=%s error=%s wire_status=%u device_id=0x%02X\r\n",
                kLogTag, step, g_sampler.lastErrorName(),
                static_cast<unsigned>(g_sampler.lastWireStatus()),
                static_cast<unsigned>(g_sampler.lastDeviceId()));
}

bool initializeSampler() {
  Ads7924PressureSampler::Config config;
  config.sda_pin = kSdaPin;
  config.scl_pin = kSclPin;
  config.int_pin = kIntPin;
  config.reset_pin = kResetPin;
  config.address = kAddress;
  config.i2c_clock_hz = kI2cClockHz;
  config.acquisition_time_code = kAcquisitionTimeCode;
  config.i2c_timeout_ms = 50;
  config.retry_count = 3;
  config.retry_delay_ms = 1;

  if (!g_sampler.begin(config)) {
    printSamplerFailure("sampler_begin");
    return false;
  }

  if (!g_sampler.prime()) {
    printSamplerFailure("sampler_prime");
    return false;
  }

  Serial.printf(
      "[%s] ADC ready addr=0x%02X id=0x%02X I2C=%luHz mode=manual-scan "
      "sample_group=%lums filter=%u output=%lums can_id=0x%08lX\r\n",
      kLogTag, static_cast<unsigned>(kAddress),
      static_cast<unsigned>(g_sampler.lastDeviceId()),
      static_cast<unsigned long>(kI2cClockHz),
      static_cast<unsigned long>(kSampleGroupPeriodMs),
      static_cast<unsigned>(kFilterSampleCount),
      static_cast<unsigned long>(kCanFramePeriodMs),
      static_cast<unsigned long>(kCanIdPgn1708));

  return true;
}

bool recoverSampler() {
  Serial.printf("[%s] recovering ADS7924/I2C without GPIO14 hardware reset\r\n",
                kLogTag);

  if (!g_sampler.recover()) {
    printSamplerFailure("sampler_recover");
    return false;
  }

  if (!g_sampler.prime()) {
    printSamplerFailure("sampler_recover_prime");
    return false;
  }

  Serial.printf("[%s] recovery complete\r\n", kLogTag);
  return true;
}

void printPressureData(const PressureFrameData &data, bool can_queued) {
  Serial.printf(
      "[%s] DATA avg_raw=%u,%u,%u,%u mv=%u,%u,%u,%u "
      "can_raw=%u,%u,%u,%u pressure_kpa=%d,%d,%d,%d id=0x%08lX queued=%s\r\n",
      kLogTag,
      static_cast<unsigned>(data.averaged_raw[0]),
      static_cast<unsigned>(data.averaged_raw[1]),
      static_cast<unsigned>(data.averaged_raw[2]),
      static_cast<unsigned>(data.averaged_raw[3]),
      static_cast<unsigned>(data.millivolts[0]),
      static_cast<unsigned>(data.millivolts[1]),
      static_cast<unsigned>(data.millivolts[2]),
      static_cast<unsigned>(data.millivolts[3]),
      static_cast<unsigned>(data.can_raw[0]),
      static_cast<unsigned>(data.can_raw[1]),
      static_cast<unsigned>(data.can_raw[2]),
      static_cast<unsigned>(data.can_raw[3]),
      static_cast<int>(data.pressure_kpa[0]),
      static_cast<int>(data.pressure_kpa[1]),
      static_cast<int>(data.pressure_kpa[2]),
      static_cast<int>(data.pressure_kpa[3]),
      static_cast<unsigned long>(kCanIdPgn1708), can_queued ? "yes" : "no");
}

void taskMain(void *) {
  Serial.printf("[%s] task started core=%d GPIO3=LOW I2C=%luHz\r\n", kLogTag,
                kPinnedCore, static_cast<unsigned long>(kI2cClockHz));

  while (!initializeSampler()) {
    Serial.printf("[%s] init failed; retry in %lums\r\n", kLogTag,
                  static_cast<unsigned long>(kInitRetryMs));
    vTaskDelay(pdMS_TO_TICKS(kInitRetryMs));
  }

  BlockAverageFilter filter;
  uint32_t sample_groups = 0;
  uint32_t can_frames = 0;
  uint32_t i2c_errors = 0;
  uint32_t can_enqueue_errors = 0;
  uint8_t consecutive_errors = 0;

  TickType_t last_wake = xTaskGetTickCount();
  TickType_t last_statistics = last_wake;
  PressureFrameData latest_data{};
  bool have_latest_data = false;

  for (;;) {
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kSampleGroupPeriodMs));

    std::array<uint16_t, kChannelCount> raw{};
    if (!g_sampler.readPreviousAndStartNext(raw)) {
      ++i2c_errors;
      ++consecutive_errors;
      printSamplerFailure("read_previous_start_next");

      if (consecutive_errors >= kConsecutiveErrorsBeforeRecover) {
        consecutive_errors = 0;
        if (!recoverSampler()) {
          vTaskDelay(pdMS_TO_TICKS(kInitRetryMs));
          initializeSampler();
        }
        filter.reset();
      }
      continue;
    }

    consecutive_errors = 0;
    ++sample_groups;

    if (!filter.addRawGroup(raw)) {
      filter.reset();
      filter.addRawGroup(raw);
    }

    if (filter.ready()) {
      const auto averaged_raw = filter.consumeAveragedRaw();
      latest_data = convertAveragedRaw(averaged_raw);
      have_latest_data = true;

      const auto msg = buildPgn1708Frame(latest_data);
      const bool queued = gateway::can_tx::enqueue(msg);
      if (!queued) {
        ++can_enqueue_errors;
      }
      ++can_frames;
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<TickType_t>(now - last_statistics) >=
        pdMS_TO_TICKS(kStatisticsPeriodMs)) {
      last_statistics = now;
      // Serial.printf(
      //     "[%s] RATE sample_groups=%lu target_groups=200/s "
      //     "can_frames=%lu target_frames=50/s i2c_errors=%lu "
      //     "can_enqueue_errors=%lu\r\n",
      //     kLogTag, static_cast<unsigned long>(sample_groups),
      //     static_cast<unsigned long>(can_frames),
      //     static_cast<unsigned long>(i2c_errors),
      //     static_cast<unsigned long>(can_enqueue_errors));

      // if (have_latest_data) {
      //   printPressureData(latest_data, can_enqueue_errors == 0);
      // }
    }
  }
}

} // namespace

void start() {
  if (g_task_handle != nullptr) {
    Serial.printf("[%s] already started\r\n", kLogTag);
    return;
  }

  const int gpio3_level = readStableGpio3Level();
  Serial.printf("[%s] GPIO%d stable=%s\r\n", kLogTag, kGpio3InputPin,
                levelName(gpio3_level));

  if (gpio3_level == HIGH) {
    Serial.printf("[%s] disabled by GPIO%d=HIGH; task not created; "
                  "PGN 0x1708 will not be sent\r\n",
                  kLogTag, kGpio3InputPin);
    return;
  }

  const BaseType_t result = xTaskCreatePinnedToCore(
      taskMain, "adc_pressure_can", kTaskStackBytes, nullptr,
      static_cast<UBaseType_t>(kTaskPriority), &g_task_handle, kPinnedCore);

  if (result != pdPASS) {
    g_task_handle = nullptr;
    Serial.printf("[%s] FAIL create task\r\n", kLogTag);
    return;
  }

  Serial.printf("[%s] created task core=%d stack=%lu priority=%lu\r\n", kLogTag,
                kPinnedCore, static_cast<unsigned long>(kTaskStackBytes),
                static_cast<unsigned long>(kTaskPriority));
}

} // namespace gateway::adc_pressure_can_task
