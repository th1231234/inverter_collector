#ifndef MODBUS_H
#define MODBUS_H

#include <stdint.h>
#include <stdbool.h>

/* ==================================================================
 * Modbus 功能码定义
 * ================================================================== */
#define MODBUS_FUNC_READ_COILS              0x01   /* 读线圈（读写区，位） */
#define MODBUS_FUNC_READ_DISCRETE_INPUTS    0x02   /* 读离散输入（只读区，位） */
#define MODBUS_FUNC_READ_HOLDING            0x03   /* 读保持寄存器（读写区，字） */
#define MODBUS_FUNC_READ_INPUT              0x04   /* 读输入寄存器（只读区，字） */
#define MODBUS_FUNC_WRITE_COIL              0x05   /* 写单线圈 */
#define MODBUS_FUNC_WRITE_REGISTER          0x06   /* 写单寄存器 */
#define MODBUS_FUNC_WRITE_COILS             0x0F   /* 写多线圈 */
#define MODBUS_FUNC_WRITE_REGISTERS         0x10   /* 写多寄存器 */

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

/* ------------------------------------------------------------------
 * 以下五个函数为通用接口
 *   slave：从机地址（1~247）。预留该参数是为了后续支持 485 总线多从机，
 *          当前单从机场景传 1 即可。
 *   返回：true 成功；false 表示超时 / CRC 错 / 从机异常应答
 * ------------------------------------------------------------------ */

/**
 * @brief 读线圈（功能码 01，位数据，可读写区）
 * @param bits 结果缓冲区，(num + 7) / 8 字节；位按 LSB 优先打包
 */
bool modbus_read_coils(uint8_t slave, uint16_t addr, uint16_t num, uint8_t *bits);

/**
 * @brief 读离散输入（功能码 02，位数据，只读区）
 * @param bits 结果缓冲区，(num + 7) / 8 字节
 */
bool modbus_read_discrete_inputs(uint8_t slave, uint16_t addr, uint16_t num, uint8_t *bits);

/**
 * @brief 读保持寄存器（功能码 03，字数据，可读写区）
 * @param regs 结果缓冲区，长度至少 num
 */
bool modbus_read_holding(uint8_t slave, uint16_t addr, uint16_t num, uint16_t *regs);

/**
 * @brief 读输入寄存器（功能码 04，字数据，只读区）
 * @param regs 结果缓冲区，长度至少 num
 */
bool modbus_read_input(uint8_t slave, uint16_t addr, uint16_t num, uint16_t *regs);

/**
 * @brief 写单个保持寄存器（功能码 06）
 */
bool modbus_write_register(uint8_t slave, uint16_t addr, uint16_t value);

/**
 * @brief 写多个连续保持寄存器（功能码 16 / 0x10）
 * @param regs 待写入数据，长度至少 num
 */
bool modbus_write_registers(uint8_t slave, uint16_t addr, uint16_t num, const uint16_t *regs);

/**
 * @brief 从位缓冲区中读取第 index 位
 *
 * Modbus 位数据按「字节内 LSB 优先」打包：
 *   index 0 → bits[0] 的 bit0，index 7 → bits[0] 的 bit7，
 *   index 8 → bits[1] 的 bit0
 */
static inline bool modbus_bit_get(const uint8_t *bits, uint16_t index)
{
    return ((bits[index >> 3] >> (index & 0x07)) & 0x01) != 0;
}

/**
 * @brief 取最近一次通信的从机异常码
 * @return 0 表示无异常；非 0 为 Modbus 异常码（0x01~0x04）
 *
 * 用于区分「从机明确拒绝」与「超时无响应」两种失败：
 * 前者会带回异常码，后者异常码为 0。
 */
uint8_t modbus_last_exception(void);

/**
 * @brief 读取保持寄存器（兼容旧接口，等价于 modbus_read_holding(1, ...)）
 *
 * 保留此函数是为了让上层业务代码无需改动。
 */
bool modbus_read_registers(uint16_t reg_addr, uint16_t reg_num, uint16_t *data);

#endif /* MODBUS_H */
