"""命令通道探针：发一条 read / write 请求，把 resp 收回来打印。

用法（默认连本地 EMQX，工号 intern01）：
    python cmd_probe.py read 12288 5          # 读 0x3000 起 5 个寄存器
    python cmd_probe.py write 12288 3000      # 写 0x3000 = 3000 (300.0V)
    python cmd_probe.py read 12288 5 abc      # 自定义 req_id

为什么要单独写一个：
    lab_pub.py 发的是固定内容的上报数据，改不了载荷；
    测下行命令需要「发一条自定义 JSON，同时订阅 resp」，这个脚本干这件事。
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

kind = sys.argv[1] if len(sys.argv) > 1 else "read"
addr = int(sys.argv[2]) if len(sys.argv) > 2 else 12288
arg3 = int(sys.argv[3]) if len(sys.argv) > 3 else (5 if kind == "read" else 3000)
req_id = sys.argv[4] if len(sys.argv) > 4 else "probe1"
# 第 5 个参数：从机站号。填一个从机上不存在的站号（如 9）即可造出 modbus_timeout，
# 不必真的去关 Modbus Slave。
slave = int(sys.argv[5]) if len(sys.argv) > 5 else 1

topic = "dev/%s/%s" % (DEV_ID, kind)
payload = {"req_id": req_id, "slave": slave, "addr": addr}
if kind == "read":
    payload["num"] = arg3
else:
    payload["value"] = arg3
body = json.dumps(payload, separators=(",", ":"))

got = []


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print("connect failed rc=%d" % rc)
        return
    # 只订阅，不在这里发请求：SUBSCRIBE 还没被 broker 确认就 publish，
    # 板子的应答可能比 SUBACK 先到，client 侧尚未建立订阅就会漏掉这条 resp。
    client.subscribe("dev/%s/resp" % DEV_ID, qos=1)


def on_subscribe(client, userdata, mid, granted_qos):
    # SUBACK 到达 = 订阅已生效，此时发请求才不会漏应答
    client.publish(topic, body, qos=1)
    print("PUB  %s" % topic)
    print("     %s" % body)


def on_message(client, userdata, msg):
    text = msg.payload.decode("utf-8", errors="replace")
    print("RESP %s" % text)
    got.append(text)


client = mqtt.Client(client_id="cmd-probe-%d" % (int(time.time()) % 10000))
client.username_pw_set(USER, PASS)
client.on_connect = on_connect
client.on_subscribe = on_subscribe
client.on_message = on_message

try:
    client.connect(HOST, PORT, 60)
except Exception as exc:
    print("can not connect %s:%d -> %s" % (HOST, PORT, exc))
    sys.exit(1)

client.loop_start()
time.sleep(6.0)
client.loop_stop()
client.disconnect()

if not got:
    print("--- 6 秒内没收到 resp，检查串口是否有 read ok / bad json ---")
else:
    print("--- 收到 %d 条 resp ---" % len(got))
