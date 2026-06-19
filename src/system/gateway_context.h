#pragma once

/**
 * @file system/gateway_context.h
 * @brief 设备级共享状态（product_id）声明；互斥仅在实现文件内用于跨任务读写。
 */

#include <cstddef>
#include <cstdint>

namespace gateway::ctx {

    void init();

    bool has_product_id();
    void get_product_id_hex(char *out9);
    /** 若解析出的 product_id 与当前保存值不同则更新并返回 true（用于触发 MQTT topic 重绑等一次性动作）。 */
    bool set_product_id_from_payload_le(const uint8_t *payload, size_t len);

} // namespace gateway::ctx
