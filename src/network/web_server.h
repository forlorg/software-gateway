#pragma once

/**
 * @file web_server.h
 * @brief HTTP 服务端口、启动与周期性 `handleClient` 轮询声明。
 */

#include <cstdint>

namespace gateway::web_server {

constexpr uint16_t kHttpListenPort = 80;

void start();

/** 每圈调用一次 `handleClient()`（仅用于同步 WebServer）。 */
void poll();

/** 刷新供 `GET /api/live_state` 返回的全局快照（原 SSE 载荷，改为轮询）。 */
void sse_tick();

} // namespace gateway::web_server
