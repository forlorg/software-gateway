#include "system/version.h"

#ifndef FW_PROJECT_NAME
#define FW_PROJECT_NAME "ps-pro-gateway"
#endif

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

#ifndef FW_HW
#define FW_HW "ESP32-S3-WROOM-1U-N16R8"
#endif

#ifndef FW_CHANNEL
#define FW_CHANNEL "stable"
#endif

namespace gateway::version {

    const char* project_name() { return FW_PROJECT_NAME; }
    const char* firmware_version() { return FW_VERSION; }
    uint32_t firmware_build() { return FW_BUILD; }
    const char* hardware() { return FW_HW; }
    const char* channel() { return FW_CHANNEL; }
    const char* build_time() { return __DATE__ " " __TIME__; }

}  // namespace gateway::version
