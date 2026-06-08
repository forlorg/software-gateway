/**
 * @file uart2_test_task.cpp
 * @brief 双核 CPU 负载监控（硬件定时器 ISR 采样法）。
 *
 * **为什么用 ISR 采样？**
 * 任务级采样存在先天盲区——采样代码运行在哪颗核心，那颗核心就永远看到自己，
 * 导致负载率恒为 0%（或 100%）。改用硬件定时器 ISR 后，中断"闯入"正在执行
 * 的真实任务，读取的 `xTaskGetCurrentTaskHandleForCPU()` 才是被中断的任务，
 * 而非监控任务自身。
 *
 * **工作流：**
 * 1. 硬件定时器（Timer Group 0, Timer 0）每 1 ms 触发一次 ISR
 * 2. ISR 中读取两核的 `xTaskGetCurrentTaskHandleForCPU(cpuid)`，与空闲任务
 *    句柄对比，累加忙/总采样数到 `volatile` 计数器
 * 3. 监控任务每 1 s 关中断 → 快照并清零计数器 → 开中断 → 计算并打印负载率
 */

#include "uart2_test_task.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp32-hal-timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace gateway::uart2_test_task {

namespace {

static constexpr const char kLogTag[]    = "SYS_LOAD";
static constexpr TickType_t kPrintPeriod = pdMS_TO_TICKS(1000);
static constexpr int kTimerDivider       = 80;    // 80 MHz / 80 = 1 MHz → 1 μs/tick
static constexpr uint64_t kTimerAlarmUs  = 1000;  // 1 ms 周期 → 1000 Hz

// ---- ISR 与任务共享的计数器（volatile，ISR 只写，任务只读后清零）--------
static volatile uint32_t g_busy_samples[2]  = {};
static volatile uint32_t g_total_samples[2] = {};

// ---- 句柄缓存（启动后不变，ISR 安全读取）------------------------------
static TaskHandle_t g_idle_handles[2] = {};
static TaskHandle_t g_monitor_handle  = nullptr;

/**
 * @brief 硬件定时器 ISR。
 *
 * 每 1 ms 触发一次，读取各核心"被中断前"正在执行的任务，更新计数器。
 * ISR 中仅做简单的比较与递增，无任何阻塞调用。
 */
void IRAM_ATTR on_timer_isr() {
  for (int core = 0; core < 2; ++core) {
    TaskHandle_t current = xTaskGetCurrentTaskHandleForCPU(static_cast<BaseType_t>(core));
    if (current == nullptr) continue;

    ++g_total_samples[core];
    if (current != g_idle_handles[core] && current != g_monitor_handle) {
      ++g_busy_samples[core];
    }
  }
}

// ---- 定时器句柄（全局，便于 start/stop）--------------------------------
static hw_timer_t *g_timer = nullptr;

// ---- 辅助 ------------------------------------------------------------------

void print_percent(const char *label, uint32_t value_x100) {
  Serial.printf("%s%3lu.%02lu%%", label,
                static_cast<unsigned long>(value_x100 / 100),
                static_cast<unsigned long>(value_x100 % 100));
}

/**
 * @brief 每秒执行一次：快照 → 清零计数器 → 计算 & 打印负载率。
 */
void sample_and_print_load() {
  // ---- 关中断，快照并清零计数器 --------------------------------------
  uint32_t busy[2]  = {};
  uint32_t total[2] = {};

  portDISABLE_INTERRUPTS();
  for (int core = 0; core < 2; ++core) {
    busy[core]  = g_busy_samples[core];
    total[core] = g_total_samples[core];
    g_busy_samples[core]  = 0;
    g_total_samples[core] = 0;
  }
  portENABLE_INTERRUPTS();

  // ---- 计算负载率 ----------------------------------------------------
  Serial.printf("[%s] cpu load: ", kLogTag);
  for (int core = 0; core < 2; ++core) {
    if (core > 0) Serial.print("  ");
    char label[8];
    snprintf(label, sizeof(label), "core%d=", core);
    if (total[core] > 0) {
      uint32_t load_x100 =
          (static_cast<uint64_t>(busy[core]) * 10000ULL) / total[core];
      print_percent(label, load_x100);
    } else {
      Serial.printf("%sn/a", label);
    }
  }

  // ---- 堆统计 --------------------------------------------------------
  const UBaseType_t task_count       = uxTaskGetNumberOfTasks();
  const size_t      free_heap        = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t      min_free_heap    = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  const size_t      largest_free_blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  Serial.printf(" | tasks=%lu | heap=%u B min=%u B max_blk=%u B\n",
                static_cast<unsigned long>(task_count),
                static_cast<unsigned int>(free_heap),
                static_cast<unsigned int>(min_free_heap),
                static_cast<unsigned int>(largest_free_blk));
}

void task_monitor(void *) {
  g_monitor_handle = xTaskGetCurrentTaskHandle();

  // 缓存空闲任务句柄（启动后不变）
  g_idle_handles[0] = xTaskGetIdleTaskHandleForCPU(0);
  g_idle_handles[1] = xTaskGetIdleTaskHandleForCPU(1);

  // 启动硬件定时器 ISR
  g_timer = timerBegin(0, kTimerDivider, true);             // timer 0, 1 MHz
  timerAttachInterrupt(g_timer, &on_timer_isr, true);       // 边沿触发
  timerAlarmWrite(g_timer, kTimerAlarmUs, true);            // 1 ms, 自动重载
  timerAlarmEnable(g_timer);

  Serial.printf("[%s] ISR-based load monitor started, core=%d, rate=1000Hz\n",
                kLogTag, static_cast<int>(xPortGetCoreID()));

  for (;;) {
    vTaskDelay(kPrintPeriod);
    sample_and_print_load();
  }
}

} // namespace

void start() {
  Serial.printf("[%s] starting pinned core=%d stack=%lu\n", kLogTag, kPinnedCore,
                static_cast<unsigned long>(kTaskStackBytes));
  xTaskCreatePinnedToCore(task_monitor, "sys_load_mon", kTaskStackBytes, nullptr,
                          static_cast<UBaseType_t>(tskIDLE_PRIORITY + 1), nullptr,
                          static_cast<BaseType_t>(kPinnedCore));
}

} // namespace gateway::uart2_test_task
