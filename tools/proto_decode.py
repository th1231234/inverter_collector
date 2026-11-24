"""PC 端 protobuf 解析工具（阶段三 P-03 / P-04 / P-05 / P-06 的取证手段）。

用法
----
1) 订阅并解码（最常用，边收边打印）：

    python tools/proto_decode.py
    python tools/proto_decode.py --count 3        # 收满 3 条就退出
    python tools/proto_decode.py --only resp      # 只看 resp

2) 用字段拼请求发下去（不用手算十六进制）：

    python tools/proto_decode.py read  --id 1 --addr 0x3000 --num 3
    python tools/proto_decode.py write --id 2 --addr 0x3000 --value 3000

3) 直接发一段十六进制（P-06 边界测试用）：

    python tools/proto_decode.py send read 0a0131

   0a0131 是 ReadRequest 被截断到 3 字节的样子，板子应当打印
   bad ReadRequest (unpack_failed) 并继续工作，不重启、不卡死。

为什么不用 MQTTX 看
------------------
MQTTX 只能显示十六进制，看不出字段；这个脚本用同一份 proto 生成的
Python 代码解析，和固件端 protobuf-c 用的是同一套字段号，解析结果
就是协议本身的意思。

脚本还会把解析出的对象重新序列化一遍，和收到的原始字节逐字节比对：
一致说明「收到的字节没有被截断、没有多出填充」——这就是 P-05
「含 0x00 仍能完整解码」的直接证据。
"""

import argparse
import sys
import time
import warnings

import paho.mqtt.client as mqtt

import telemetry_pb2 as pb

try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

HOST = "192.168.137.1"
PORT = 1883
USER, PASS = "esp32", "123456"
DEV_ID = "intern01"

TOPIC_DATA = "dev/%s/data" % DEV_ID
TOPIC_RESP = "dev/%s/resp" % DEV_ID
TOPIC_READ = "dev/%s/read" % DEV_ID
TOPIC_WRITE = "dev/%s/write" % DEV_ID


# ----------------------------------------------------------------------
# 打印：字段名对齐 proto 定义，数值按 Modbus 习惯给十六进制
# ----------------------------------------------------------------------
def show_telemetry(raw: bytes) -> bool:
    t = pb.Telemetry()
    try:
        t.ParseFromString(raw)
    except Exception as exc:
        print("[data] 解析失败（%d 字节）: %s" % (len(raw), exc))
        print("       hex=%s" % raw.hex())
        return False

    # ts_ms 对时成功是 Unix 毫秒（约 1.7e12），未对时是开机毫秒（很小）
    if t.ts_ms > 1_000_000_000_000:
        ts_note = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t.ts_ms / 1000))
    else:
        ts_note = "开机 %.1f 秒（未对时）" % (t.ts_ms / 1000.0)

    print("[data] device=%s seq=%d state=%d  (%d 字节)" %
          (t.device_id, t.seq, t.state, len(raw)))
    print("       ts_ms=%d -> %s" % (t.ts_ms, ts_note))
    for m in t.metrics:
        print("       addr=0x%04X raw=%d value=%.2f" % (m.addr, m.raw, m.value))

    # 完整性自检：重新序列化应与收到的字节完全一致
    again = t.SerializeToString()
    if again == raw:
        print("       完整性 OK（重新序列化 %d 字节，逐字节一致）" % len(again))
    else:
        print("       完整性 不一致：收到 %d 字节 / 重编码 %d 字节" % (len(raw), len(again)))
    return True


def show_resp(raw: bytes) -> bool:
    r = pb.Resp()
    try:
        r.ParseFromString(raw)
    except Exception as exc:
        print("[resp] 解析失败（%d 字节）: %s" % (len(raw), exc))
        print("       hex=%s" % raw.hex())
        return False

    if r.ok:
        if r.values:
            vals = ", ".join("%d(0x%04X)" % (v, v) for v in r.values)
            print("[resp] req_id=%s ok=true values=[%s]  (%d 字节)" %
                  (r.req_id, vals, len(raw)))
        else:
            print("[resp] req_id=%s ok=true  (%d 字节)" % (r.req_id, len(raw)))
    else:
        print("[resp] req_id=%s ok=false msg=%s  (%d 字节)" %
              (r.req_id, r.msg, len(raw)))
    return True


# ----------------------------------------------------------------------
# 组请求
# ----------------------------------------------------------------------
def build_read(req_id: str, slave: int, addr: int, num: int) -> bytes:
    m = pb.ReadRequest(req_id=req_id, slave=slave, addr=addr, num=num)
    return m.SerializeToString()


