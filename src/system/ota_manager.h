#pragma once

#include <stdint.h>

namespace gateway::ota {

    enum class State {
        Disabled,
        Idle,
        WaitingNetwork,
        CheckManifest,
        UpToDate,
        UpdateAvailable,
        Downloading,
        Verifying,
        Applying,
        Backoff,
        Rebooting,
        Failed
    };

    struct Status {
        State state;
        bool update_available;
        bool update_in_progress;
        uint8_t progress;
        uint32_t current_build;
        uint32_t latest_build;
        const char* current_version;
        const char* latest_version;
        const char* last_error;
    };

    void begin();
    void loop();

    bool request_check_now();
    bool request_upgrade_now();

    Status status();
    bool update_in_progress();
    const char* state_string(State state);

}  // namespace gateway::ota
