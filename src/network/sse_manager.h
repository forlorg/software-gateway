#pragma once

/**
 * @file sse_manager.h
 * @brief 实时推送 tick 入口（现与 HTTP 轮询共用快照逻辑）。
 */

namespace gateway::sse_manager {

void tick();

} // namespace gateway::sse_manager
