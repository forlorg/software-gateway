/**
 * @file web_server.cpp
 * @brief 同步 WebServer：页面资源、分页 REST API、MQTT/WiFi 配置与调试 live_state JSON。
 */

#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <cstring>

#include "can/can_parsed_data.h"
#include "can/can_traffic_stats.h"
#include "can/can_tx.h"
#include "config/config_store.h"
#include "network/mqtt_manager.h"
#include "network/provision_fsm.h"
#include "network/web_assets.h"
#include "network/wifi_manager.h"
#include "system/gateway_context.h"
#include "system/state_machine.h"
#include "system/statistics.h"
#include "system/time_sync.h"
#include "system/version.h"
#include "system/ota_manager.h"
#include "task/mqtt_uplink.h"
#include "transport/at_frame_dispatcher.h"

namespace gateway::web_server {

    namespace {

        WebServer g_http(kHttpListenPort);

        char g_live_state_json[2560]{};

        void write_common_status(JsonDocument &doc) {
            const unsigned long ms = static_cast<unsigned long>(millis());
            doc["uptime_ms"] = ms;
            doc["mono_ms"] = ms;
            doc["epoch_sec"] = gateway::time_sync::utc_epoch_seconds();
            char wall_buf[52]{};
            gateway::time_sync::format_wall_time_iso8601(wall_buf, sizeof(wall_buf));
            doc["wall_time"] = wall_buf;
            doc["ntp_synced"] = gateway::time_sync::ntp_has_sync();
            doc["timezone_ready"] = gateway::time_sync::timezone_is_ready();
            doc["timezone_offset_hours"] = gateway::time_sync::timezone_offset_hours();
            doc["timezone_bits"] = gateway::time_sync::timezone_bits();
            doc["timezone_source"] = gateway::time_sync::timezone_source();
            doc["timestamp_wall_ready"] = gateway::time_sync::can_use_wall_timestamp_for_upload();
            doc["system_state"] = gateway::state_machine::system_state_str(gateway::state_machine::current());
        }

        void write_wifi_status(JsonDocument &doc) {
            const bool linked = gateway::wifi_manager::sta_is_linked();
            doc["sta"] = linked ? "connected" : "disconnected";
            doc["sta_rssi"] = gateway::wifi_manager::sta_rssi_cached();
            char ip[28]{};
            gateway::wifi_manager::sta_local_ip_str(ip, sizeof(ip));
            doc["sta_ip"] = ip;
            char cssid[40]{};
            gateway::wifi_manager::sta_current_ssid(cssid, sizeof(cssid));
            doc["sta_ssid"] = cssid;
            String s;
            String p;
            if (!linked && gateway::config_store::wifi_get(s, p) && s.length() > 0) {
                doc["saved_ssid"] = s;
            } else {
                doc["saved_ssid"] = "";
            }
        }

        void write_can_traffic(JsonObject doc) {
            const auto cs = gateway::can_traffic_stats::get_web_snapshot();
            doc["rx_frames"] = cs.rx_frames;
            doc["rx_payload_bytes"] = cs.rx_payload_bytes;
            doc["uplink_bytes"] = cs.uplink_bytes;
            doc["rx_fps_5s"] = cs.rx_fps_5s;
            doc["rx_payload_bytes_s_5s"] = cs.rx_payload_bytes_s_5s;
            doc["uplink_bytes_s_5s"] = cs.uplink_bytes_s_5s;
            char bus_pid[9]{};
            if (gateway::ctx::has_product_id()) {
                gateway::ctx::get_product_id_hex(bus_pid);
            }
            doc["bus_product_id_hex"] = bus_pid;
        }