def build_write(req_id: str, slave: int, addr: int, value: int) -> bytes:
    m = pb.WriteRequest(req_id=req_id, slave=slave, addr=addr, value=value)
    return m.SerializeToString()


# ----------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="protobuf 收发与解析")
    ap.add_argument("action", nargs="?", default="listen",
                    choices=["listen", "read", "write", "send"],
                    help="listen=只订阅解码；read/write=发请求；send=发原始 hex")
    ap.add_argument("hexpayload", nargs="?", default=None,
                    help="send 动作的十六进制载荷")
    ap.add_argument("--id", default="1", help="req_id（默认 1）")
    ap.add_argument("--slave", type=int, default=1, help="从机站号（默认 1，填 9 可造超时）")
    ap.add_argument("--addr", default="0x3000", help="寄存器地址，支持 0x 前缀（默认 0x3000）")
    ap.add_argument("--num", type=int, default=3, help="read 读取个数（默认 3）")
    ap.add_argument("--value", type=int, default=3000, help="write 写入值（默认 3000）")
    ap.add_argument("--count", type=int, default=0, help="收满 N 条后退出，0=一直跑")
    ap.add_argument("--only", choices=["data", "resp"], default=None, help="只订阅上行主题之一")
    ap.add_argument("--topic", choices=["read", "write"], default="read",
                    help="send 动作发到哪个下行主题（默认 read）")
    ap.add_argument("--hex", action="store_true", help="每条消息额外打印原始十六进制")
    args = ap.parse_args()

    addr = int(args.addr, 0)      # int(x, 0) 才能同时接受 0x3000 和 12288

    got = {"n": 0}
    wait_suback = {"done": False}

    def on_connect(client, userdata, flags, rc):
        if rc != 0:
            print("连接失败 rc=%d（检查 broker 地址与用户名密码）" % rc)
            return
        if args.only != "resp":
            client.subscribe(TOPIC_DATA, qos=1)
        if args.only != "data":
            client.subscribe(TOPIC_RESP, qos=1)

    def on_subscribe(client, userdata, mid, granted_qos):
        # SUBACK 到了才发请求：订阅没生效就 publish，板子的应答会漏掉
        wait_suback["done"] = True

    def on_message(client, userdata, msg):
        raw = msg.payload
        if msg.topic == TOPIC_DATA:
            ok = show_telemetry(raw)
        elif msg.topic == TOPIC_RESP:
            ok = show_resp(raw)
        else:
            print("[?] 未预期的主题 %s" % msg.topic)
            return
        # --hex：把原始字节打出来，P-05 要证明「报文里有 0x00 也没被截断」
        if args.hex and ok:
            print("       hex=%s" % raw.hex())
        got["n"] += 1
        if args.count and got["n"] >= args.count:
            client.disconnect()

    # paho-mqtt 2.x 用 VERSION1 会打一条 DeprecationWarning，而 1.x 又没有
    # CallbackAPIVersion 这个枚举。为了同一份脚本在 1.x / 2.x 都能跑，
    # 这里固定用 1.x 风格回调，只把那条告警静音（不影响行为）。
    warnings.filterwarnings("ignore", category=DeprecationWarning)
    client = mqtt.Client(client_id="proto-decode-%d" % (int(time.time()) % 100000))
    client.username_pw_set(USER, PASS)
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message

    try:
        client.connect(HOST, PORT, 60)
    except Exception as exc:
        print("连不上 %s:%d -> %s" % (HOST, PORT, exc))
        return 1

    client.loop_start()

    # ---- 发请求（如果是 read / write / send） ----
    if args.action != "listen":
        for _ in range(60):                 # 最多等 3 秒 SUBACK
            if wait_suback["done"]:
                break
            time.sleep(0.05)

        if args.action == "read":
            topic, body = TOPIC_READ, build_read(args.id, args.slave, addr, args.num)
        elif args.action == "write":
            topic, body = TOPIC_WRITE, build_write(args.id, args.slave, addr, args.value)
        else:
            # send：载荷原样来自 --hex，用来发固件不该接受的东西
            topic = TOPIC_WRITE if args.topic == "write" else TOPIC_READ
            body = bytes.fromhex(args.hexpayload or "")

        if not body:
            print("载荷为空，检查参数")
            client.loop_stop()
            return 1

        print("PUB  %s  (%d 字节)  hex=%s" % (topic, len(body), body.hex()))
        client.publish(topic, body, qos=1)

    # ---- 收 ----
    deadline = time.time() + 30
    while True:
        time.sleep(0.2)
        if args.count and got["n"] >= args.count:
            break
        if time.time() > deadline:
            break

    client.loop_stop()
    client.disconnect()
    print("--- 共收到 %d 条 ---" % got["n"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
