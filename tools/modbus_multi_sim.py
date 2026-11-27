"""一个串口、多个从机站号的 Modbus RTU 模拟器。

为什么需要它：
    Modbus Slave（Witte 图形版）每个实例只能扮演一个站号，而且一个 COM 口
    只能被一个进程打开。想在一对 RS-485 线上验证"多从机轮询"，要么买多个
    USB 转 485 适配器，要么用这个脚本——一个进程监听一个口，按帧里的站号分发。

默认三台从机，寄存器都从 0x3000 开始（地址可以相同，靠站号区分）：

    站号 1: 0x3000=3500  0x3002=500   0x3004=1750
    站号 2: 0x3000=4000  0x3002=600   0x3004=2000
    站号 3: 0x3000=2200  0x3002=300   0x3004=660

用法：
    python tools/modbus_multi_sim.py COM10 9600
    python tools/modbus_multi_sim.py COM10 9600 --verbose
    python tools/modbus_multi_sim.py COM10 9600 --only 1,2    # 让 3 号"掉线"，验证单台故障
    python tools/modbus_multi_sim.py --selftest          # 不需要串口，跑单元自测

支持的功能码：
    0x03 读保持寄存器  0x06 写单个寄存器  0x10 写多个寄存器
    站号 0（广播）：执行但不回应（符合规范）
    其它功能码：回异常码 01；地址越界：回异常码 02；数量非法：回异常码 03
"""

import sys
import time

import serial

# ---------------------------------------------------------------------------
# 从机数据模型
# ---------------------------------------------------------------------------
BASE_ADDR = 0x3000
REG_COUNT = 16                      # 每台从机 0x3000~0x300F

SLAVE_DEFAULTS = {
    1: {0x3000: 3500, 0x3002: 500, 0x3004: 1750},
    2: {0x3000: 4000, 0x3002: 600, 0x3004: 2000},
    3: {0x3000: 2200, 0x3002: 300, 0x3004: 660},
}

# 运行期寄存器表：slave_id -> {addr: value}
REGISTERS = {}
for _sid, _pts in SLAVE_DEFAULTS.items():
    _bank = {BASE_ADDR + i: 0 for i in range(REG_COUNT)}
    _bank.update(_pts)
    REGISTERS[_sid] = _bank


def apply_only_filter(only) -> None:
    """--only 1,2：把不在名单里的从机从寄存器表里删掉。

    删掉之后它对任何帧都沉默（走 handle_frame 里 "slave not in REGISTERS"
    那个分支），效果等价于"这台从机掉线/断电/没接线"。
    用来验证固件的单台故障处理：其余从机照常读数，掉线那台保留上次的值。
    """
    for sid in list(REGISTERS):
        if sid not in only:
            del REGISTERS[sid]


