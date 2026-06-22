#pragma once

#include <stddef.h>
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

    static constexpr size_t kStatusVersionCapacity = 33;
    static constexpr size_t kStatusErrorCapacity = 96;

    struct Status {
        State state;
        bool update_available;
        bool update_in_progress;
        uint8_t progress;
        uint32_t current_build;
        uint32_t latest_build;
        char current_version[kStatusVersionCapacity];
        char latest_version[kStatusVersionCapacity];
        char last_error[kStatusErrorCapacity];
    };

    void begin();
    void loop();

    bool request_check_now();
    bool request_upgrade_now();

    Status status();
    bool update_in_progress();
    const char* state_string(State state);

}  // namespace gateway::ota
