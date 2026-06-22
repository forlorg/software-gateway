#pragma once

#include <Arduino.h>

#include <stddef.h>
#include <stdint.h>

#include "ota/ota_types.h"

namespace gateway::ota::detail {

    enum class InstallStage : uint8_t {
        Downloading,
        Verifying,
        Applying,
        Rebooting
    };

    struct InstallCallbacks {
        void (*on_stage)(InstallStage stage) = nullptr;
        void (*on_progress)(uint8_t progress) = nullptr;
    };

    bool download_and_apply(const OtaManifest& manifest,
                            const EndpointContext& endpoint,
                            const InstallCallbacks& callbacks,
                            String* error_code);

}  // namespace gateway::ota::detail
