#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <stddef.h>
#include <stdint.h>

namespace gateway::ota::detail {

    enum class EndpointKind : uint8_t {
        CachedDomainIp = 0,
        Domain = 1,
        LanFallback = 2
    };

    struct EndpointContext {
        EndpointKind kind = EndpointKind::Domain;
        String scheme;
        String logical_host;
        uint16_t port = 0;
        bool use_tls = false;
        IPAddress transport_ip;
        bool transport_ip_valid = false;
    };

    struct CandidatePlan {
        static constexpr size_t kMaxEndpoints = 3;

        EndpointContext endpoints[kMaxEndpoints];
        size_t count = 0;
    };

    struct OtaManifest {
        bool update_available = false;
        String project;
        String hw;
        String channel;
        String version;
        uint32_t build = 0;
        bool mandatory = false;
        String firmware_url;
        String firmware_path;
        size_t size = 0;
        String md5;
        String sha256;
        String release_notes;
        EndpointContext source;
        bool source_valid = false;
    };

}  // namespace gateway::ota::detail
