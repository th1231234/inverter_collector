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

# 与固件 main/telemetry_codec.h 的 CODEC_MAX_READ_NUM 保持一致
MAX_READ_NUM = 16


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
# 判断一段字节属于哪份 message
#
# 为什么不能只靠"解出来再编回去是否一致"：不同的 message 字段号有重叠，
# 一段 Resp 往往也能当作 Telemetry 解出东西（保护字段名不同但字段号
# 相同），往返比对同样成立，会误判。这里改成扫顶层 tag，看
# (字段号, 线路类型) 是否与 schema 完全吻合，吻合才算。
# ----------------------------------------------------------------------
SCHEMAS = {
    # 字段号: 允许的线路类型集合（0=变长 1=64位 2=长度前缀 5=32位）
    "Telemetry":    {1: {2}, 2: {0}, 3: {0}, 4: {0}, 5: {2}},
    "Resp":         {1: {2}, 2: {0}, 3: {2}, 4: {0, 2}},
    "ReadRequest":  {1: {2}, 2: {0}, 3: {0}, 4: {0}},
    "WriteRequest": {1: {2}, 2: {0}, 3: {0}, 4: {0}},
}


def _varint(buf, i):
    shift = 0
    val = 0
    while True:
        if i >= len(buf) or shift > 63:
            raise ValueError("varint 越界")
        b = buf[i]
        val |= (b & 0x7F) << shift
        i += 1
        if not (b & 0x80):
            return val, i
        shift += 7


def scan_tags(raw):
    """扫顶层字段，返回 [(字段号, 线路类型, 变长字段的值或 None)]。

    结构不完整（截断、非法线路类型）返回 None。
    """
    tags = []
    i = 0
    try:
        while i < len(raw):
            key, i = _varint(raw, i)
            field, wt = key >> 3, key & 7
            if field == 0:
                return None
            val = None
            if wt == 0:
                val, i = _varint(raw, i)
            elif wt == 1:
                i += 8
            elif wt == 2:
                ln, i = _varint(raw, i)
                i += ln
            elif wt == 5:
                i += 4
            else:
                return None
            if i > len(raw):
                return None
            tags.append((field, wt, val))
    except ValueError:
        return None
    return tags


def guess_types(raw):
    """返回可能匹配的 message 名列表（按可信度排序），空列表表示都不匹配。"""
    tags = scan_tags(raw)
    if tags is None:
        return []
    fields = {f for f, _, _ in tags}
    wt_of = {f: wt for f, wt, _ in tags}

    cands = [name for name, tbl in SCHEMAS.items()
             if all(f in tbl and wt in tbl[f] for f, wt, _ in tags)]

    # 语义过滤 1：固件发的 Telemetry 一定带 metrics（字段 5，长度前缀）
    # 和 ts_ms（字段 3）。否则一条 10 字节的 ReadRequest 会被解成
    # "3 个空测点的遥测"，看着像成功其实是胡说。
    if "Telemetry" in cands and (wt_of.get(5) != 2 or 3 not in fields):
        cands.remove("Telemetry")

    # 语义过滤 2：ReadRequest.num 固件限制在 1~16（CODEC_MAX_READ_NUM），
    # 字段 4 的值超过 16 就只能是 WriteRequest.value。
    v4 = next((v for f, wt, v in tags if f == 4 and wt == 0), None)
    if "ReadRequest" in cands and v4 is not None and v4 > MAX_READ_NUM:
        cands.remove("ReadRequest")

    order = ["Telemetry", "Resp", "ReadRequest", "WriteRequest"]
    return [n for n in order if n in cands]


def show_hex(raw: bytes) -> int:
    cands = guess_types(raw)
    if not cands:
        print("四份 message 都对不上：检查是否粘少了字节，或者这根本不是本工程的报文")
        print("hex=%s" % raw.hex())
        return 1

    name = cands[0]
    print("识别为 %s:" % name)
    if name == "Telemetry":
        show_telemetry(raw)
    elif name == "Resp":
        show_resp(raw)
    else:
        cls = pb.ReadRequest if name == "ReadRequest" else pb.WriteRequest
        # 注意：ParseFromString 返回的是「解析了多少字节」，不是消息对象
        msg = cls()
        msg.ParseFromString(raw)
        print(str(msg).rstrip())

    rest = cands[1:]
    if rest:
        print("（结构上也吻合：%s —— 字段号相同，只能结合主题或上下文判断）"
              % " / ".join(rest))
    return 0


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
    # hex 模式下允许不写引号：cmd 会把 0a08 696e ... 拆成多个参数，
    # 这里在 argparse 之前把它们重新粘成一个，否则 argparse 直接报
    # "unrecognized arguments"。其它子命令不受影响。
    argv = sys.argv[1:]
    if "hex" in argv:
        i = argv.index("hex")
        joined = "".join(argv[i + 1:])
        argv = argv[:i + 1] + ([joined] if joined else [])

    ap = argparse.ArgumentParser(description="protobuf 收发与解析")
    ap.add_argument("action", nargs="?", default="listen",
                    choices=["listen", "read", "write", "send", "hex"],
                    help="listen=只订阅解码；read/write=发请求；send=发原始 hex；"
                         "hex=离线解码一段 hex（不连 broker）")
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
    args = ap.parse_args(argv)

    addr = int(args.addr, 0)      # int(x, 0) 才能同时接受 0x3000 和 12288

    # ---- 离线解码：把 MQTTX 里复制的十六进制粘进来，不用连 broker ----
    if args.action == "hex":
        text = (args.hexpayload or "")
        text = text.replace(" ", "").replace("\t", "").replace("\n", "").replace("\r", "")
        if not text:
            print("用法: python tools/proto_decode.py hex 0a08696e7465726e3031...")
            print("      （十六进制可以直接粘，带空格不用加引号）")
            return 1
        try:
            raw = bytes.fromhex(text)
        except ValueError:
            print("不是合法的十六进制串，检查是否粘进了 'Topic:' 之类的文字")
            return 1
        print("输入 %d 字节" % len(raw))
        return show_hex(raw)

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
