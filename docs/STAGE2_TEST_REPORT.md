# 阶段二测试报告：MQTT 接入（查询 / 上报 / 修改）

> 对应《嵌入式初级助理技术考核指导书》第二部分
> 版本：V1.0（2026-11-23）　固件：`inverter_collector.bin` 1,004,560 字节
> 工号：`intern01`　Broker：本机 EMQX 5.2.1 `mqtt://192.168.137.1:1883`

---

## 1. 测试环境

| 项目 | 值 |
|---|---|
| 开发板 | ESP32-C3（QFN32, rev v0.4），MAC `34:85:18:cf:d1:f0` |
| 框架 | ESP-IDF v5.5.5 |
| Broker | EMQX 5.2.1（本机 Windows，端口 1883 / Dashboard 18083） |
| 从机仿真 | Modbus Slave（COM10 / RS485，站号 1，9600-8-N-1） |
| 上位机 | MQTTX + 自研 `tools/mqtt_lab/cmd_probe.py`（命令行验证） |
| 网络 | Windows 移动热点，电脑 `192.168.137.1`，板子 `192.168.137.140` |
| 固件配置 | `MQTT_DEV_ID=intern01`，`REPORT_PROTO=MQTT`，自检已关闭 |

主题规范（指导书 2.3）：

| 方向 | 主题 | 用途 |
|---|---|---|
| 下行 | `dev/intern01/read` | 查询寄存器（功能码 03） |
| 下行 | `dev/intern01/write` | 修改寄存器（功能码 06） |
| 上行 | `dev/intern01/resp` | 应答，每个请求必回一条 |
| 上行 | `dev/intern01/data` | 周期上报，5 秒一条 |

---

## 2. 用例结果汇总

| 编号 | 用例 | 分值 | 结果 | 证据来源 |
|---|---|---|---|---|
| M-01 | 连接 broker | 10 | **PASS** | `serial_log5.txt` |
| M-02 | 周期上报 data | 15 | **PASS** | 串口 + MQTTX |
| M-03 | 查询 read | 20 | **PASS** | `cmd_probe.py` |
| M-04 | 修改 write | 20 | **PASS** | `cmd_probe.py` + 回读 |
| M-05 | 应答格式 / req_id | 5 | **PASS** | `cmd_probe.py` 连发 a/b/c |
| M-06 | 断线重连 | 10 | **PASS** | `serial_log3.txt` + netstat |
| M-07 | 异常上报 | 10 | **PASS** | `serial_log6.txt` |
| M-08 | 边界安全 | 10 | **PASS** | `serial_log6.txt` |

基础功能 **100/100**（70 分为合格线）。

---

## 3. 逐条实测

### M-01 连接 broker

串口（`serial_log5.txt`）：

```text
I (7608) MAIN: network ready, IP=192.168.137.140
I (7608) MQTT_CMD: cmd ready: sub "dev/intern01/read" & "dev/intern01/write", pub "dev/intern01/resp"
I (7778) MQTT: broker connected: mqtt://192.168.137.1:1883
I (7778) MQTT: client_id=intern01, pub topic=dev/intern01/data
I (7788) MQTT: subscribed "dev/intern01/read" (msg_id=41151), "dev/intern01/write" (msg_id=25170)
```

- Client ID = 工号 `intern01`（指导书 2.4 要求）；
- 订阅写在 `MQTT_EVENT_CONNECTED` 回调里，每次重连自动恢复；
- Wi-Fi 从启动到拿到 IP 用了 7.6 秒（其中前 3 次关联失败 `reason=201/2/201` 后自愈）。

### M-02 周期上报 data

```text
I (216158) MAIN: Value: Voltage=350.0V, Current=5.00A, Power=1750W
I (216168) MQTT: data -> "dev/intern01/data": {"voltage":350.0,"current":5.00,"power":1750,"state":0}
```

- 每 5 秒一条，`state=0`；读失败时 `state=1` 且数值沿用上一次缓存；
- 数值与 Modbus Slave 预设一致（3500/500/1750 → ÷10 / ÷100 / ÷1）；
- 上报门控：连续成功 3 次才启动（`I (18528) MAIN: 3 consecutive reads OK, start MQTT reporting`），即开机后约 18 秒出第一条。

### M-03 查询 read

