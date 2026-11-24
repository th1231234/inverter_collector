#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化命令处理模块：创建请求队列与执行任务
 * @return true 成功
 *
 * 架构说明（为什么不在 MQTT 回调里直接读 Modbus）：
 *   MQTT 回调运行在 esp-mqtt 的任务上下文里，一次 Modbus 读最坏要
 *   200ms 超时 × 3 次重试 ≈ 600ms。在回调里同步读会让 MQTT 任务停摆，
 *   超过 keepalive（60s）就被 Broker 判定离线，表现为反复掉线重连。
 *
 *   所以回调只做「传达室」：校验长度 → protobuf 解码 → 入队，然后立刻返回；
 *   真正的 Modbus 读写放在本模块自己的任务里慢慢等超时。
 */
bool mqtt_cmd_init(void);

/**
 * @brief 由 MQTT 的 MQTT_EVENT_DATA 回调调用
 * @param topic     主题（**不以 '\0' 结尾**，长度由 topic_len 给出）
 * @param topic_len 主题字节数
 * @param data      载荷（**不以 '\0' 结尾**，可能含 0x00，长度由 data_len 给出）
 * @param data_len  载荷字节数
 *
 * 参数校验全部由编解码层完成（长度、req_id 字符集、各字段范围）；
 * 能取到合法 req_id 的越界请求立即回 Resp{ok=false}，
 * 取不到 req_id 的坏报文只记日志丢弃。合法请求入队，执行后回应答。
 */
void mqtt_cmd_handle_message(const char *topic, int topic_len,
                             const char *data, int data_len);

#ifdef __cplusplus
}
#endif
