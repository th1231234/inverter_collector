#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief CRC16-Modbus 校验计算
 * @param data 数据指针
 * @param len  数据长度（字节）
 * @return 16 位 CRC 校验值
 *
 * 多项式 0x8005（反转形式 0xA001），初始值 0xFFFF
 */
uint16_t modbus_crc16(const uint8_t *data, uint16_t len);

/**
 * @brief 初始化 Modbus 串口（9600-8-N-1）
 * @return true 成功，false 失败
 */
bool modbus_init(void);

/**
 * @brief 读取从机保持寄存器（功能码 03）
 * @param reg_addr 寄存器起始地址
 * @param reg_num  读取寄存器数量（1~125）
 * @param data     结果缓冲区，长度至少 reg_num，按大端序解析为 uint16
 * @return true 成功，false 失败（超时 / CRC 错 / 从机异常）
 */
bool modbus_read_registers(uint16_t reg_addr, uint16_t reg_num, uint16_t *data);

#endif /* MODBUS_H */