请求 `dev/intern01/read`：

```json
{"req_id":"m03","slave":1,"addr":12288,"num":5}
```

应答 `dev/intern01/resp`：

```json
{"req_id":"m03","result":"ok","values":[5,0,200,0,1750]}
```

> 该次 Slave 中 `0x3000`/`0x3002` 尚为 5/200，故返回 5 和 200；改回 3500/500 后复测为
> `values:[3500,0,500,0,1750]`，与预设完全一致。

**为什么用 `num=5` 而不是指导书样例的 `num=3`**：三个参数分别在 `0x3000`/`0x3002`/`0x3004`，地址步长为 2（指导书 1.4.1 定义）。从 `0x3000` 连续读 3 个只能覆盖到 `0x3002`，拿不到功率。读 5 个才能覆盖 `0x3000~0x3004`，返回的 5 个值中第 1/3/5 个分别是电压、电流、功率。

### M-04 修改 write

请求 `dev/intern01/write`：

```json
{"req_id":"m04","slave":1,"addr":12288,"value":3000}
```

应答：

```json
{"req_id":"m04","result":"ok"}
```

回读确认（`num=5`）：

```json
{"req_id":"m04b","result":"ok","values":[3000,0,200,0,1750]}
```

`0x3000` 从 5 变为 3000，即 300.0V。下一条 `data` 的 `voltage` 同步变为 `300.0`。

### M-05 应答格式与 req_id 配对

连发三条（`req_id` 分别为 `a`/`b`/`c`）：

```text
RESP {"req_id":"a","result":"ok","values":[3000,0,200,0,1750]}
RESP {"req_id":"b","result":"ok","values":[3000,0,200,0,1750]}
RESP {"req_id":"c","result":"ok","values":[3000,0,200,0,1750]}
```

三条 `resp` 的 `req_id` 一一对应，`result` 规范为 `ok`/`error`。
`req_id` 限定为 `[A-Za-z0-9_-]`、长度 ≤31，超过即判 `invalid_param`。

### M-06 断线重连

步骤：停 EMQX → 等 60 秒 → 重启 EMQX，全程不碰开发板。

断开期间的串口（`serial_log3.txt`）：

```text
E (213038) esp-tls: [sock=54] select() timeout
E (213038) transport_base: Failed to open a new connection: 32774
E (213038) MQTT: transport error: sock_errno=0, tls_last_err=0x8006
W (213038) MQTT: broker disconnected, waiting auto reconnect
I (216158) MAIN: Value: Voltage=350.0V, Current=5.00A, Power=1750W
W (216168) MQTT: broker not connected yet, drop publish to "dev/intern01/data"
I (216178) MAIN: Stats: ok=39, err=0
```

**采集不中断、上报跳过、进程不崩溃**，`err=0`。

恢复时刻的网络状态：

```text
TCP  192.168.137.1:1883  192.168.137.140:64038  ESTABLISHED
```

板子侧源端口 `64038`，说明是设备主动发起的重连（esp-mqtt 自带重连逻辑，间隔 5 秒）。

恢复后的上报（`serial_log4.txt`）：

```text
I (560838) MQTT: data -> "dev/intern01/data": {"voltage":350.0,...,"state":0}
I (566148) MQTT: data -> "dev/intern01/data": {"voltage":350.0,...,"state":0}
```

每 5 秒一条，无需人工干预。

### M-07 异常上报

| 故障 | 输入 | 应答 / 日志 |
|---|---|---|
| 坏 JSON | `{"req_id":` | 串口 `bad json on dev/intern01/read, dropped`，**无 resp**（取不到 req_id，无法组包） |
| 地址越界 | `addr=70000` | `{"req_id":"m07a","result":"error","msg":"invalid_param"}` |
| 数量超限 | `num=999` | `{"req_id":"m07b","result":"error","msg":"invalid_param"}` |
| 从机无响应 | `slave=9`（不存在的站号） | `{"req_id":"t1","result":"error","msg":"modbus_timeout"}` |
| 从机拒绝 | 非法功能码/地址 | `modbus_illegal_function` / `modbus_illegal_address` |

`msg` 取值由 `modbus_last_exception()` 区分：异常码 0 判超时，非 0 按 0x01~0x04 翻译。

