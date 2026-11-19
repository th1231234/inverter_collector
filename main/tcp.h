#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建一个 TCP 客户端 socket（不立即连接，首次发送时自动连接） */
bool tcp_client_init(void);

/* 当前是否已有可用连接 */
bool tcp_client_is_connected(void);

/* 主动关闭并重新连接，失败返回 false */
bool tcp_client_reconnect(void);

/* 关闭 socket（Wi-Fi 断线后需要调用，避免使用半死连接） */
void tcp_client_close(void);

/* 发送一条 JSON，失败时自动关闭连接、下个周期重建 */
bool tcp_send_json(float voltage, float current, float power);

#ifdef __cplusplus
}
#endif
