#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>

/**
 * @brief 初始化 Wi-Fi 并以 STA 模式连接路由器（含断线自动重连）
 * @return true 成功，false 失败
 */
bool wifi_init_sta(void);

/**
 * @brief 查询当前是否已连接并获取到 IP
 */
bool wifi_is_connected(void);

/**
 * @brief 获取当前 IP 字符串（未连接时返回 "0.0.0.0"）
 */
const char *wifi_get_ip(void);

#endif /* WIFI_H */