        void write_cloud_stats(JsonObject doc) {
            doc["mqtt_connected"] = gateway::mqtt_manager::is_connected();
            doc["mqtt_queue_bytes"] = static_cast<uint32_t>(gateway::mqtt_uplink::ring_used_bytes());
            doc["can_tx_frames"] = gateway::statistics::can_tx();
            doc["mqtt_tx_msgs"] = gateway::statistics::mqtt_tx();
            doc["mqtt_rx_msgs"] = gateway::statistics::mqtt_rx();
            doc["dropped_lines"] = gateway::statistics::dropped();
            doc["serial_mirror_queue_drops"] = gateway::statistics::serial_mirror_queue_drops();
            doc["serial_mirror_queue_depth"] = gateway::at_frame_dispatcher::kSerialMirrorQueueDepth;
            doc["can_tx_queue_drops"] = gateway::statistics::can_tx_queue_drops();
            doc["can_tx_high_queue_drops"] = gateway::statistics::can_tx_high_queue_drops();
            doc["can_tx_transmit_failures"] = gateway::statistics::can_tx_transmit_failures();
            doc["mqtt_uplink_queue_drops"] = gateway::statistics::mqtt_uplink_queue_drops();
            doc["mqtt_publish_queue_drops"] = gateway::statistics::mqtt_publish_queue_drops();
            doc["mqtt_downlink_can_drops"] = gateway::statistics::mqtt_downlink_can_drops();
            doc["at_dispatch_frames"] = gateway::statistics::at_dispatch_frames();
            doc["at_encode_failures"] = gateway::statistics::at_encode_failures();
        }

        void refresh_live_json_snapshot() {
            JsonDocument doc;
            write_common_status(doc);
            write_wifi_status(doc);
            {
                JsonDocument can_doc;
                auto can = can_doc.to<JsonObject>();
                write_can_traffic(can);
                doc["can_rx_frames"] = can["rx_frames"];
                doc["can_rx_payload_bytes"] = can["rx_payload_bytes"];
                doc["can_uplink_bytes"] = can["uplink_bytes"];
                doc["can_rx_fps_5s"] = can["rx_fps_5s"];
                doc["can_rx_payload_bytes_s_5s"] = can["rx_payload_bytes_s_5s"];
                doc["can_uplink_bytes_s_5s"] = can["uplink_bytes_s_5s"];
                doc["bus_product_id_hex"] = can["bus_product_id_hex"];
            }
            write_cloud_stats(doc.as<JsonObject>());
            (void)serializeJson(doc, g_live_state_json, sizeof(g_live_state_json));
        }

        void send_json_doc(JsonDocument &doc, char *buf, size_t cap) {
            const size_t n = serializeJson(doc, buf, cap);
            if (n >= cap) {
                g_http.send(500, "application/json; charset=utf-8", "{\"error\":\"json_truncated\"}");
                return;
            }
            g_http.send(200, "application/json; charset=utf-8", buf);
        }

