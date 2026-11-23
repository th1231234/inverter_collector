#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 * 主题定义（指导书 2.3：dev/<工号>/用途）
 *
 *   下行 dev/<工号>/read    上位机 → 设备，查询寄存器（功能码 03）
 *   下行 dev/<工号>/write   上位机 → 设备，修改寄存器（功能码 06）
 *   上行 dev/<工号>/resp    设备 → 上位机，应答（每个请求必回一条）
 *   上行 dev/<工号>/data    设备 → 上位机，周期上报
 *
 * 用字符串字面量拼接，编译期完成，运行时零开销；
 * 改工号只需改 menuconfig 里的 CONFIG_MQTT_DEV_ID。
 * ================================================================== */
#define MQTT_TOPIC_READ   "dev/" CONFIG_MQTT_DEV_ID "/read"
#define MQTT_TOPIC_WRITE  "dev/" CONFIG_MQTT_DEV_ID "/write"
#define MQTT_TOPIC_RESP   "dev/" CONFIG_MQTT_DEV_ID "/resp"
#define MQTT_TOPIC_DATA   "dev/" CONFIG_MQTT_DEV_ID "/data"

/**
 * @brief 初始化 MQTT 客户端并连接 Broker
 * @return true 初始化成功（连接是异步的，结果看事件回调）
 *
 * 连接、断线重连都由 esp-mqtt 组件自动完成，这里只负责启一次。
 * 每次连上（含断线重连）都会在回调里重新订阅 read / write，
 * 否则 clean session 下重连后订阅关系丢失，表现为「能上报但收不到命令」。
 */
bool mqtt_report_init(void);

/**
 * @brief 当前是否已经连上 Broker
 */
bool mqtt_report_is_connected(void);

/**
 * @brief 发布一条采集数据到 dev/<工号>/data（QoS 1）
 * @param voltage 电压（V）
 * @param current 电流（A）
 * @param power   功率（W）
 * @param state   设备状态：0=正常，1=故障（读失败时数值为上一次缓存值）
 * @return true 已交给协议栈发送，false 未连接或发送失败
 *
 * 载荷格式：
 * {"voltage":350.0,"current":5.00,"power":1750,"state":0}
 */
bool mqtt_report_send_json(float voltage, float current, float power, int state);

/**
 * @brief 通用发布接口（应答模块用）
 * @param topic   目标主题
 * @param payload 载荷，允许不以 '\0' 结尾
 * @param len     载荷真实字节数
 *
 * 阶段二都是文本 JSON，len 传 strlen 即可；阶段三换成 protobuf 二进制时，
 * len 必须传真实字节数，不能传 0 交给 strlen —— 二进制里遇到 0x00 会被截断。
 */
bool mqtt_report_publish(const char *topic, const char *payload, int len);

#ifdef __cplusplus
}
#endif
