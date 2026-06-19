#pragma once

/**
 * @file provision_fsm.h
 * @brief 一期配网门户：阶段常量、超时与 WiFi 结果通知接口。
 */

#include <cstddef>
#include <cstdint>

namespace gateway::provision {

    /**
     * 配网状态机超时（毫秒）：表单触发连接后若在此时长内未得到「成功」（含开发接口模拟），一期无 WiFi
     * 时通常判为超时失败。
     */
    constexpr uint32_t kPhase1FailTimeoutMs = 8000;

    /**
     * 假定用户连接设备 SoftAP 时使用的默认网关（与常见 192.168.4.1 一致）。写入 JSON /
     * SSE 脚本中的 Host；二期 AP 就绪后应与 `WiFi.softAPIP()` 对齐。亦可在此后与 SNTP /
     * SSE 载荷中增补 `epoch_sec` 等墙钟字段。
     */
    constexpr const char *kPortalGatewayHost = "192.168.4.1";

    void tick();

    /** HTTP 校验 ssid 后调用，进入 Connecting（本期不向 WiFi 驱动写入凭据以外的逻辑）。 */
    void notify_connect_attempt_started();

    /** 由 `wifi_manager` 在 STA 成功/失败时调用（仅当当前为 Connecting）。 */
    void notify_wifi_sta_linked();
    void notify_wifi_sta_failed(const char *reason);

    /** 用户主动断开 STA：配网页回到 idle，避免出现「仍为 ok」与按钮状态不一致。 */
    void notify_sta_user_disconnected();

    enum class Phase : uint8_t { Idle = 0, Connecting, Ok, Fail };

    Phase current_phase();

    /** 写入 JSON UTF-8；返回 `serializeJson` 结果（字节数）。 */
    size_t write_status_json(char *buf, size_t cap);

    /** 一期联调：`ok==true` 视为配网成功并生成脚本字段；否则进入 Fail。 */
    void dev_force_outcome(bool ok, const char *error_if_fail);

} // namespace gateway::provision
