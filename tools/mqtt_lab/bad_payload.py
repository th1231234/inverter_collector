"""异常载荷投递：验证设备收到坏数据时不崩溃、不静默。

依次发送三类：
    1. 坏 JSON       {"req_id":            -> 串口应打印 bad json on ...，无 resp
    2. 超长载荷      约 1KB 的 JSON        -> 串口应打印 payload too long，无 resp
    3. 正常请求      合法 read             -> 必须仍能正常回 resp，证明设备没被搞死

用法：
    python bad_payload.py
"""

import json
import sys
import time

import paho.mqtt.client as mqtt

try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

HOST = "192.168.137.1"
PORT = 1883
USER, PASS = "esp32", "123456"
DEV_ID = "intern01"

TOPIC_READ = "dev/%s/read" % DEV_ID
TOPIC_RESP = "dev/%s/resp" % DEV_ID

# 约 1KB：正常字段 + 一大段填充，模拟上位机发错或故意灌数据
big = {"req_id": "big1", "slave": 1, "addr": 12288, "num": 5,
       "padding": "x" * 1000}
BIG_BODY = json.dumps(big, separators=(",", ":"))
BAD_BODY = '{"req_id":'
GOOD_BODY = json.dumps({"req_id": "after", "slave": 1, "addr": 12288, "num": 5},
                       separators=(",", ":"))

resp = []


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print("connect failed rc=%d" % rc)
        return
    client.subscribe(TOPIC_RESP, qos=1)

    print("[1] 坏 JSON，%d 字节" % len(BAD_BODY))
    client.publish(TOPIC_READ, BAD_BODY, qos=1)
    time.sleep(1.5)

    print("[2] 超长载荷，%d 字节" % len(BIG_BODY))
    client.publish(TOPIC_READ, BIG_BODY, qos=1)
    time.sleep(1.5)

    print("[3] 正常请求（验证设备仍可用）")
    client.publish(TOPIC_READ, GOOD_BODY, qos=1)


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", errors="replace")
    print("RESP %s" % text)
    resp.append(text)


client = mqtt.Client(client_id="bad-payload-%d" % (int(time.time()) % 10000))
client.username_pw_set(USER, PASS)
client.on_connect = on_connect
client.on_message = on_message
client.connect(HOST, PORT, 60)
client.loop_start()
time.sleep(6.0)
client.loop_stop()
client.disconnect()

print("--- 共收到 %d 条 resp（期望 1 条，即第 3 步的正常请求）---" % len(resp))