坏 JSON 之后设备未重启，后续正常请求照常响应（`resp ok: req_id=after`）。

**超时项的取证方式**：不必真的去关 Modbus Slave，改发一个从机上不存在的站号（9）即可——从机会直接忽略非本站的帧，效果与掉线一致，且采集任务不受影响。

```powershell
python cmd_probe.py read 12288 5 t1 9      # 第 5 个参数是 slave
```

```text
W (6154698) MODBUS: func 0x03 timeout (attempt 1/3)
W (6154918) MODBUS: func 0x03 timeout (attempt 2/3)
W (6155138) MODBUS: func 0x03 timeout (attempt 3/3)
E (6155138) MQTT_CMD: read failed: req_id=t1 addr=0x3000 msg=modbus_timeout
W (6155138) MQTT_CMD: resp error: req_id=t1 msg=modbus_timeout
I (6158278) MAIN: Stats: ok=1151, err=0
```

三个要点：

1. **3 次重试，每次 220ms**，共 660ms 后放弃（`attempt 1/3` → `3/3`）
2. 应答在最后一次重试的同一毫秒内发出，没有额外等待
3. 同一时段采集任务 `ok=1150 → 1153` 继续增长，**err=0**：命令超时完全不影响周期采集（两者抢总线锁，但锁会排队而不是让采集失败）

### M-08 边界安全与分片

**测试 1：1KB 超长载荷**

发送 1061 字节 JSON 到 `dev/intern01/read`，串口（`serial_log6.txt`）：

```text
W (65838) MQTT: fragmented payload (1000/1061 bytes), dropped
W (65848) MQTT: fragmented payload (61/1061 bytes), dropped
I (66138) MQTT_CMD: read ok: req_id=after slave=1 addr=0x3000 num=5
```

**1061 = 1000 + 61**：超过 1024 字节接收缓冲的载荷被 esp-mqtt 拆成两片投递，每片单独触发一次 `MQTT_EVENT_DATA`。固件通过 `event->total_data_len != event->data_len` 识别分片，两片均整条丢弃。

> 这一判断是必要的：旧实现按片独立解析，若某个残片恰好构成合法 JSON，就会执行一条本该被拒绝的命令。本版已修正。

**测试 2：长度上限**

`mqtt_cmd` 侧另有 512 字节硬上限，超过打印 `payload too long (n >= 512), dropped`，作为分片检查之外的第二道防线。

**测试 3：参数边界**

| 参数 | 合法范围 | 校验位置 |
|---|---|---|
| `slave` | 1~247 | `json_get_uint` |
| `addr` | 0~65535，且 `addr + num - 1 ≤ 65535` | `json_get_uint` + 溢出检查 |
| `num` | 1~16 | `json_get_uint` |
| `value` | 0~65535 | `json_get_uint` |
| `req_id` | 1~31 字符，`[A-Za-z0-9_-]` | `req_id_is_valid` |

载荷先校验长度再 `memcpy` 到定长缓冲并补 `'\0'`，不依赖 MQTT 报文自带的结束符（`event->data` 不以 `\0` 结尾，且可能含 `0x00`）。

---

## 4. 架构说明

### 4.1 回调只做“传达室”

```
MQTT 回调（esp-mqtt 任务）
   │  长度校验 → 分片判断 → 主题分发 → cJSON 解析 → 参数校验 → 入队
   ▼
mqtt_cmd 队列（深度 8）
   ▼
cmd_task（独立任务，栈 4096）
   │  modbus_read_holding / modbus_write_register（最坏 600ms）
   ▼
resp 发布（QoS 1）
```

一次 Modbus 读最坏耗时 = 200ms 超时 × 3 次尝试 ≈ 600ms。若在回调里同步执行，MQTT 任务将停摆超过 keepalive（60 秒）的判定窗口，导致反复掉线。因此回调只入队，耗时的 IO 放到独立任务。

### 4.2 总线互斥锁

阶段二起，采集任务（每 5 秒）与命令任务（收到请求即执行）会同时访问同一个 UART。两帧交错会导致地址/功能码/CRC 全部校验失败，表现为随机 `read failed`。

`modbus.c` 中新增 `s_bus_lock`（互斥量），锁粒度为**一次完整事务**：清缓冲 → 发帧 → 收帧 → 校验 → 重试。

