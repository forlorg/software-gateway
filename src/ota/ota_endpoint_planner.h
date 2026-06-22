#pragma once

#include <Arduino.h>

#include <stdint.h>

#include "ota/ota_types.h"

namespace gateway::ota::detail {

    const char* endpoint_kind_string(EndpointKind kind);
    uint8_t endpoint_rank(EndpointKind kind);

    EndpointContext primary_endpoint(EndpointKind kind);
    EndpointContext lan_endpoint();

    String build_base_url(const EndpointContext& endpoint);
    String endpoint_host_header(const EndpointContext& endpoint);
    String expected_firmware_path(const String& version);
    String build_check_url(const EndpointContext& endpoint);

    CandidatePlan build_candidate_plan(uint8_t minimum_rank);

    bool prepare_endpoint(EndpointContext* endpoint, String* error_code);
    bool save_domain_cache(const EndpointContext& endpoint);
    void handle_candidate_failure(const EndpointContext& endpoint, const char* stage);

}  // namespace gateway::ota::detail
