#include "task/ota_task.h"

#include <Arduino.h>

#include "config/ota_config.h"
#include "system/ota_manager.h"

namespace gateway::ota_task {
    namespace {

        static constexpr const char* kTag = "OTA_TASK";

        void task_ota(void*) {
            Serial.printf("[%s] start core=%d\n",
                kTag,
                static_cast<int>(xPortGetCoreID()));

            gateway::ota::begin();

            for (;;) {
                gateway::ota::loop();
                vTaskDelay(pdMS_TO_TICKS(gateway::ota_config::kOtaTaskLoopDelayMs));
            }
        }

    }  // namespace

    void start() {
        xTaskCreatePinnedToCore(
            task_ota,
            "OTA_TASK",
            gateway::ota_config::kOtaTaskStackBytes,
            nullptr,
            static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1),
            nullptr,
            gateway::ota_config::kOtaTaskPinnedCore);
    }

}  // namespace gateway::ota_task
