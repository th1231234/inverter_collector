#ifndef MODBUS_SELFTEST_H
#define MODBUS_SELFTEST_H

/**
 * @brief 依次测试 Modbus 六个功能码与异常应答
 *
 * 测试内容（从机地址固定为 1）：
 *   1. 03 读保持寄存器 0x3000 起 5 个
 *   2. 06 写 0x3010 = 4000，再用 03 读回校验
 *   3. 16 写 0x3011~0x3013 = 1111/2222/3333，再用 03 读回校验
 *   4. 04 读输入寄存器 0x3000 起 3 个
 *   5. 01 读线圈 0x0000 起 8 个
 *   6. 02 读离散输入 0x0000 起 8 个
 *   7. 读越界地址 0xFFFF，期望从机返回异常帧
 *
 * 该函数为阻塞式，约耗时 3~6 秒，跑完打印汇总表。
 * 仅用于 training / 调试，由 CONFIG_MODBUS_SELFTEST 控制是否启用。
 */
void modbus_selftest_run(void);

#endif /* MODBUS_SELFTEST_H */
