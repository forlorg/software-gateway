#pragma once

#include <Arduino.h>

#include <stddef.h>

#include "ota/ota_types.h"

namespace gateway::ota::detail {

    size_t ota_partition_size();

    String firmware_url_from_manifest(const OtaManifest& manifest,
                                      const EndpointContext& endpoint);
    bool is_allowed_firmware_url(const String& url,
                                 const String& version,
                                 const EndpointContext& endpoint);

    bool fetch_manifest(const EndpointContext& endpoint,
                        OtaManifest* output,
                        String* error_code);

}  // namespace gateway::ota::detail
