#pragma once

/**
 * @file http_server_task.h
 * @brief HTTP 服务任务栈、绑核与轮询周期配置。
 */

#include <cstdint>

namespace gateway::http_server_task {

constexpr uint32_t kHeapWarnMinFreeBytes = 30 * 1024;

constexpr uint32_t kTaskStackBytes = 8192;

/** 同步 WebServer::handleClient() 所在 CPU 核。 */
constexpr int kPinnedCpuCoreIndex = 0;

constexpr uint32_t kLoopDelayMs = 40;
constexpr uint32_t kHeapCheckIntervalMs = 2000;

void start();

} // namespace gateway::http_server_task
