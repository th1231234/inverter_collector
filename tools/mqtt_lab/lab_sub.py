"""MQTT 订阅端（实验用）。

做的事情只有三件：
    1. 连上 broker
    2. 订阅一个主题
    3. 每当有人往这个主题发消息，就把内容打印出来

用法：
    python lab_sub.py                                    # 默认 127.0.0.1:1883, 订阅 inverter/#
    python lab_sub.py 127.0.0.1 1883 inverter/collector/data
    python lab_sub.py 127.0.0.1 1883 "inverter/#" esp32 123456   # EMQX 需要账号时

注意：`inverter/#` 里的 `#` 是通配符，表示"inverter/ 下面所有层级"，
      `inverter/+` 只匹配一层，`#` 匹配任意多层。
"""

import sys
import time

import paho.mqtt.client as mqtt

# 重要：程序在终端里长时间运行时，Python 默认会把输出攒够 8KB 才刷出来，
# 表现就是"明明收到了消息，屏幕上却一片空白"。强制按行刷新即可。
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
TOPIC = sys.argv[3] if len(sys.argv) > 3 else "inverter/#"
USER = sys.argv[4] if len(sys.argv) > 4 else ""
PASS = sys.argv[5] if len(sys.argv) > 5 else ""

count = 0


def rc_code(rc):
    """paho 1.x 传 int，2.x 传 ReasonCode 对象，统一成 int。"""
    return rc.value if hasattr(rc, "value") else int(rc)


def on_connect(client, userdata, flags, rc, properties=None):
    code = rc_code(rc)
    if code != 0:
        print("[连接失败] rc=%s" % code)
        return
    print("[已连接] %s:%d" % (HOST, PORT))
    client.subscribe(TOPIC, qos=1)
    print("[已订阅] %s   (qos=1)" % TOPIC)
    print("-" * 64)


def on_message(client, userdata, msg):
    global count
    count += 1
    payload = msg.payload.decode("utf-8", errors="replace")
    print("[%s] #%-3d topic=%-28s qos=%d retain=%s"
          % (time.strftime("%H:%M:%S"), count, msg.topic, msg.qos, msg.retain))
    print("         payload: %s" % payload)


def on_disconnect(client, userdata, *args):
    print("[已断开] %s" % (args,))


CID = "lab-sub-%d" % (int(time.time()) % 100000)

# 兼容 paho-mqtt 1.x / 2.x 两种 API
try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CID)
except AttributeError:
    client = mqtt.Client(client_id=CID)

client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

if USER:
    client.username_pw_set(USER, PASS)
    print("使用账号认证: %s" % USER)

print("正在连接 %s:%d ..." % (HOST, PORT))
client.connect(HOST, PORT, keepalive=60)

try:
    client.loop_forever()      # 阻塞在这里，有消息就回调 on_message
except KeyboardInterrupt:
    print("\n已停止，共收到 %d 条消息" % count)
    client.disconnect()
