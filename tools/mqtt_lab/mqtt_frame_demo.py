"""把 MQTT 报文拆开看 —— 不用 paho，不用 amqtt，纯 socket 手拼字节。

目的：证明"客户端和服务器通信"不是魔法，就是把几个字节拼好发过去。
本文件只做四件事：
    1. 用 TCP 连上 1883 端口
    2. 手拼一个 CONNECT 报文，等 CONNACK
    3. 手拼一个 SUBSCRIBE 报文，等 SUBACK
    4. 手拼一个 PUBLISH 报文，等 broker 把它转发回来

用法：
    python mqtt_frame_demo.py
    python mqtt_frame_demo.py 192.168.137.1 1883
"""

import socket
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883

NAMES = {
    0x10: "CONNECT", 0x20: "CONNACK",
    0x30: "PUBLISH", 0x40: "PUBACK",
    0x82: "SUBSCRIBE", 0x90: "SUBACK",
    0xE0: "DISCONNECT",
}


def enc_len(n):
    """MQTT 的"剩余长度"字段：每字节 7 位有效，最高位表示后面还有没有。"""
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            break
    return bytes(out)


def mstr(s):
    """MQTT 字符串 = 2 字节长度 + UTF-8 内容（不是 C 语言的 \\0 结尾）。"""
    raw = s.encode("utf-8")
    return len(raw).to_bytes(2, "big") + raw


def dump(tag, data):
    print("  %-12s %3d 字节  %s" % (tag, len(data), data.hex(" ").upper()))


def recv_packet(sock):
    """收一个完整报文：1 字节类型 + 变长长度 + 内容。"""
    head = sock.recv(1)
    if not head:
        return b""
    raw = bytearray(head)
    mult, length = 1, 0
    while True:
        b = sock.recv(1)
        if not b:
            return bytes(raw)
        raw += b
        length += (b[0] & 0x7F) * mult
        mult *= 128
        if not b[0] & 0x80:
            break
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            break
        body += chunk
    return bytes(raw) + body


def send(sock, packet, note):
    kind = packet[0]
    print("\n[发出] %-12s  %s" % (NAMES.get(kind, hex(kind)), note))
    dump("原始字节", packet)
    sock.sendall(packet)


def take(sock, note):
    pkt = recv_packet(sock)
    if not pkt:
        print("\n[收到] 连接被关闭")
        return b""
    kind = pkt[0]
    print("[收到] %-12s  %s" % (NAMES.get(kind, hex(kind)), note))
    dump("原始字节", pkt)
    return pkt


print("=" * 66)
print("目标 broker: %s:%d" % (HOST, PORT))
print("=" * 66)

sock = socket.create_connection((HOST, PORT), timeout=5)
print("\n[步骤 0] TCP 三次握手完成，1883 端口已连通")

# ---------- 步骤 1：CONNECT ----------
# 固定头 0x10 = CONNECT
# 可变头 = 协议名 "MQTT" + 协议级别 0x04(3.1.1) + 连接标志 0x02(clean session) + 心跳 60 秒
# 载荷   = client_id
vh = mstr("MQTT") + bytes([0x04, 0x02]) + (60).to_bytes(2, "big")
body = vh + mstr("raw-demo-001")
send(sock, bytes([0x10]) + enc_len(len(body)) + body, "我是 raw-demo-001，我要连你")
pkt = take(sock, "CONNACK，第 4 字节 00 = 连接成功")

# ---------- 步骤 2：SUBSCRIBE ----------
# 固定头 0x82 = SUBSCRIBE（低 4 位必须是 0010，这是协议硬性规定）
# 可变头 = 报文标识符 0x0001
# 载荷   = 主题过滤器 + 请求的 QoS
body = (0x0001).to_bytes(2, "big") + mstr("inverter/#") + bytes([0x01])
send(sock, bytes([0x82]) + enc_len(len(body)) + body, "我要订阅 inverter/# ，QoS 1")
pkt = take(sock, "SUBACK，最后的 01 = 同意以 QoS 1 授予")

# ---------- 步骤 3：PUBLISH ----------
# 固定头 0x30 = PUBLISH + QoS 0（低 4 位是标志位，QoS 0 时全 0）
# 可变头 = 主题名
# 载荷   = 真正的数据
msg = '{"voltage":10.0,"current":5.00,"power":1750}'
body = mstr("inverter/collector/data") + msg.encode()
send(sock, bytes([0x30]) + enc_len(len(body)) + body, "发一条数据")
pkt = take(sock, "broker 把它转发回来了（因为我订阅了 inverter/#）")

if len(pkt) > 2:
    # 固定头低 4 位是标志位，其中 bit1~bit2 表示 QoS
    qos = (pkt[0] >> 1) & 0x03
    i = 1
    while pkt[i] & 0x80:
        i += 1
    i += 1
    tlen = int.from_bytes(pkt[i:i + 2], "big")
    offset = i + 2 + tlen
    if qos > 0:
        offset += 2          # QoS 1/2 的报文多带 2 字节"报文标识符"
    payload = pkt[offset:].decode("utf-8", "replace")
    print("\n  下发 QoS = %d，所以报文里%s报文标识符" % (qos, "多带了 2 字节" if qos else "没有"))
    print("  >>> 转发回来的数据内容: %s" % payload)

# ---------- 步骤 4：DISCONNECT ----------
send(sock, bytes([0xE0, 0x00]), "我走了")
sock.close()
print("\n" + "=" * 66)
print("全程没有用任何 MQTT 库，只有 socket 和 bytes。")
print("=" * 66)