        void handle_page_system_status() {
            JsonDocument doc;
            // write_common_status(doc);

            const auto sys = gateway::can_parsed_data::get_obj_system_status();
            const auto drive = gateway::can_parsed_data::get_obj_drive();
            const auto pto = gateway::can_parsed_data::get_obj_pto();

            auto groups = doc["groups"].to<JsonArray>();

            auto sys_group = groups.add<JsonObject>();
            sys_group["name"] = "系统状态";
            auto sys_items = sys_group["items"].to<JsonArray>();
            {
                auto item = sys_items.add<JsonObject>();
                item["name"] = "状态机";
                item["value"] = sys.valid ? sys.state_machine : "-";
            }
            {
                auto item = sys_items.add<JsonObject>();
                item["name"] = "系统压力";
                item["value"] = sys.valid ? sys.system_pressure : "-";
            }
            {
                auto item = sys_items.add<JsonObject>();
                item["name"] = "系统运行时间";
                item["value"] = sys.valid ? sys.system_uptime : "-";
            }

            auto drive_group = groups.add<JsonObject>();
            drive_group["name"] = "动力换挡状态";
            auto drive_items = drive_group["items"].to<JsonArray>();
            {
                auto item = drive_items.add<JsonObject>();
                item["name"] = "动力换挡";
                item["value"] = drive.valid ? drive.power_shuttle_state : "-";
            }
            {
                auto item = drive_items.add<JsonObject>();
                item["name"] = "离合器打滑率";
                item["value"] = drive.valid ? drive.clutch_padel_slip : "-";
            }
            {
                auto item = drive_items.add<JsonObject>();
                item["name"] = "换向控制锁";
                item["value"] = drive.valid ? drive.drive_lock : "-";
            }
            {
                auto item = drive_items.add<JsonObject>();
                item["name"] = "动力换挡电磁阀";
                item["value"] = drive.valid ? drive.drive_solenoid : "-";
            }
            {
                auto item = drive_items.add<JsonObject>();
                item["name"] = "动力换挡压力";
                item["value"] = drive.valid ? drive.drive_pressure : "-";
            }

            auto pto_group = groups.add<JsonObject>();
            pto_group["name"] = "PTO 状态";
            auto pto_items = pto_group["items"].to<JsonArray>();
            {
                auto item = pto_items.add<JsonObject>();
                item["name"] = "PTO 开关";
                item["value"] = pto.valid ? pto.pto_switch : "-";
            }
            {
                auto item = pto_items.add<JsonObject>();
                item["name"] = "PTO 锁";
                item["value"] = pto.valid ? pto.pto_lock : "-";
            }
            {
                auto item = pto_items.add<JsonObject>();
                item["name"] = "PTO 电磁阀";
                item["value"] = pto.valid ? pto.pto_solenoid : "-";
            }
            {
                auto item = pto_items.add<JsonObject>();
                item["name"] = "PTO 压力";
                item["value"] = pto.valid ? pto.pto_pressure : "-";
            }

            char stack[2048];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_page_calibration() {
            JsonDocument doc;
            // write_common_status(doc);

            const auto clutch = gateway::can_parsed_data::get_obj_clutch_startup();

            auto clutches = doc["clutches"].to<JsonArray>();

            auto mk = [&](const char *id, const char *name, const String &current) {
                auto c = clutches.add<JsonObject>();
                c["id"] = id;
                c["name"] = name;
                c["current"] = current;
            };

            mk("high", "高档离合器标定", clutch.high_clutch_startup);
            mk("low",  "低档离合器标定", clutch.low_clutch_startup);
            mk("rev",  "倒档离合器标定", clutch.rev_clutch_startup);
            mk("pto",  "PTO离合器标定",  clutch.pto_clutch_startup);

            char stack[1280];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_page_traffic_stats() {
            JsonDocument doc;
            auto can = doc["can"].to<JsonObject>();
            write_can_traffic(can);
            auto cloud = doc["cloud"].to<JsonObject>();
            write_cloud_stats(cloud);
            char stack[2048];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_page_provision() {
            JsonDocument doc;
            write_common_status(doc);
            write_wifi_status(doc);
            doc["mqtt_connected"] = gateway::mqtt_manager::is_connected();
            char stack[1280];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_calibration_submit() {
            if (!g_http.hasArg("plain")) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, g_http.arg("plain"))) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"json\"}");
                return;
            }

            const char *clutch = doc["clutch"] | "";
            const char *value  = doc["value"]  | "";

            if (!clutch || !clutch[0]) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"clutch required\"}");
                return;
            }
            if (!value || !value[0]) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"value required\"}");
                return;
            }

            // 校验离合器名称
            if (std::strcmp(clutch, "high") != 0 &&
                std::strcmp(clutch, "low")  != 0 &&
                std::strcmp(clutch, "rev")  != 0 &&
                std::strcmp(clutch, "pto")  != 0) {
                g_http.send(400, "application/json; charset=utf-8",
                    "{\"ok\":false,\"error\":\"unknown clutch\"}");
                return;
            }

            const bool sent = gateway::can_tx::send_clutch_startup(clutch, value);
            if (!sent) {
                g_http.send(400, "application/json; charset=utf-8",
                    "{\"ok\":false,\"error\":\"value out of range or send failed\"}");
                return;
            }

            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
        }

        // OTA guard forward declarations (defined later in OTA API injection block)
        bool reject_if_ota_in_progress();

        void handle_wifi_connect() {
            if (reject_if_ota_in_progress()) { return; }

            if (!g_http.hasArg("plain")) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, g_http.arg("plain"))) {
                g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
                return;
            }
            const char *ssid = doc["ssid"] | "";
            const char *pass = doc["password"] | "";
            if (!ssid || !ssid[0]) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"ssid\"}");
                return;
            }

            gateway::config_store::wifi_set(String(ssid), pass && pass[0] ? String(pass) : String(""));
            gateway::wifi_manager::schedule_sta_connect(ssid, pass);
            gateway::provision::notify_connect_attempt_started();
            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"accepted\":true}");
        }

        void handle_mqtt_config() {
            if (reject_if_ota_in_progress()) { return; }

            if (!g_http.hasArg("plain")) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, g_http.arg("plain"))) {
                g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
                return;
            }
            const char *host = doc["host"] | "";
            uint16_t port = static_cast<uint16_t>((int)doc["port"] | 1883);
            const char *user = doc["username"] | "";
            const char *pwd = doc["password"] | "";
            if (!host || !host[0]) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"host\"}");
                return;
            }
            gateway::config_store::mqtt_set(String(host), port, String(user), String(pwd));
            gateway::mqtt_manager::apply_config_from_store();
            gateway::mqtt_manager::notify_product_id_changed();
            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
        }

        void handle_wifi_disconnect() {
            gateway::wifi_manager::schedule_sta_disconnect();
            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"accepted\":true,\"note\":\"sta_off_nvs_kept\"}");
        }

        void handle_wifi_scan() {
            const char *json = nullptr;
            int phase = gateway::wifi_manager::wifi_scan_take_result_json(&json);
            if (phase == 2 && json != nullptr) {
                g_http.send(200, "application/json; charset=utf-8", json);
                return;
            }
            if (phase == 0) {
                gateway::wifi_manager::wifi_scan_request_from_http();
                phase = gateway::wifi_manager::wifi_scan_take_result_json(&json);
                if (phase == 2 && json != nullptr) {
                    g_http.send(200, "application/json; charset=utf-8", json);
                    return;
                }
            }
            g_http.send(202, "application/json; charset=utf-8", "{\"scan\":\"pending\"}");
        }

        void handle_factory_reset() {
            if (reject_if_ota_in_progress()) { return; }

            gateway::config_store::factory_reset();
            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
            delay(200);
            ESP.restart();
        }

        void handle_dev_provision() {
            if (!g_http.hasArg("plain")) {
                g_http.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"body\"}");
                return;
            }
            JsonDocument doc;
            if (deserializeJson(doc, g_http.arg("plain"))) {
                g_http.send(400, "application/json; charset=utf-8", "{\"error\":\"json\"}");
                return;
            }
            const bool ok = doc["ok"] | false;
            const char *err = doc["error"] | "";
            gateway::provision::dev_force_outcome(ok, err);
            g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
        }

        void handle_live_state() {
            refresh_live_json_snapshot();
            g_http.send(200, "application/json; charset=utf-8", g_live_state_json);
        }

        void handle_flash_firmware() {
            const bool sent = gateway::can_tx::flash_firmware();
            if (sent) {
                g_http.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
            } else {
                g_http.send(500, "application/json; charset=utf-8", "{\"ok\":false,\"error\":\"enqueue failed\"}");
            }
        }

        // ===== OTA API injection begin =====
        void send_ota_busy_error() {
            g_http.send(409, "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"ota_in_progress\"}");
        }

        bool reject_if_ota_in_progress() {
            if (gateway::ota::update_in_progress()) {
                send_ota_busy_error();
                return true;
            }
            return false;
        }

        void handle_api_version() {
            JsonDocument doc;
            doc["project"] = gateway::version::project_name();
            doc["version"] = gateway::version::firmware_version();
            doc["build"] = gateway::version::firmware_build();
            doc["hw"] = gateway::version::hardware();
            doc["channel"] = gateway::version::channel();
            doc["build_time"] = gateway::version::build_time();
            doc["flash_size"] = ESP.getFlashChipSize();
            doc["psram_size"] = ESP.getPsramSize();
            doc["free_heap"] = ESP.getFreeHeap();

            char stack[768];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_ota_status() {
            const auto s = gateway::ota::status();

            JsonDocument doc;
            doc["state"] = gateway::ota::state_string(s.state);
            doc["update_available"] = s.update_available;
            doc["update_in_progress"] = s.update_in_progress;
            doc["progress"] = s.progress;
            doc["current_version"] = s.current_version;
            doc["current_build"] = s.current_build;
            doc["latest_version"] = s.latest_version;
            doc["latest_build"] = s.latest_build;
            doc["last_error"] = s.last_error;

            char stack[768];
            send_json_doc(doc, stack, sizeof(stack));
        }

        void handle_ota_check() {
            if (gateway::ota::request_check_now()) {
                g_http.send(200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"message\":\"ota_check_requested\"}");
                return;
            }

            g_http.send(409, "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"ota_busy_or_too_soon\"}");
        }

        void handle_ota_upgrade() {
            if (gateway::ota::request_upgrade_now()) {
                g_http.send(200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"message\":\"ota_upgrade_requested\"}");
                return;
            }

            g_http.send(409, "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"ota_busy\"}");
        }
        // ===== OTA API injection end =====


    } // namespace

    void start() {
        g_http.on("/", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                g_http.send_P(200, "text/html; charset=utf-8", gateway::web_assets::kIndexHtml);
#pragma GCC diagnostic pop
        });
        g_http.on("/style.css", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                g_http.send_P(200, "text/css; charset=utf-8", gateway::web_assets::kStyleCss);
#pragma GCC diagnostic pop
        });
        g_http.on("/app.js", HTTP_GET, []() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                g_http.send_P(200, "application/javascript; charset=utf-8", gateway::web_assets::kAppJs);
#pragma GCC diagnostic pop
        });

        g_http.on("/api/wifi/provision", HTTP_GET, []() {
                char stack[768];
                gateway::provision::write_status_json(stack, sizeof(stack));
                g_http.send(200, "application/json; charset=utf-8", stack);
        });

        g_http.on("/api/status", HTTP_GET, []() {
                refresh_live_json_snapshot();
                JsonDocument out;
                if (deserializeJson(out, g_live_state_json)) {
                    out.clear();
            }
                out["heap_free"] = ESP.getFreeHeap();
                char stack[1536];
                send_json_doc(out, stack, sizeof(stack));
        });

        g_http.on("/api/live_state", HTTP_GET, handle_live_state);
        g_http.on("/api/version", HTTP_GET, handle_api_version);
        g_http.on("/api/ota/status", HTTP_GET, handle_ota_status);
        g_http.on("/api/ota/check", HTTP_POST, handle_ota_check);
        g_http.on("/api/ota/upgrade", HTTP_POST, handle_ota_upgrade);
        g_http.on("/api/page/system_status", HTTP_GET, handle_page_system_status);
        g_http.on("/api/page/calibration", HTTP_GET, handle_page_calibration);
        g_http.on("/api/page/traffic_stats", HTTP_GET, handle_page_traffic_stats);
        g_http.on("/api/page/provision", HTTP_GET, handle_page_provision);
        g_http.on("/api/calibration/submit", HTTP_POST, handle_calibration_submit);
        g_http.on("/api/flash_firmware", HTTP_POST, handle_flash_firmware);

        g_http.on("/api/wifi/connect", HTTP_POST, handle_wifi_connect);
        g_http.on("/api/wifi/disconnect", HTTP_POST, handle_wifi_disconnect);
        g_http.on("/api/wifi/scan", HTTP_GET, handle_wifi_scan);
        g_http.on("/api/mqtt/config", HTTP_POST, handle_mqtt_config);
        g_http.on("/api/factory_reset", HTTP_POST, handle_factory_reset);
        g_http.on("/api/dev/provision_outcome", HTTP_POST, handle_dev_provision);

        g_http.onNotFound([]() { g_http.send(404, "text/plain", "Not Found"); });

        g_http.begin();
        refresh_live_json_snapshot();
        Serial.printf("[HTTP] Arduino WebServer (sync) :%u\r\n", static_cast<unsigned>(kHttpListenPort));
    }

    void poll() { g_http.handleClient(); }

} // namespace gateway::web_server
