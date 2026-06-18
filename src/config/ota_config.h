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

#ifndef OTA_API_SCHEME
#define OTA_API_SCHEME "http"
#endif

#ifndef OTA_API_HOST
#define OTA_API_HOST "192.168.1.101"
#endif

#ifndef OTA_API_PORT
#define OTA_API_PORT 50080
#endif

#ifndef OTA_API_USE_TLS
#define OTA_API_USE_TLS 0
#endif

namespace gateway::ota_config {

static constexpr const char* kProject = FW_PROJECT_NAME;
static constexpr const char* kApiScheme = OTA_API_SCHEME;
static constexpr const char* kApiHost = OTA_API_HOST;
static constexpr uint16_t kApiPort = OTA_API_PORT;
static constexpr bool kUseTls = OTA_API_USE_TLS != 0;

static constexpr const char* kCheckPath = "/api/v1/ota/check";
static constexpr const char* kFirmwareFileName = "firmware.bin";

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

// 生产环境替换为 do-update.top 对应 Root CA。
// 测试环境 OTA_API_USE_TLS=0 时不会使用。
static constexpr const char* kRootCaPem = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_DO_UPDATE_TOP_ROOT_CA
-----END CERTIFICATE-----
)EOF";

}  // namespace gateway::ota_config
