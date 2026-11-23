#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
串口抓日志小工具（只读，不发数据）

用法:
    python tools/serial_read.py [端口] [波特率] [秒数]
例:
    python tools/serial_read.py COM8 115200 25

为什么要自己写:
    idf.py monitor 会触发 cmake 重新配置（可能间接引发重编），
    这个脚本只开串口读字节，绝不动构建目录。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM8"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
DURATION = float(sys.argv[3]) if len(sys.argv) > 3 else 25.0

try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

print("打开 %s @ %d ..." % (PORT, BAUD))
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
# 关键: 不要用 DTR/RTS 去拉复位脚，否则芯片会被按在复位/下载模式
ser.dtr = False
ser.rts = False
ser.open()

total = 0
lines = 0
deadline = time.time() + DURATION
try:
    while time.time() < deadline:
        chunk = ser.read(4096)
        if not chunk:
            continue
        total += len(chunk)
        text = chunk.decode("utf-8", errors="replace")
        lines += text.count("\n")
        sys.stdout.write(text)
finally:
    ser.close()

print("\n--- 共收到 %d 字节, %d 行 ---" % (total, lines))
if total == 0:
    print("一个字节都没收到: 板子可能还在复位/下载模式，或者 TX 线没接对。")
    print("请手动点一下板子上的 RST 键，再重跑本脚本。")