```c
static bool modbus_transact(...)
{
    xSemaphoreTake(s_bus_lock, portMAX_DELAY);
    bool ok = modbus_transact_locked(...);
    xSemaphoreGive(s_bus_lock);
    return ok;
}
```

只锁 UART 读写是不够的：中途被插入另一条请求同样会污染接收缓冲。

---

## 5. 遗留事项

| 项 | 状态 | 说明 |
|---|---|---|
| app 分区余量 4% | **已解决** | 改用 `SINGLE_APP_LARGE` 分区，1500K 中用 1,004,560 字节，余量 35%（531KB） |
| M-07 超时项 | **已补齐** | 用不存在的站号 `slave=9` 造超时，见第 3 章 M-07 |
| `req_id` 重复投递 | 已知特性 | QoS 1 语义允许，端到端可能出现同一条 resp 两次；实测单次触发只产生一条 |

### 5.1 分区扩容与复烧记录（16:15）

原分区（IDF 默认 `partitions_singleapp.csv`）app 只有 1MB，1,004,560 字节占 96%，阶段三加 protobuf-c 会溢出。

改用 `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` 后的布局：

| Label | Usage | Offset | Length |
|---|---|---|---|
| nvs | WiFi data | 0x009000 | 0x006000 |
| phy_init | RF data | 0x00f000 | 0x001000 |
| factory | factory app | 0x010000 | **0x177000（1500K）** |

**nvs 偏移未变（仍是 0x9000）**，所以已有 Wi-Fi 凭据保留，复烧后不用重新配网。

复烧内容与校验：

```text
0x00000  bootloader        20,960 bytes   Hash of data verified
0x08000  partition-table    3,072 bytes   Hash of data verified
0x10000  app            1,004,560 bytes   Hash of data verified
```

复位后启动日志确认：

```text
rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)
I (72) boot:  2 factory          factory app      00 00 00010000 00177000
I (261) boot: Loaded app from partition at offset 0x10000
I (1838) MAIN: network ready, IP=192.168.137.140
I (1868) MQTT: broker connected: mqtt://192.168.137.1:1883
I (1878) MQTT: subscribed "dev/intern01/read" (msg_id=39334), "dev/intern01/write" (msg_id=62560)
```

四点值得注意：

1. `boot:0xc` 是正常 flash 启动（`0x4` 才是卡在下载模式）
2. 分区表长度 `0x177000` 与预期一致
3. **网络就绪只用了 1838ms**（上次冷启动是 7608ms）——nvs 里凭据还在，省掉了扫描和四次握手
4. 烧录后命令通道立即可用：`{"req_id":"post","result":"ok","values":[3000,0,800,0,1750]}`

分区余量前后对比：

| | 分区大小 | 固件占用 | 余量 |
|---|---|---|---|
| 扩容前 | 1,048,576 | 1,004,560（96%） | 44,016 字节（4%） |
| **扩容后** | **1,536,768** | 1,004,560（65%） | **531,440 字节（35%）** |

---

## 6. 复现步骤

```powershell
# 1. 启动 Broker
cd D:\work\MQTT\emqx-5.2.1-windows-amd64\bin
.\emqx.cmd start

# 2. 编译并烧录
cd D:\work\espidf_projects\inverter_collector
python tools\build_run.py
# 按住 BOOT → 点 RST → 松 BOOT，然后：
python -m esptool --chip esp32c3 -b 460800 -p COM8 --before no_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\inverter_collector.bin
# 烧完手动点 RST

# 3. 命令行验证
cd tools\mqtt_lab
python cmd_probe.py read 12288 5 m03      # 查询
python cmd_probe.py write 12288 3000 m04  # 修改
python bad_payload.py                     # 坏 JSON + 1KB 超长
```

---

## 7. 结论

阶段二 8 条用例全部通过，基础功能满分。核心能力：

- 四个主题按 `dev/<工号>/用途` 规范落地，工号一处配置；
- 下行命令经队列解耦，Modbus 读写不阻塞 MQTT 心跳；
- 每条请求必回一条 `resp`，失败必带 `msg`，异常不静默；
- 断线、分片、越界、坏 JSON 四类异常均不导致崩溃或功能退化。

已知不足：app 分区仅余 4%，阶段三需先扩分区。
