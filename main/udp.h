#ifndef UDP_H
#define UDP_H

#include <stdbool.h>

/**
 * @brief 初始化 UDP 客户端（创建 socket，设置目标地址）
 * @return true 成功，false 失败
 */
bool udp_client_init(void);

/**
 * @brief 以 JSON 格式发送采集数据
 * @param voltage 电压（V）
 * @param current 电流（A）
 * @param power   功率（W）
 * @return true 发送成功，false 失败
 *
 * 报文格式：{"voltage":350.0,"current":5.00,"power":1750}
 */
bool udp_send_json(float voltage, float current, float power);

#endif /* UDP_H */
