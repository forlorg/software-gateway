#pragma once

/**
 * @file wifi_manager.h
 * @brief Wi-Fi 驱动侧操作入口（仅允许在 `wifi_ap_task` 中调用 `loop`）。
 */

namespace gateway::wifi_manager {

    void start();

    /** 仅从 `WiFi_AP` 任务周期性调用（与 Arduino-ESP WiFi API 同源）。 */
    void loop();

    /** 断开当前 STA 后按给定凭据连接；不写 NVS（调用方可先 wifi_set）。 */
    bool request_sta_connect(const char *ssid, const char *password);

    /** 从 HTTP 等在其它任务排队，仅在 `WiFi_AP` 任务 drain 执行 `WiFi.begin`。 */
    void schedule_sta_connect(const char *ssid, const char *password);

    /** STA 断开：保留 NVS 凭据以便下次上电/手动重连，并暂时禁止空闲态自动发起 saved STA。 */
    void schedule_sta_disconnect();

    bool sta_is_linked();

    const char *sta_status_json_fragment();

    int sta_rssi_cached();
    void sta_local_ip_str(char *buf, unsigned cap);

    /** STA 已连接时写入当前关联 SSID，否则置空串。 */
    void sta_current_ssid(char *buf, unsigned cap);

    void wifi_scan_request_from_http();

    /** @return 0 空闲 / 1 扫描中 / 2 可用（*out 指向内部缓冲） */
    int wifi_scan_take_result_json(const char **out);

} // namespace gateway::wifi_manager
