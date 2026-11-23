#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 初始化 Wi-Fi 并以 STA 模式连接路由器（含断线自动重连）
 * @return true 成功，false 失败
 */
bool wifi_init_sta(void);

/**
 * @brief 阻塞等待 Wi-Fi 真正拿到 IP
 *
 * wifi_init_sta() 只是"启动连接"就立刻返回，真正连上是在事件回调里
 * 异步完成的。上层如果紧接着就去连服务器（MQTT/TCP），会在没有 IP 的
 * 情况下白连失败若干次，所以这里提供显式等待。
 *
 * 注意：超时返回 false 不代表失败，调用方应继续走采集流程——
 * 题目要求断网不影响本地采集，网络恢复后自动续传。
 *
 * @param timeout_ms 最长等待毫秒数
 * @return true 已拿到 IP，false 超时
 */
bool wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief 查询当前是否已连接并获取到 IP
 */
bool wifi_is_connected(void);

/**
 * @brief 获取当前 IP 字符串（未连接时返回 "0.0.0.0"）
 */
const char *wifi_get_ip(void);

#endif /* WIFI_H */
