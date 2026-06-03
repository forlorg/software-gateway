/**
 * @file sse_manager.cpp
 * @brief SSE 兼容层：委托 `web_server::sse_tick` 刷新实时快照。
 */

#include "sse_manager.h"

#include "network/web_server.h"

namespace gateway::sse_manager {

void tick() { gateway::web_server::sse_tick(); }

} // namespace gateway::sse_manager
