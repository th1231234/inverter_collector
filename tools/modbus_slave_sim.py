"""Modbus RTU 从机模拟器（替代 Modbus Slave 图形界面，方便脚本化测试）。

为什么自己写一个：
    图形界面点来点去改不了「一口气把 0x3000 改成 0」这种测试步骤，
    也留不下带时间戳的日志。阶段三的 P-04 / P-05 都要精确控制寄存器值，
    用脚本从机最省事。

用法：
    python tools/modbus_slave_sim.py                 # COM10, 9600, 站号 1
    python tools/modbus_slave_sim.py COM10 9600 1
    python tools/modbus_slave_sim.py COM10 9600 1 --set 0x3000=3000

支持的功能码：
    0x03 读保持寄存器      0x06 写单个寄存器
其它功能码回异常码 01（非法功能），地址越界回异常码 02（非法地址）。

寄存器初值就是指导书的逆变器映射：
    0x3000 = 3500 (350.0V)   0x3002 = 500 (5.00A)   0x3004 = 1750 (1750W)
0x3001 / 0x3003 是空位，保持 0。写命令会直接改这里的值，
所以板子发来的 write 请求能立刻反映到下一帧 data 里。
"""

import struct
import sys
import time

import serial


def crc16(data: bytes) -> int:
    """Modbus RTU 的 CRC16（多项式 0xA001，初值 0xFFFF，低字节在前）。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def with_crc(body: bytes) -> bytes:
    return body + struct.pack("<H", crc16(body))


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 9600
    slave = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    # 逆变器映射：addr -> value
    regs = {0x3000: 3500, 0x3001: 0, 0x3002: 500, 0x3003: 0, 0x3004: 1750}

    # --set 0x3000=3000 覆盖初值，方便直接摆出想要的工况
    for arg in sys.argv[4:]:
        if arg.startswith("--set"):
            continue
        if "=" in arg:
            k, v = arg.split("=", 1)
            regs[int(k, 0)] = int(v, 0) & 0xFFFF

    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except Exception as exc:
        print("打不开 %s: %s" % (port, exc))
        return 1

    print("从机就绪 %s @ %d, 站号 %d" % (port, baud, slave))
    print("寄存器初值: " + "  ".join("0x%04X=%d" % (a, regs[a]) for a in sorted(regs)))

    try:
        ser.reset_input_buffer()
    except Exception:
        pass

    while True:
        # 一帧最短 8 字节；先读 2 字节看站号，再按需补齐
        head = ser.read(2)
        if len(head) < 2:
            continue

        if head[0] != slave:
            # 不是发给本站的帧，丢掉（真实总线上多站共存时会走到这里）
            continue

        func = head[1]
        rest_len = 6 if func in (0x03, 0x06, 0x04, 0x01, 0x02, 0x05) else 4
        rest = ser.read(rest_len)
        if len(rest) < rest_len:
            print("[drop] 帧不完整 func=0x%02X" % func)
            continue

        frame = head + rest
        if struct.unpack("<H", frame[-2:])[0] != crc16(frame[:-2]):
            print("[drop] CRC 错 %s" % frame.hex())
            continue

        if func in (0x03, 0x04):
            addr, num = struct.unpack(">HH", frame[2:6])
            print("[%s] read  0x%04X x %d" % (time.strftime("%H:%M:%S"), addr, num))

            if num == 0 or num > 125:
                ser.write(with_crc(bytes([slave, func | 0x80, 0x03])))
                continue
            out = []
            bad = False
            for i in range(num):
                a = addr + i
                if a not in regs:
                    bad = True
                    break
                out.append(regs[a])
            if bad:
                print("        -> 地址越界，回异常 02")
                ser.write(with_crc(bytes([slave, func | 0x80, 0x02])))
                continue

            body = bytes([slave, func, num * 2]) + struct.pack(">%dH" % num, *out)
            ser.write(with_crc(body))
            print("        -> " + " ".join("%d" % v for v in out))

        elif func == 0x06:
            addr, value = struct.unpack(">HH", frame[2:6])
            print("[%s] write 0x%04X = %d" % (time.strftime("%H:%M:%S"), addr, value))
            if addr not in regs:
                ser.write(with_crc(bytes([slave, func | 0x80, 0x02])))
                continue
            regs[addr] = value
            ser.write(frame)        # 06 的正常响应就是原样回显
            print("        -> 已写，当前 0x%04X=%d" % (addr, value))

        else:
            print("[%s] 不支持的功能码 0x%02X，回异常 01" % (time.strftime("%H:%M:%S"), func))
            ser.write(with_crc(bytes([slave, func | 0x80, 0x01])))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n退出")
