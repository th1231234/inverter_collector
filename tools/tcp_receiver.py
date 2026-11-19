#!/usr/bin/env python3
"""
TCP 接收端：模拟导师那台 192.168.1.200:8080 的服务器
用途：验证 ESP32 以 TCP 方式上报的 JSON 数据是否正确（配合 menuconfig 选 TCP）

运行方式（在电脑上开一个终端）：
    python tcp_receiver.py

按 Ctrl+C 退出
"""

import socket
import datetime

LISTEN_IP = "0.0.0.0"   # 监听所有网卡
LISTEN_PORT = 8080

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((LISTEN_IP, LISTEN_PORT))
srv.listen(1)

print("=" * 60)
print(f"TCP 接收端已启动，监听 {LISTEN_IP}:{LISTEN_PORT}")
print("等待 ESP32 连接（Ctrl+C 退出）")
print("=" * 60)

count = 0
try:
    while True:
        conn, addr = srv.accept()
        print(f"[{datetime.datetime.now():%H:%M:%S}] ESP32 已连接：{addr[0]}:{addr[1]}")
        buf = b""
        try:
            while True:
                data = conn.recv(1024)
                if not data:
                    print("对端断开，等待重新连接 ...")
                    break
                buf += data
                # TCP 是字节流，可能出现粘包/半包，按换行符切分
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    count += 1
                    now = datetime.datetime.now().strftime("%H:%M:%S")
                    print(f"[{now}] #{count} 数据: {line.decode('utf-8', errors='replace')}")
        finally:
            conn.close()
except KeyboardInterrupt:
    print(f"\n已退出，共收到 {count} 条数据")
finally:
    srv.close()
