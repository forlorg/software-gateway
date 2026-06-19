/**
 * @file mqtt_manager.cpp
 * @brief MQTT 客户端：主题拼装、连接退避、下行 AT 解析与 CAN 下发。
 *
 * **Core1 代理连接**：PubSubClient::connect() 内部 lwIP DNS/TCP 可能阻塞 >12s，
 * 远超 ESP-IDF 默认 5s 任务看门狗。因此将阻塞的 connect 委托到 Core1 执行，
 * Core0 通过 semaphore 等待（xSemaphoreTake 可被调度，IDLE0 得到喂养）。
 */

#include "mqtt_manager.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "system/gateway_context.h"
#include "can/can_traffic_stats.h"
#include "can/can_tx.h"
#include "config/config_store.h"
#include "protocol/at_protocol.h"
#include "system/state_machine.h"
#include "system/statistics.h"

namespace gateway::mqtt_manager {

    namespace {

        WiFiClient g_tcp;
        PubSubClient g_mqtt(g_tcp);

        char g_host[80];
        uint16_t g_port{1883};
        char g_user[48];
        char g_pass[48];

        char g_topic_up[96];
        char g_topic_dn[96];
        char g_topic_will[96];
        char g_hex[9];

        bool g_topics_ready{false};
        volatile bool g_mqtt_connected{false};
        volatile bool g_had_success_connect{false};
        volatile bool g_mqtt_reconfig_pending{false};

        uint32_t g_backoff_ms{1000};
        uint32_t g_next_attempt_ms{0};

        // ---- 重连风暴检测 -------------------------------------------------------
        uint32_t g_recent_reconnect_times[5]{};
        uint8_t  g_recent_reconnect_idx{0};
        constexpr uint32_t kStormWindowMs = 15000;
        constexpr uint32_t kStormMinBackoffMs = 10000;

        bool is_reconnect_storm() {
            const uint32_t now = millis();
            uint8_t count = 0;
            for (unsigned i = 0; i < 5; ++i) {
                if (g_recent_reconnect_times[i] != 0
                    && now - g_recent_reconnect_times[i] < kStormWindowMs) {
                    ++count;
                }
            }
            return count >= 4;
        }

        void record_reconnect_event() {
            g_recent_reconnect_times[g_recent_reconnect_idx % 5] = millis();
            ++g_recent_reconnect_idx;
        }

        // ---- Core1 代理连接：避免阻塞 Core0 导致 WDT ---------------------------
        SemaphoreHandle_t g_connect_done_sem = nullptr;

        struct ConnectParams {
            char client_id[28];
            char user[48];
            char pass[48];
            bool use_will;
        };
        ConnectParams g_cp{};
        bool          g_connect_result_ok = false;
        int           g_connect_result_rc = -4;
        TaskHandle_t  g_connect_task = nullptr;

        struct PendingPublish {
            uint16_t len;
            uint8_t data[config::kPendingPublishMaxBytes];
        };
        QueueHandle_t g_publish_q{};
        SemaphoreHandle_t g_publish_enqueue_mu{};
        PendingPublish g_publish_enqueue_item{};
        PendingPublish g_publish_drop_item{};
        PendingPublish g_publish_drain_item{};

        void ensure_publish_queue() {
            if (!g_publish_q) {
                g_publish_q = xQueueCreate(
                    static_cast<UBaseType_t>(config::kPendingPublishQueueDepth),
                    sizeof(PendingPublish));
            }
            if (!g_publish_enqueue_mu) {
                g_publish_enqueue_mu = xSemaphoreCreateMutex();
            }
        }

