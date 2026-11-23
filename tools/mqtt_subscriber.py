"""MQTT 订阅端：接收 ESP32 上报的 JSON 并打印。

用法：
    pip install paho-mqtt
    python mqtt_subscriber.py
    python mqtt_subscriber.py broker.emqx.io 1883 inverter/collector/data

默认参数与 Kconfig 中的默认值一致，板子端不用改也能对得上。
"""

import json
import sys
import time

import paho.mqtt.client as mqtt

BROKER = sys.argv[1] if len(sys.argv) > 1 else "broker.emqx.io"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 1883
TOPIC = sys.argv[3] if len(sys.argv) > 3 else "inverter/collector/data"

count = 0


def on_connect(client, userdata, flags, rc, *extra):
    if rc == 0:
        print("[connected] %s:%d" % (BROKER, PORT))
        print("[subscribe] %s" % TOPIC)
        print("-" * 60)
        client.subscribe(TOPIC, qos=1)
    else:
        print("[connect failed] rc=%s" % rc)


def on_message(client, userdata, msg):
    global count
    count += 1
    raw = msg.payload.decode("utf-8", errors="replace")
    stamp = time.strftime("%H:%M:%S")

    try:
        data = json.loads(raw)
        print("[%s] #%d  voltage=%.1fV current=%.2fA power=%.0fW"
              % (stamp, count, data["voltage"], data["current"], data["power"]))
    except Exception:
        print("[%s] #%d  %s" % (stamp, count, raw))


def on_disconnect(client, userdata, rc, *extra):
    print("[disconnected] rc=%s, retrying ..." % rc)


# 兼容 paho-mqtt 1.x 与 2.x
try:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
except AttributeError:
    client = mqtt.Client()

client.on_connect = on_connect
client.on_message = on_message
client.on_disconnect = on_disconnect

print("connecting to %s:%d ..." % (BROKER, PORT))
client.connect(BROKER, PORT, keepalive=60)

try:
    client.loop_forever()
except KeyboardInterrupt:
    print("\nstopped, %d messages received" % count)
    client.disconnect()
