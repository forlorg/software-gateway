/**
 * @file can_traffic_stats.cpp
 * @brief CAN 与 MQTT 上行的计数、滑动窗口速率，供网页 `/api/live_state` 展示。
 */

#include "can_traffic_stats.h"

#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <freertos/portmacro.h>

namespace gateway::can_traffic_stats {

namespace {
std::atomic<uint64_t> g_rx_frames{};
std::atomic<uint64_t> g_rx_payload_bytes{};
std::atomic<uint64_t> g_uplink_bytes{};

/** 速率估算用的时间点快照。 */
struct Sample {
  uint32_t ms;
  uint64_t rx_frames;
  uint64_t rx_payload_bytes;
  uint64_t uplink_bytes;
};

constexpr int kHistCap = 24;
constexpr uint32_t kRateWindowMs = 5000;

static Sample g_hist[kHistCap];
static int g_hist_len = 0;
static portMUX_TYPE g_hist_mux = portMUX_INITIALIZER_UNLOCKED;

/** 丢弃超出 `kRateWindowMs` 的旧样本。 */
static void hist_prune_locked(uint32_t now_ms) {
  while (g_hist_len > 0 && now_ms - g_hist[0].ms > kRateWindowMs) {
    if (g_hist_len > 1) {
      memmove(&g_hist[0], &g_hist[1], static_cast<size_t>(g_hist_len - 1) * sizeof(Sample));
    }
    --g_hist_len;
  }
}

/** 根据窗口内首尾样本差分估算帧率与字节率。 */
static void compute_rates_locked(float *rx_fps, float *rx_bps, float *up_bps) {
  *rx_fps = *rx_bps = *up_bps = 0.f;
  if (g_hist_len < 2) {
    return;
  }
  const Sample &a = g_hist[0];
  const Sample &b = g_hist[g_hist_len - 1];
  const uint32_t dt = b.ms - a.ms;
  if (dt < 100) {
    return;
  }
  const float inv = 1000.f / static_cast<float>(dt);
  *rx_fps = static_cast<float>(b.rx_frames - a.rx_frames) * inv;
  *rx_bps = static_cast<float>(b.rx_payload_bytes - a.rx_payload_bytes) * inv;
  *up_bps = static_cast<float>(b.uplink_bytes - a.uplink_bytes) * inv;
}
} // namespace

void record_rx_frame(uint8_t dlc) {
  g_rx_frames.fetch_add(1, std::memory_order_relaxed);
  g_rx_payload_bytes.fetch_add(dlc, std::memory_order_relaxed);
}

void record_uplink_bytes(uint32_t n) {
  if (n == 0) {
    return;
  }
  g_uplink_bytes.fetch_add(n, std::memory_order_relaxed);
}

void on_aggregate_tick() {
  const uint32_t now = millis();
  const uint64_t rf = g_rx_frames.load(std::memory_order_relaxed);
  const uint64_t rb = g_rx_payload_bytes.load(std::memory_order_relaxed);
  const uint64_t ub = g_uplink_bytes.load(std::memory_order_relaxed);

  portENTER_CRITICAL(&g_hist_mux);
  hist_prune_locked(now);
  if (g_hist_len >= kHistCap) {
    memmove(&g_hist[0], &g_hist[1], static_cast<size_t>(kHistCap - 1) * sizeof(Sample));
    g_hist_len = kHistCap - 1;
  }
  g_hist[g_hist_len++] = {now, rf, rb, ub};
  hist_prune_locked(now);
  portEXIT_CRITICAL(&g_hist_mux);
}

WebSnapshot get_web_snapshot() {
  WebSnapshot out{};
  out.rx_frames = g_rx_frames.load(std::memory_order_relaxed);
  out.rx_payload_bytes = g_rx_payload_bytes.load(std::memory_order_relaxed);
  out.uplink_bytes = g_uplink_bytes.load(std::memory_order_relaxed);

  portENTER_CRITICAL(&g_hist_mux);
  compute_rates_locked(&out.rx_fps_5s, &out.rx_payload_bytes_s_5s, &out.uplink_bytes_s_5s);
  portEXIT_CRITICAL(&g_hist_mux);
  return out;
}

} // namespace gateway::can_traffic_stats