        bool enqueue_pending_publish(const uint8_t *data, size_t len) {
            if (!data || len == 0 || len > config::kPendingPublishMaxBytes) {
                return false;
            }
            ensure_publish_queue();
            if (!g_publish_q) {
                return false;
            }

            if (!g_publish_enqueue_mu ||
                xSemaphoreTake(g_publish_enqueue_mu, pdMS_TO_TICKS(5)) != pdTRUE) {
                return false;
            }

            g_publish_enqueue_item.len = static_cast<uint16_t>(len);
            memcpy(g_publish_enqueue_item.data, data, len);

            if (xQueueSend(g_publish_q, &g_publish_enqueue_item, 0) == pdTRUE) {
                xSemaphoreGive(g_publish_enqueue_mu);
                return true;
            }

            // 发布队列满：丢旧保新。MQTT 是外部链路，不允许反压 CAN/USB 主链路。
            if (xQueueReceive(g_publish_q, &g_publish_drop_item, 0) == pdTRUE) {
                statistics::add_mqtt_publish_queue_drops(1);
            }
            if (xQueueSend(g_publish_q, &g_publish_enqueue_item, 0) == pdTRUE) {
                xSemaphoreGive(g_publish_enqueue_mu);
                return true;
            }

            xSemaphoreGive(g_publish_enqueue_mu);
            statistics::add_mqtt_publish_queue_drops(1);
            return false;
        }

        bool publish_blob_locked(const char *topic_override, const uint8_t *data, size_t len) {
            if (!data || len == 0 || !g_mqtt_connected || !g_mqtt.connected()) {
                return false;
            }
            const char *t = topic_override ? topic_override : g_topic_up;
            if (!t || !t[0]) {
                return false;
            }
            const bool ok = g_mqtt.publish(t, data, static_cast<unsigned int>(len), false);
            if (!ok) {
                return false;
            }
            statistics::add_mqtt_tx(1);
            return true;
        }

        void drain_publish_queue_locked() {
            if (!g_publish_q || !g_mqtt_connected || !g_mqtt.connected()) {
                return;
            }

            uint8_t sent_count = 0;
            constexpr uint8_t kMaxPublishesPerLoop = 4;
            while (sent_count < kMaxPublishesPerLoop &&
                   xQueueReceive(g_publish_q, &g_publish_drain_item, 0) == pdTRUE) {
                if (!publish_blob_locked(nullptr, g_publish_drain_item.data, g_publish_drain_item.len)) {
                    statistics::add_mqtt_publish_queue_drops(1);
                    Serial.printf("[MQTT] publish failed; dropped pending upload len=%u\r\n",
                        static_cast<unsigned>(g_publish_drain_item.len));
                    g_mqtt_connected = false;
                    g_tcp.stop();
                    record_reconnect_event();
                    g_next_attempt_ms = millis() + g_backoff_ms + (ESP.getCycleCount() % 500);
                    if (g_backoff_ms < 60000) {
                        g_backoff_ms *= 2;
                    }
                    return;
                }
                can_traffic_stats::record_uplink_bytes(static_cast<uint32_t>(g_publish_drain_item.len));
                ++sent_count;
            }
        }

        /** Core1 任务：执行阻塞的 g_mqtt.connect()，完成后给信号量并自删除。 */
        void mqtt_connect_task_fn(void *) {
            g_connect_result_ok = g_cp.use_will
                ? g_mqtt.connect(g_cp.client_id, g_cp.user, g_cp.pass,
                                 g_topic_will, 0, true, "offline", true)
                : g_mqtt.connect(g_cp.client_id, g_cp.user, g_cp.pass);
            g_connect_result_rc = g_mqtt.state();
            xSemaphoreGive(g_connect_done_sem);
            g_connect_task = nullptr;
            vTaskDelete(nullptr);
        }

        void ensure_connect_sem() {
            if (!g_connect_done_sem) {
                g_connect_done_sem = xSemaphoreCreateBinary();
            }
        }

        // ---- 主题构建 -----------------------------------------------------------

        bool build_topics_locked() {
            if (!ctx::has_product_id()) {
                return false;
            }
            ctx::get_product_id_hex(g_hex);
            if (!g_hex[0]) {
                return false;
            }
            snprintf(g_topic_up, sizeof(g_topic_up), "topic_ps_pro/vehicle_upload/%s", g_hex);
            snprintf(g_topic_dn, sizeof(g_topic_dn), "topic_ps_pro/vehicle_download/%s", g_hex);
            snprintf(g_topic_will, sizeof(g_topic_will), "topic_ps_pro/will/%s", g_hex);
            g_topics_ready = true;
            return true;
        }

