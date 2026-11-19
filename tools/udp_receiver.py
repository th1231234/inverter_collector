#!/usr/bin/env python3
"""
UDP 接收端：模拟上司/导师那台 192.168.1.200:8080 的服务器
用途：验证 ESP32 上报的 JSON 数据是否正确

运行方式（在电脑上开一个终端）：
    python udp_receiver.py

按 Ctrl+C 退出
"""

import socket
import datetime

LISTEN_IP = "0.0.0.0"   # 监听所有网卡
LISTEN_PORT = 8080

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((LISTEN_IP, LISTEN_PORT))

print("=" * 60)
print(f"UDP 接收端已启动，监听 {LISTEN_IP}:{LISTEN_PORT}")
print("等待 ESP32 上报数据（Ctrl+C 退出）")
print("=" * 60)

count = 0
try:
    while True:
        data, addr = sock.recvfrom(2048)
        count += 1
        now = datetime.datetime.now().strftime("%H:%M:%S")
        print(f"[{now}] #{count} 来自 {addr[0]}:{addr[1]}")
        print(f"        数据: {data.decode('utf-8', errors='replace')}")
except KeyboardInterrupt:
    print(f"\n已退出，共收到 {count} 条数据")
finally:
    sock.close()
