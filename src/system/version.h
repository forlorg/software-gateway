#pragma once

#include <stdint.h>

namespace gateway::version {

const char* project_name();
const char* firmware_version();
uint32_t firmware_build();
const char* hardware();
const char* channel();
const char* build_time();

}  // namespace gateway::version