        void copy_creds_from_store() {
            String h;
            uint16_t p = 1883;
            String u;
            String pw;
            if (config_store::mqtt_get(h, p, u, pw) && h.length() > 0) {
                snprintf(g_host, sizeof(g_host), "%s", h.c_str());
                g_port = p;
                snprintf(g_user, sizeof(g_user), "%s", u.c_str());
                snprintf(g_pass, sizeof(g_pass), "%s", pw.c_str());
            } else {
                snprintf(g_host, sizeof(g_host), "%s", "broker.hivemq.com");
                g_port = 1883;
                g_user[0] = '\0';
                g_pass[0] = '\0';
            }
            g_mqtt.setServer(g_host, g_port);
        }

        void on_mqtt_download(const uint8_t *blob, size_t len) {
            if (!blob || len == 0) {
                return;
            }
            uint8_t work[4096];
            if (len > sizeof(work)) {
                statistics::add_mqtt_rx(1);
                return;
            }
            memcpy(work, blob, len);
            size_t wlen = len;
            at_protocol::consume_at_buffer(
                work, &wlen, nullptr,
                [](void *, const twai_message_t &tm) {
                    if (!can_tx::enqueue_cloud_command(tm)) {
                        Serial.printf("[MQTT dn] CAN TX normal queue full; dropped cloud command\r\n");
                    }
                });
            statistics::add_mqtt_rx(1);
        }

        void mqtt_subscribe_cb(char *topic, byte *payload, unsigned int len) {
            if (payload && len > 0) {
                const unsigned kPreview = 48;
                const unsigned n = static_cast<unsigned>(len) < kPreview
                ? static_cast<unsigned>(len)
                : kPreview;
                Serial.print("[MQTT dn] payload(hex): ");
                for (unsigned i = 0; i < n; ++i) {
                    Serial.printf("%02x", payload[i]);
                }
                if (static_cast<unsigned>(len) > n) {
                    Serial.print("...");
                }
                Serial.print("\r\n");
            }
            on_mqtt_download(reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(len));
        }

    } // namespace

    void init() {
        ensure_publish_queue();
        g_mqtt.setBufferSize(config::kPubSubBufferBytes);
        g_mqtt.setCallback(mqtt_subscribe_cb);
        copy_creds_from_store();
        g_tcp.setTimeout(8000);
    }

    void apply_config_from_store() { g_mqtt_reconfig_pending = true; }

    void notify_product_id_changed() { g_mqtt_reconfig_pending = true; }

