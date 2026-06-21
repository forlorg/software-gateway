#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef FW_PROJECT_NAME
#define FW_PROJECT_NAME "ps-pro-gateway"
#endif

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

#ifndef FW_BUILD
#define FW_BUILD 2026061801
#endif

#ifndef FW_HW
#define FW_HW "ESP32-S3-WROOM-1U-N16R8"
#endif

#ifndef FW_CHANNEL
#define FW_CHANNEL "stable"
#endif

#ifndef OTA_PRIMARY_SCHEME
#define OTA_PRIMARY_SCHEME "http"
#endif

#ifndef OTA_PRIMARY_HOST
#define OTA_PRIMARY_HOST "do-update.top"
#endif

#ifndef OTA_PRIMARY_PORT
#define OTA_PRIMARY_PORT 50080
#endif

#ifndef OTA_PRIMARY_USE_TLS
#define OTA_PRIMARY_USE_TLS 0
#endif

#ifndef OTA_PRIMARY_HTTPS_PORT
#define OTA_PRIMARY_HTTPS_PORT 50443
#endif

#ifndef OTA_LAN_SCHEME
#define OTA_LAN_SCHEME "http"
#endif

#ifndef OTA_LAN_HOST
#define OTA_LAN_HOST "192.168.1.101"
#endif

#ifndef OTA_LAN_PORT
#define OTA_LAN_PORT 50080
#endif

namespace gateway::ota_config {

    static constexpr const char* kProject = FW_PROJECT_NAME;

    // 当前主 OTA 逻辑源。缓存 IP 只替换 TCP 连接地址，不替换该逻辑源。
    static constexpr const char* kPrimaryScheme = OTA_PRIMARY_SCHEME;
    static constexpr const char* kPrimaryHost = OTA_PRIMARY_HOST;
    static constexpr uint16_t kPrimaryPort = OTA_PRIMARY_PORT;
    static constexpr bool kPrimaryUseTls = OTA_PRIMARY_USE_TLS != 0;

    // 未来切换 HTTPS 时使用 50443；本次仍使用 HTTP 50080。
    static constexpr uint16_t kPrimaryHttpsPort = OTA_PRIMARY_HTTPS_PORT;

    // 局域网备用 OTA 端点。
    static constexpr const char* kLanScheme = OTA_LAN_SCHEME;
    static constexpr const char* kLanHost = OTA_LAN_HOST;
    static constexpr uint16_t kLanPort = OTA_LAN_PORT;

    static constexpr const char* kCheckPath = "/api/v1/ota/check";
    static constexpr const char* kFirmwareFileName = "firmware.bin";

    static constexpr uint32_t kDomainCacheVersion = 1;

    static constexpr uint32_t kFirstCheckDelayMs = 60UL * 1000UL;
    static constexpr uint32_t kCheckIntervalMs = 6UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t kManualCheckMinIntervalMs = 10UL * 1000UL;

    static constexpr uint32_t kHttpTimeoutMs = 15UL * 1000UL;
    static constexpr uint32_t kDownloadIdleTimeoutMs = 30UL * 1000UL;
    static constexpr uint32_t kRetryBackoffMinMs = 5UL * 60UL * 1000UL;
    static constexpr uint32_t kRetryBackoffMaxMs = 2UL * 60UL * 60UL * 1000UL;

    static constexpr size_t kManifestMaxBytes = 8192;
    static constexpr size_t kDownloadBufferSize = 4096;
    static constexpr uint32_t kOtaTaskStackBytes = 12288;
    static constexpr int kOtaTaskPinnedCore = 0;
    static constexpr uint32_t kOtaTaskLoopDelayMs = 250;

    // 未来启用 HTTPS 时替换为 do-update.top 对应 Root CA。
    // 当前 OTA_PRIMARY_USE_TLS=0，不使用该证书。
    static constexpr const char* kRootCaPem = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_DO_UPDATE_TOP_ROOT_CA
-----END CERTIFICATE-----
)EOF";

}  // namespace gateway::ota_config
