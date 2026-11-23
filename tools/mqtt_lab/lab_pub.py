"""MQTT 发布端（实验用），模拟 ESP32 上报逆变器数据。

做的事情：
    1. 连上 broker
    2. 往指定主题发 N 条 JSON
    3. 每条都检查 broker 的确认（puback），打印 msg_id

用法：
    python lab_pub.py                                   # 发 5 条到 127.0.0.1
    python lab_pub.py 127.0.0.1 1883 inverter/collector/data 10 2
    #                                       主题            条数 间隔秒
    python lab_pub.py 127.0.0.1 1883 inverter/collector/data 5 1 esp32 123456
    #                                                            └─ 账号 ─┘

为什么用 QoS 1：
    QoS 0 = 发了就不管（可能丢）
    QoS 1 = broker 必须回 PUBACK 确认（至少一次，可能重复）
    QoS 2 = 四次握手（恰好一次，最慢）
    上报采集数据用 1 最划算。
"""

import json
import sys
import time

import paho.mqtt.client as mqtt

# 同样是防输出缓冲：程序跑着的时候也要能立刻看到打印
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
TOPIC = sys.argv[3] if len(sys.argv) > 3 else "inverter/collector/data"
COUNT = int(sys.argv[4]) if len(sys.argv) > 4 else 5
INTERVAL = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
USER = sys.argv[6] if len(sys.argv) > 6 else ""
PASS = sys.argv[7] if len(sys.argv) > 7 else ""


def on_connect(client, userdata, flags, rc, properties=None):
    code = rc.value if hasattr(rc, "value") else int(rc)
    print("[已连接] %s:%d  (rc=%s)" % (HOST, PORT, code))


CID = "lab-pub-%d" % (int(time.time()) % 100000)
try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CID)
except AttributeError:
    client = mqtt.Client(client_id=CID)

client.on_connect = on_connect

if USER:
    client.username_pw_set(USER, PASS)
    print("使用账号认证: %s" % USER)

print("正在连接 %s:%d ..." % (HOST, PORT))
client.connect(HOST, PORT, keepalive=60)
client.loop_start()          # 后台线程跑网络循环，主线程才能 sleep 着发数据
time.sleep(0.5)              # 等 on_connect 打印出来

print("开始发布 %d 条到 \"%s\"，间隔 %.1f 秒" % (COUNT, TOPIC, INTERVAL))
print("-" * 64)

try:
    for i in range(1, COUNT + 1):
        # 模拟逆变器读数：电压 10.0V 起、电流 5.00A 起，每轮稍微变化
        data = {
            "seq": i,
            "voltage": round(10.0 + i * 0.3, 1),
            "current": round(5.00 + i * 0.11, 2),
            "power": round((10.0 + i * 0.3) * (5.00 + i * 0.11), 0),
            "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
        }
        payload = json.dumps(data, ensure_ascii=False)

        info = client.publish(TOPIC, payload, qos=1)
        info.wait_for_publish(timeout=5)      # 等 broker 回 PUBACK

        if info.is_published():
            print("#%d  msg_id=%-3d  已确认  %s" % (i, info.mid, payload))
        else:
            print("#%d  msg_id=%-3d  未确认(超时)  %s" % (i, info.mid, payload))

        if i < COUNT:
            time.sleep(INTERVAL)
except KeyboardInterrupt:
    print("\n用户中断")

time.sleep(0.3)
client.loop_stop()
client.disconnect()
print("-" * 64)
print("发布结束，共 %d 条" % COUNT)