def crc16(data: bytes) -> int:
    """Modbus CRC16：多项式 0xA001（0x8005 反转），初值 0xFFFF。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(body: bytes) -> bytes:
    """附加 CRC，低字节在前。"""
    crc = crc16(body)
    return body + bytes([crc & 0xFF, crc >> 8])


def crc_ok(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    calc = crc16(frame[:-2])
    recv = frame[-2] | (frame[-1] << 8)
    return calc == recv


# ---------------------------------------------------------------------------
# 帧处理（纯函数，便于脱离串口做自测）
# ---------------------------------------------------------------------------
def handle_frame(frame: bytes, verbose: bool = False):
    """处理一帧请求，返回应答字节；不应答时返回 None。"""
    if not crc_ok(frame):
        if verbose:
            print("  [drop] CRC 错: %s" % frame.hex(" "))
        return None

    slave = frame[0]
    func = frame[1]

    if slave != 0 and slave not in REGISTERS:
        if verbose:
            print("  [drop] 站号 %d 不是我管的（本机: %s）"
                  % (slave, sorted(REGISTERS)))
        return None

    bank = REGISTERS.get(slave, {})

    if func == 0x03:
        addr = (frame[2] << 8) | frame[3]
        num = (frame[4] << 8) | frame[5]
        if num < 1 or num > 125:
            return _exception(slave, func, 0x03)
        if addr < BASE_ADDR or addr + num - 1 >= BASE_ADDR + REG_COUNT:
            return _exception(slave, func, 0x02)
        values = [bank.get(addr + i, 0) for i in range(num)]
        body = bytes([slave, func, num * 2])
        for v in values:
            body += bytes([(v >> 8) & 0xFF, v & 0xFF])
        if verbose:
            print("  站号%d 读 0x%04X×%d -> %s" % (slave, addr, num, values))
        return None if slave == 0 else with_crc(body)

    if func == 0x06:
        addr = (frame[2] << 8) | frame[3]
        value = (frame[4] << 8) | frame[5]
        if addr < BASE_ADDR or addr >= BASE_ADDR + REG_COUNT:
            return _exception(slave, func, 0x02)
        bank[addr] = value
        if verbose:
            print("  站号%d 写 0x%04X = %d (%d.%s)"
                  % (slave, addr, value, value // 10, value % 10))
        return None if slave == 0 else with_crc(frame[:6])

    if func == 0x10:
        addr = (frame[2] << 8) | frame[3]
        num = (frame[4] << 8) | frame[5]
        data = frame[7:7 + num * 2]
        if num < 1 or num > 123 or len(data) != num * 2:
            return _exception(slave, func, 0x03)
        if addr < BASE_ADDR or addr + num - 1 >= BASE_ADDR + REG_COUNT:
            return _exception(slave, func, 0x02)
        for i in range(num):
            bank[addr + i] = (data[i * 2] << 8) | data[i * 2 + 1]
        if verbose:
            print("  站号%d 批量写 0x%04X×%d" % (slave, addr, num))
        return None if slave == 0 else with_crc(frame[:6])

    return _exception(slave, func, 0x01)


def _exception(slave: int, func: int, code: int):
    """异常应答：功能码最高位置 1，长度固定 5 字节。"""
    return with_crc(bytes([slave, func | 0x80, code]))


def frame_expected_len(buf: bytes):
    """已知功能码时推断整帧长度，用于判断是否收全。"""
    if len(buf) < 2:
        return None
    func = buf[1]
    if func in (0x03, 0x06):
        return 8
    if func == 0x10:
        return 9 + buf[6] if len(buf) >= 7 else None
    return 8


# ---------------------------------------------------------------------------
# 串口主循环
# ---------------------------------------------------------------------------
def run(port: str, baud: int, verbose: bool = False) -> int:
    t35 = max(3.5 * 11 / baud, 0.00175)      # Modbus 帧间静默，最保守 1.75 ms

    try:
        ser = serial.Serial(port, baud, timeout=0.01)
    except Exception as exc:
        print("打开 %s 失败: %s" % (port, exc))
        return 1

    print("多从机模拟器已启动: %s %d-8-N-1，模拟站号 %s"
          % (port, baud, sorted(REGISTERS)))
    for sid in sorted(REGISTERS):
        pts = {hex(a): v for a, v in REGISTERS[sid].items() if v}
        print("  站号 %d: %s" % (sid, pts))
    print("按 Ctrl+C 退出\n")

    buf = b""
    last_rx = 0.0
    total = 0
    try:
        while True:
            chunk = ser.read(256)
            now = time.time()
            if chunk:
                buf += chunk
                last_rx = now
                continue
            # 没有新字节，且静默超过 t3.5，且缓冲区里像是一帧 → 处理
            if buf and now - last_rx >= t35:
                need = frame_expected_len(buf)
                if need is None or len(buf) >= need:
                    total += 1
                    resp = handle_frame(buf, verbose)
                    if verbose:
                        print("[%d] RX %s" % (total, buf.hex(" ")))
                    if resp:
                        ser.write(resp)
                        if verbose:
                            print("    TX %s" % resp.hex(" "))
                    buf = b""
                elif len(buf) > 256:
                    print("  [drop] 缓冲区异常，长度 %d" % len(buf))
                    buf = b""
            elif buf and now - last_rx > 1.0:
                # 半帧卡住超过 1 秒，丢弃
                if verbose:
                    print("  [drop] 半帧超时: %s" % buf.hex(" "))
                buf = b""
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print("\n共处理 %d 帧，已退出" % total)
    return 0


# ---------------------------------------------------------------------------
# 自测：不依赖串口，直接喂字节验证应答
# ---------------------------------------------------------------------------
def selftest() -> int:
    cases = []

    # 1) 三台从机各读 5 个寄存器
    for sid in (1, 2, 3):
        req = with_crc(bytes([sid, 0x03, 0x30, 0x00, 0x00, 0x05]))
        cases.append(("站号%d 读 5 个" % sid, req, "有应答"))

    # 2) 不存在的站号 → 不回应
    cases.append(("站号 9 读", with_crc(bytes([9, 0x03, 0x30, 0x00, 0x00, 0x05])), "不应答"))

    # 3) 地址越界 → 异常码 02
    cases.append(("站号1 读 0x3050", with_crc(bytes([1, 0x03, 0x30, 0x50, 0x00, 0x02])), "异常02"))

    # 4) 功能码不支持 → 异常码 01
    cases.append(("站号1 功能码 0x04", with_crc(bytes([1, 0x04, 0x30, 0x00, 0x00, 0x02])), "异常01"))

    # 5) CRC 错 → 不回应
    bad = bytearray(with_crc(bytes([1, 0x03, 0x30, 0x00, 0x00, 0x05])))
    bad[-1] ^= 0xFF
    cases.append(("CRC 损坏", bytes(bad), "不应答"))

    ok = True
    for name, req, expect in cases:
        resp = handle_frame(req)
        if expect == "不应答":
            passed = resp is None
            detail = "无应答" if resp is None else resp.hex(" ")
        elif expect == "异常02":
            passed = resp is not None and resp[2] == 0x02
            detail = resp.hex(" ") if resp else "无应答"
        elif expect == "异常01":
            passed = resp is not None and resp[2] == 0x01
            detail = resp.hex(" ") if resp else "无应答"
        else:
            passed = resp is not None and crc_ok(resp) and resp[2] == 0x0A
            detail = resp.hex(" ") if resp else "无应答"
        print("%-18s %s  %s" % (name, "PASS" if passed else "FAIL", detail))
        ok = ok and passed

    # 6) 写单个寄存器后读回来
    REGISTERS[2][0x3000] = 4000
    handle_frame(with_crc(bytes([2, 0x06, 0x30, 0x00, 0x0F, 0xA0])))   # 写 4000
    resp = handle_frame(with_crc(bytes([2, 0x03, 0x30, 0x00, 0x00, 0x01])))
    got = (resp[3] << 8) | resp[4]
    print("%-18s %s  读回 %d" % ("站号2 写后读回", "PASS" if got == 4000 else "FAIL", got))
    ok = ok and (got == 4000)

    # 7) 站号隔离：写 1 号不应影响 2 号
    handle_frame(with_crc(bytes([1, 0x06, 0x30, 0x00, 0x0C, 0x1C])))   # 1 号写 3100
    s1 = (handle_frame(with_crc(bytes([1, 0x03, 0x30, 0x00, 0x00, 0x01]))))
    v1 = (s1[3] << 8) | s1[4]
    print("%-18s %s  站号1=%d 站号2=%d" % ("站号隔离", "PASS" if v1 == 3100 else "FAIL", v1, got))
    ok = ok and v1 == 3100

    print("\n%s" % ("全部通过" if ok else "有失败项"))
    return 0 if ok else 1


def main() -> int:
    args = sys.argv[1:]
    if "--selftest" in args:
        return selftest()
    if not args:
        print(__doc__)
        return 2
    port = args[0]
    baud = int(args[1]) if len(args) > 1 and not args[1].startswith("-") else 9600

    # --only 1,2 ：只让指定的站号应答，其余当成掉线（故障注入用）
    only = None
    if "--only" in args:
        idx = args.index("--only")
        if idx + 1 < len(args):
            only = {int(x) for x in args[idx + 1].split(",") if x.strip()}
    if only:
        apply_only_filter(only)

    return run(port, baud, verbose=("--verbose" in args or "-v" in args))


if __name__ == "__main__":
    sys.exit(main())
