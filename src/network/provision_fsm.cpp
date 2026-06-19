/**
 * @file provision_fsm.cpp
 * @brief 配网页流程状态机：连接中/成功/失败阶段与 JSON 状态输出。
 */

#include "provision_fsm.h"
#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <cstring>

namespace gateway::provision {

    namespace {

        portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

        Phase s_phase{Phase::Idle};
        uint32_t s_connect_t0_ms{0};
        char s_last_error[96]{};

        const char *phase_str(Phase p) {
            switch (p) {
            case Phase::Idle:
                return "idle";
            case Phase::Connecting:
                return "connecting";
            case Phase::Ok:
                return "ok";
            case Phase::Fail:
                return "fail";
            }
            return "idle";
        }

        void portal_softap_host(char *out, size_t cap) {
            IPAddress ip = WiFi.softAPIP();
            snprintf(out, cap, "%s", ip.toString().c_str());
        }

        void live_poll_url_fmt(char *out, size_t cap) {
            char host[48];
            portal_softap_host(host, sizeof(host));
            if (gateway::web_server::kHttpListenPort == 80) {
                snprintf(out, cap, "http://%s/api/live_state", host);
            } else {
                snprintf(out, cap, "http://%s:%u/api/live_state", host,
                    static_cast<unsigned>(gateway::web_server::kHttpListenPort));
            }
        }

    } // namespace

    void tick() {
        /* WiFi STA 的结果与超时由 wifi_manager → notify_wifi_sta_*；无需在此轮询。 */
    }

    void notify_wifi_sta_linked() {
        portENTER_CRITICAL(&s_mux);
        if (s_phase == Phase::Connecting) {
            s_phase = Phase::Ok;
            s_last_error[0] = '\0';
        }
        portEXIT_CRITICAL(&s_mux);
    }

    void notify_wifi_sta_failed(const char *reason) {
        portENTER_CRITICAL(&s_mux);
        if (s_phase == Phase::Connecting) {
            s_phase = Phase::Fail;
            snprintf(s_last_error, sizeof(s_last_error), "%s",
                reason && reason[0] ? reason : "wifi_sta_fail");
        }
        portEXIT_CRITICAL(&s_mux);
    }

    void notify_sta_user_disconnected() {
        portENTER_CRITICAL(&s_mux);
        s_phase = Phase::Idle;
        s_connect_t0_ms = 0;
        s_last_error[0] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }

    void notify_connect_attempt_started() {
        portENTER_CRITICAL(&s_mux);
        s_phase = Phase::Connecting;
        s_connect_t0_ms = millis();
        s_last_error[0] = '\0';
        portEXIT_CRITICAL(&s_mux);
    }

    Phase current_phase() {
        portENTER_CRITICAL(&s_mux);
        const Phase p = s_phase;
        portEXIT_CRITICAL(&s_mux);
        return p;
    }

    size_t write_status_json(char *buf, size_t cap) {
        if (!buf || cap < 32) {
            return 0;
        }

        portENTER_CRITICAL(&s_mux);
        const Phase ph = s_phase;
        char err_copy[sizeof(s_last_error)];
        memcpy(err_copy, s_last_error, sizeof(err_copy));
        portEXIT_CRITICAL(&s_mux);

        JsonDocument doc;
        doc["phase"] = phase_str(ph);
        if (err_copy[0] != '\0') {
            doc["error"] = err_copy;
        }

        if (ph == Phase::Ok) {
            char poll[96];
            live_poll_url_fmt(poll, sizeof(poll));
            doc["live_poll_url"] = poll;

            char host_curl[48];
            portal_softap_host(host_curl, sizeof(host_curl));
            char curl_line[280];
            if (gateway::web_server::kHttpListenPort == 80) {
                snprintf(curl_line, sizeof(curl_line),
                    "while true; do curl -sS 'http://%s/api/live_state'; echo; sleep 1; done", host_curl);
            } else {
                snprintf(curl_line, sizeof(curl_line),
                    "while true; do curl -sS 'http://%s:%u/api/live_state'; echo; sleep 1; done", host_curl,
                    static_cast<unsigned>(gateway::web_server::kHttpListenPort));
            }
            doc["curl"] = curl_line;

            char js[280];
            snprintf(js, sizeof(js),
                "setInterval(async()=>console.log(await (await fetch('%s')).text()),1000);",
                poll);

            doc["js_console"] = js;
            doc["hint_zh"] = "STA 已联网；可同时保持 SoftAP；/api/live_state 可查 STA IP/RSSI（同步 WebServer，无 "
            "AsyncTCP）。";
        }

        size_t n = serializeJson(doc, buf, cap);
        if (n >= cap && cap > 4) {
            buf[cap - 4] = '.';
            buf[cap - 3] = '.';
            buf[cap - 2] = '.';
            buf[cap - 1] = '\0';
            return cap - 1;
        }
        return n;
    }

    void dev_force_outcome(bool ok, const char *error_if_fail) {
        portENTER_CRITICAL(&s_mux);
        if (ok) {
            s_phase = Phase::Ok;
            s_last_error[0] = '\0';
        } else {
            s_phase = Phase::Fail;
            if (error_if_fail && error_if_fail[0]) {
                snprintf(s_last_error, sizeof(s_last_error), "%s", error_if_fail);
            } else {
                snprintf(s_last_error, sizeof(s_last_error), "%s", "dev_forced_fail");
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }

} // namespace gateway::provision