    void loop_poll() {
        if (g_mqtt_reconfig_pending) {
            g_mqtt_reconfig_pending = false;
            if (g_mqtt.connected()) {
                g_mqtt.disconnect();
            }
            g_mqtt_connected = false;
            g_tcp.stop();
            copy_creds_from_store();
        }

        if (!WiFi.isConnected()) {
            if (g_mqtt_connected) {
                g_mqtt_connected = false;
                g_tcp.stop();
                state_machine::set_state(state_machine::SystemState::MqttLost);
            }
            return;
        }
        if (!ctx::has_product_id()) {
            return;
        }
        if (!build_topics_locked()) {
            return;
        }

        if (g_mqtt.connected()) {
            g_mqtt_connected = true;
            const uint32_t t_mqtt_loop = millis();
            const bool loop_ok = g_mqtt.loop();
            const uint32_t t_mqtt_loop_elapsed = millis() - t_mqtt_loop;

            if (!loop_ok) {
                Serial.printf("[MQTT] loop() returned disconnected; closing TCP socket\r\n");
                g_mqtt_connected = false;
                g_tcp.stop();
                vTaskDelay(pdMS_TO_TICKS(10));
                state_machine::set_state(state_machine::SystemState::MqttLost);
                record_reconnect_event();
                if (is_reconnect_storm()) {
                    g_backoff_ms = kStormMinBackoffMs;
                    Serial.printf("[MQTT] STORM DETECTED forcing backoff=%lums\r\n",
                        static_cast<unsigned long>(g_backoff_ms));
                }
                g_next_attempt_ms = millis() + g_backoff_ms + (ESP.getCycleCount() % 500);
                if (g_backoff_ms < 60000) {
                    g_backoff_ms *= 2;
                }
                return;
            }

            state_machine::set_state(state_machine::SystemState::MqttReady);
            drain_publish_queue_locked();
            if (t_mqtt_loop_elapsed > 500) {
                Serial.printf("[MQTT] loop() SLOW elapsed=%lums\r\n",
                    static_cast<unsigned long>(t_mqtt_loop_elapsed));
            }
            return;
        }

        if (g_mqtt_connected) {
            g_mqtt_connected = false;
            g_tcp.stop();
            vTaskDelay(pdMS_TO_TICKS(10));
            state_machine::set_state(state_machine::SystemState::MqttLost);
            record_reconnect_event();
            if (is_reconnect_storm()) {
                g_backoff_ms = kStormMinBackoffMs;
                Serial.printf("[MQTT] STORM DETECTED forcing backoff=%lums\r\n",
                    static_cast<unsigned long>(g_backoff_ms));
            }
            g_next_attempt_ms = millis() + g_backoff_ms + (ESP.getCycleCount() % 500);
            if (g_backoff_ms < 60000) {
                g_backoff_ms *= 2;
            }
            Serial.printf("[MQTT] disconnected; backoff=%lums\r\n",
                static_cast<unsigned long>(g_backoff_ms));
        }

        const uint32_t now = millis();
        if (now < g_next_attempt_ms) {
            state_machine::set_state(state_machine::SystemState::MqttConnecting);
            return;
        }

        state_machine::set_state(state_machine::SystemState::MqttConnecting);

        // ---- 重连前强制清理 TCP 状态 -------------------------------------------
        g_tcp.stop();
        vTaskDelay(pdMS_TO_TICKS(20));

        // ---- 预连接诊断 --------------------------------------------------------
        const wl_status_t wifi_st = WiFi.status();
        const int rssi = (wifi_st == WL_CONNECTED) ? WiFi.RSSI() : -100;
        const uint32_t free_heap = ESP.getFreeHeap();
        const UBaseType_t stack_watermark = uxTaskGetStackHighWaterMark(nullptr);

        Serial.printf("[MQTT] connect %s:%u (PubSubClient) client=pspro-%s\r\n", g_host,
            static_cast<unsigned>(g_port), g_hex);
        Serial.printf("[MQTT] pre_connect diag: wifi=%d rssi=%d heap=%lu stack_free=%lu "
            "backoff=%lu\r\n",
            static_cast<int>(wifi_st), rssi,
            static_cast<unsigned long>(free_heap),
            static_cast<unsigned long>(stack_watermark),
            static_cast<unsigned long>(g_backoff_ms));

        // ---- 委托 Core1 执行阻塞的 MQTT connect -------------------------------
        // 原理：g_mqtt.connect() → WiFiClient::connect() → lwIP DNS/TCP SYN 重传
        // 在 Core0 可阻塞 >12s（超 5s WDT 阈值）。Core1 执行期间，NET_TASK 通过
        // xSemaphoreTake 阻塞 → IDLE0 被调度 → WDT 得到喂养。
        ensure_connect_sem();

        snprintf(g_cp.client_id, sizeof(g_cp.client_id), "pspro-%s", g_hex);
        snprintf(g_cp.user, sizeof(g_cp.user), "%s", g_user[0] ? g_user : "");
        snprintf(g_cp.pass, sizeof(g_cp.pass), "%s", g_pass[0] ? g_pass : "");
        g_cp.use_will = g_topics_ready;
        g_connect_result_ok = false;
        g_connect_result_rc = -4;

        BaseType_t created = xTaskCreatePinnedToCore(
            mqtt_connect_task_fn, "mqtt_conn", 6144, nullptr,
            static_cast<UBaseType_t>(tskIDLE_PRIORITY + 0),
            &g_connect_task, static_cast<BaseType_t>(1));

        if (created != pdPASS) {
            Serial.printf("[MQTT] FAILED to create Core1 connect task\r\n");
            g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
            return;
        }

        const uint32_t t_before_connect = millis();
        constexpr uint32_t kConnectHardTimeoutMs = 18000;
        bool timed_out = true;

        // 每 50ms 轮询 semaphore（阻塞 → IDLE0 可运行喂狗）
        while (millis() - t_before_connect < kConnectHardTimeoutMs) {
            if (xSemaphoreTake(g_connect_done_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
                timed_out = false;
                break;
            }
        }

        const uint32_t t_connect_elapsed = millis() - t_before_connect;

        if (timed_out) {
            Serial.printf("[MQTT] CONNECT TIMEOUT after %lums; killing Core1 task\r\n",
                static_cast<unsigned long>(t_connect_elapsed));
            if (g_connect_task) {
                vTaskDelete(g_connect_task);
                g_connect_task = nullptr;
            }
            g_tcp.stop();
            vTaskDelay(pdMS_TO_TICKS(20));
            g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
            if (g_backoff_ms < 60000) {
                g_backoff_ms *= 2;
            }
            return;
        }

        const bool ok = g_connect_result_ok;
        const int  rc = g_connect_result_rc;

        Serial.printf("[MQTT] post_connect: ok=%d rc=%d elapsed_ms=%lu\r\n",
            static_cast<int>(ok), rc,
            static_cast<unsigned long>(t_connect_elapsed));

        if (!ok) {
            Serial.printf("[MQTT] connect failed rc=%d (see PubSubClient MQTT_*)\r\n", rc);
            g_tcp.stop();
            vTaskDelay(pdMS_TO_TICKS(10));
            g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
            if (g_backoff_ms < 60000) {
                g_backoff_ms *= 2;
            }
            return;
        }

        // ---- subscribe 在 Core0 执行（用时短，不会触发 WDT）--------------------
        if (!g_mqtt.subscribe(g_topic_dn, 1)) {
            Serial.printf("[MQTT] subscribe failed\r\n");
            g_mqtt.disconnect();
            g_tcp.stop();
            vTaskDelay(pdMS_TO_TICKS(10));
            g_next_attempt_ms = now + g_backoff_ms + (ESP.getCycleCount() % 500);
            if (g_backoff_ms < 60000) {
                g_backoff_ms *= 2;
            }
            return;
        }

        g_mqtt_connected = true;
        g_had_success_connect = true;
        g_backoff_ms = 1000;
        g_next_attempt_ms = 0;
        state_machine::set_state(state_machine::SystemState::MqttReady);
        Serial.printf("[MQTT] connected; subscribe topic=%s\r\n", g_topic_dn);
    }

    bool is_connected() { return g_mqtt_connected; }

    bool had_successful_connection() { return g_had_success_connect; }

    bool enqueue_vehicle_upload(const uint8_t *data, size_t len) {
        return enqueue_pending_publish(data, len);
    }

    bool publish_vehicle_upload(const uint8_t *data, size_t len) {
        return enqueue_vehicle_upload(data, len);
    }

    bool publish_blob(const char *topic_override, const uint8_t *data, size_t len) {
        return publish_blob_locked(topic_override, data, len);
    }

    const char *vehicle_upload_topic() {
        return (g_topics_ready && g_topic_up[0]) ? g_topic_up : "";
    }

    const char *vehicle_download_topic() {
        return (g_topics_ready && g_topic_dn[0]) ? g_topic_dn : "";
    }

} // namespace gateway::mqtt_manager
