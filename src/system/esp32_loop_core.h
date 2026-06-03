#pragma once

/**
 * @file esp32_loop_core.h
 * @brief Arduino 主循环任务所在 CPU 核常量，供 MQTT/WiFi 同核 pinned 使用。
 */

#include <freertos/portmacro.h>

/** Arduino loopTask 所在核；与上行聚合任务 pinned 策略对齐 ESP32-S3。 */
#if defined(CONFIG_ARDUINO_RUNNING_CORE)
inline constexpr BaseType_t kArduinoLoopPinnedCore =
    static_cast<BaseType_t>(CONFIG_ARDUINO_RUNNING_CORE);
#else
inline constexpr BaseType_t kArduinoLoopPinnedCore = static_cast<BaseType_t>(1);
#endif
