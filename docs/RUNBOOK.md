# 全流程操作手册：从编译烧录到四主题验收

> 适用：`main` 分支（阶段三，MQTT + Protobuf）、ESP32-C3、Windows + ESP-IDF 5.5.5
> 目标：照着从上往下做，能把整个工程从「代码在硬盘上」跑到「MQTTX 里看到遥测帧、发命令能收到应答」

---

## 0. 一页速查

每次开机要重做的最小集合，只有 5 步：

```text
① 开移动热点        → PC 得到 192.168.137.1
② 起 EMQX           → emqx.cmd console（前台窗口，别关）
③ 起 Modbus 从机    → python tools\modbus_slave_sim.py COM10 9600 1
④ 板子按 RST        → 串口日志出现 "MQTT: subscribed ..."
⑤ MQTTX 连接        → 订阅 dev/intern01/#
```

改代码之后才需要加一步「编译 + 烧录」。

---

## 1. 环境自检（只做一次）

### 1.1 三样东西必须在

| 项目 | 位置 / 检查命令 |
|---|---|
| ESP-IDF v5.5.5 | `D:\work\Espressif\frameworks\esp-idf-v5.5.5` |
| IDF 专用 Python | `D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe` |
| EMQX 5.2.1 | `D:\work\MQTT\emqx-5.2.1-windows-amd64` |

### 1.2 每次开新终端都要激活 IDF

**必须用 PowerShell，不能用 bash**（bash 里的 `export.sh` 会让 `idf.py` 报 `No module named 'click'`）：

```powershell
cd D:\work\Espressif\frameworks\esp-idf-v5.5.5
.\export.ps1
```

如果报「禁止运行脚本」：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
```

激活成功的标志：`idf.py --version` 能输出版本号。

### 1.3 串口要认两个

| 串口 | 用途 | 谁占用 |
|---|---|---|
| **COM8** | 烧录 + 控制台日志（UART0） | `idf.py flash` / 串口脚本 |
| **COM10** | Modbus 总线（USB 转 485） | 从机模拟器 / Modbus Slave |

同一个串口**不能同时被两个程序打开**，否则报 `port is busy` / `Access is denied`。

---

## 2. 硬件接线

### 2.1 两路串口完全独立，不要混

| 用途 | 板上引脚 | 接到哪 |
|---|---|---|
| **烧录 / 日志**（UART0） | `GPIO21`(TX0)、`GPIO20`(RX0)、`GND` | USB 转 TTL 线 → **COM8** |
| **Modbus**（UART1） | `GPIO5`(TX)、`GPIO6`(RX)、`GND` | TTL 转 485 → USB 转 485 → **COM10** |

> 接到 USB 转 485 之前，A 接 A、B 接 B，**GND 必须共地**。桌上短距离不用 120 Ω 终端电阻。

### 2.2 烧录线的限制（决定了烧录怎么操作）

这条线只有 **VCC / GND / TX / RX 四根，没有 DTR、没有 RTS**。后果有两个，都必须记住：

1. esptool **无法自动让芯片进下载模式** → 要手动按住 BOOT 再点 RST
2. 烧录结束那行 `Hard resetting via RTS pin` 是**空喊**（没接线）→ 烧完要手动按一次 RST，否则程序不跑

---

## 3. 启动顺序（重要：PC 侧先，板子后）

板子上电后会立刻连 Wi-Fi 和 broker。如果这时候 broker 还没起，它只能一直重连 —— 虽然会自动恢复，但日志会很脏、容易误判成故障。

```text
① 开热点          ② 起 EMQX        ③ 起从机           ④ 板子上电
   ↓                 ↓                 ↓                  ↓
192.168.137.1     1883 开始监听     COM10 开始应答     连 Wi-Fi → 连 broker
```

### ① 开移动热点

固件里 broker 地址写的是 `mqtt://192.168.137.1:1883`，这是 **Windows 移动热点的网卡地址**，不是固定 IP。

- 设置 → 网络和 Internet → 移动热点 → 打开
- 确认：`ipconfig` 里能看到 `192.168.137.1`
- 板子连的就是这个热点（SSID / 密码在 `menuconfig → Inverter Collector → Wi-Fi`）

> 热点一关，这个地址就消失，板子会表现为「一直重连但 PC 上 EMQX 明明在跑」。

### ② 起 EMQX

```bat
cd /d D:\work\MQTT\emqx-5.2.1-windows-amd64\bin
emqx.cmd console
```

- **必须用 `console`，不要用 `start`**。`start` 分支是 `start werl` 开新窗口，从 PowerShell / 后台调用会静默失败
- `console` 是前台运行，这个窗口**不能关**，关了 broker 就停了
- 验证：浏览器打开 http://127.0.0.1:18083 （默认 admin / public），或看 1883 是否在监听

---

## 4. 编译

在工程根目录，已激活 IDF 的 PowerShell 里：

```powershell
cd D:\work\espidf_projects\inverter_collector
idf.py build
```

成功标志：

```text
Project build complete. To flash, run:
 idf.py flash
```

产物 `build/inverter_collector.bin` 约 **997 KB**。

**如果日志看起来卡在 `777/999` 不动** —— 那不是死锁，是 PowerShell 在把管道输出逐行转成 .NET 对象。用脚本绕开：

```powershell
python tools\build_run.py
```

日志会写到 `build_log.txt`，能实时看到真实进度。

---

## 5. 烧录

### 5.1 命令

```powershell
idf.py -p COM8 -b 115200 flash
```

**波特率必须是 115200。** 实测过的三组结果：

| 配置 | 结果 |
|---|---|
| `-b 460800` | `The chip stopped responding` |
| `-b 115200 --no-stub` | 写到 15% 报 `0105 message format invalid` |
| **`-b 115200`（默认带 stub）** | ✅ 成功，997,280 字节，约 56 秒 |

### 5.2 报 `No serial data received` 怎么办

说明芯片没进下载模式（因为线没有 DTR/RTS）。手动来：

```text
① 按住 BOOT 键不放
② 点一下 RST 键
③ 松开 BOOT
④ 立刻执行 idf.py -p COM8 -b 115200 flash
```

### 5.3 烧完必须手动按 RST

`Hard resetting via RTS pin...` 在这条线上是空操作，芯片可能还停在下载模式（日志会显示 `boot:0x4 (DOWNLOAD)`）。**按一下 RST**，让它从 flash 启动。

> 不要试图用 `esptool.py --after hard_reset` 或 `boot_run.py` 的软复位 —— ESP32-C3 不支持 esptool 软件复位，会报 `Soft resetting is currently only supported on ESP8266`。

---

## 6. 看日志

三种方式，按场景选：

| 方式 | 命令 | 特点 |
|---|---|---|
| **标准** | `idf.py -p COM8 monitor` | 退出按 `Ctrl + ]`；非交互终端里会立刻 EOF |
| **抓启动日志**（推荐） | `python tools\boot_run.py COM8 115200 60` | 抓 60 秒存 `tools/boot_*.log`，中文不乱码 |
| **一直盯着看** | `python tools\watch_serial.py` | 适合长时间观察 |

### 6.1 看到这四行才算启动成功

```text
I (812) MAIN: report channel: MQTT -> mqtt://192.168.137.1:1883
I (1724) MQTT_CMD: cmd ready: sub "dev/intern01/read" & "dev/intern01/write", pub "dev/intern01/resp"
I (2414) MQTT: client_id=intern01, pub topic=dev/intern01/data
I (2414) MQTT: subscribed "dev/intern01/read" (msg_id=1), "dev/intern01/write" (msg_id=2)
```

四行分别对应：上报通道选对了 → 命令模块起来了 → 连上 broker 了 → **订阅成功了**。

---

## 7. MQTTX 连接与订阅

### 7.1 连接配置

| 字段 | 填什么 |
|---|---|
| Host | `127.0.0.1`（MQTTX 和 EMQX 在同一台 PC 上） |
| Port | `1883` |
| Username | `esp32` |
| Password | 见 `menuconfig → MQTT Password` |
| **Client ID** | **不能用 `intern01`**！用 `mqttx_pc` 之类 |

> Client ID 重名会**互踢** —— 你用 `intern01` 连上，板子就被踢下线；板子重连，你又掉线。表现为两边疯狂断开重连。

### 7.2 订阅

```text
dev/intern01/#
```

`#` 从当前层级往下全匹配，一次订全四个主题。实际会有数据的只有 `data` 和 `resp`。

### 7.3 载荷格式：Hex，不是 JSON

阶段三的载荷是 **protobuf 二进制**，MQTTX 底部发布面板要把格式切到 `Hex`：

```text
Topic:   dev/intern01/read
Payload: 0a013110011880602005
```

```text
Topic:   dev/intern01/write
Payload: 0a0133100118806020ac1b     ← value=3500，即 350.0V
```

不想手算就用脚本：

```powershell
python tools\proto_decode.py read  --id 1 --addr 0x3000 --num 5
python tools\proto_decode.py write --id 2 --addr 0x3000 --value 3000
```

---

## 8. 四条验证路径

### 8.1 周期上报（等 5 秒就有）

不用做任何操作，`data` 每 5 秒来一帧：

```text
data: device="intern01" seq=7 ts_ms=1764... state=0
  metrics: addr=0x3000 raw=3500 value=350.0
  metrics: addr=0x3002 raw=500  value=5.00
  metrics: addr=0x3004 raw=1750 value=1750.0
```

### 8.2 读寄存器

发 `read` → 在 **`resp`** 收到应答（不是在 `data`）：

```text
resp: req_id="1" ok=true values=[3500, 500, 1750]
```

### 8.3 写寄存器

发 `write` 改 `0x3000` 为 `3000`：

```text
resp: req_id="2" ok=true                    ← 立即回执
下一帧 data: raw=3000 value=300.0            ← 5 秒内生效
```

### 8.4 掉线状态

把从机停掉（Ctrl+C 关掉模拟器），下一帧会变成：

```text
state=1  metrics: 沿用上一次的缓存值
```

恢复从机后回到 `state=0`。

---

## 9. 验收用例对照

### 阶段三（P-01 ~ P-07）

| 编号 | 验什么 | 怎么确认 |
|---|---|---|
| P-01 | 生成代码入库并能编译 | `main/telemetry.pb-c.c` 存在，`idf.py build` 零警告 |
| P-02 | `seq` 递增、时间间隔 | 相邻两帧 `seq` 差 1，`ts_ms` 差约 5000 |
| P-03 | 数值与从机一致 | 三行 metric 的 `raw` 等于从机寄存器值 |
| P-04 | 读写闭环 | 8.2 / 8.3 / 8.4 三条都通 |
| P-05 | 含 `0x00` 的帧不截断 | 写 `0` 后帧长 50 字节，`proto_decode.py` 解码完整并报「完整性 OK」 |
| P-06 | 坏报文被丢弃 | 发 `0a0531` → 日志 `bad ReadRequest (unpack_failed), len=3, dropped`，不回应答、不重启 |
| P-07 | 工具链版本可复现 | README 12 章记录了 protoc 6.30.2 / protoc-gen-c 1.5.2 / 运行时 1.4.1 |

### 阶段二（M-01 ~ M-08）

覆盖：四主题收发、命令队列、总线互斥、分片丢弃、坏载荷、`req_id` 校验、超时、QoS 1 重复投递。详见 `docs/STAGE2_TEST_REPORT.md`。

---

## 10. 故障对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| 烧录报 `port is busy` | 串口助手还占着 COM8 | 关掉串口助手 / monitor 再烧 |
| 烧录报 `No serial data received` | 没进下载模式 | 按住 BOOT → 点 RST → 松 BOOT |
| 烧录成功但程序不跑 | 停在下载模式（无 RTS） | 手动按一次 RST |
| 日志一直 `MQTT reconnect`，PC 上 EMQX 在跑 | 热点没开，`192.168.137.1` 不存在 | 打开移动热点 |
| 日志连不上 broker，热点正常 | EMQX 没启动 | `emqx.cmd console` |
| MQTTX 和板子互相踢下线 | Client ID 都是 `intern01` | MQTTX 改名 `mqttx_pc` |
| 发命令到 `data` 没反应 | 板子只订阅 `read`/`write` | 主题改成 `read` 或 `write` |
| MQTTX 显示 `{addr,raw,value}` 一串 | 用了 `Metric` 而不是 `Telemetry` 解码 | 解码类型选 `Telemetry` |
| 同一帧收到 3 次 | QoS 1 是「至少一次」 | 正常现象，靠 `seq` 去重 |
| 从机数值全 0 | 模拟器没起 / 连错 COM 口 | 确认 COM10、9600-8-N-1、Slave ID=1 |
| 烧录卡在 `Connecting....` | 芯片在跑 App，没进下载 | BOOT + RST 手动进下载模式 |

---

## 11. 收尾顺序

```text
① 关 monitor / 串口脚本（释放 COM8）
② Ctrl+C 停从机模拟器（释放 COM10）
③ emqx.cmd console 窗口按 Ctrl+C 两次（或直接关窗口）
④ 板子断电
```

不按顺序也行，但下次开机会遇到「端口被占用」，多一步排查。

---

## 12. 只改配置不烧录能生效吗

不能。`menuconfig` 改的值是**编译期常量**（`CONFIG_*` 宏），改完必须重新 `build` + `flash`。

```powershell
idf.py menuconfig      # 改参数
idf.py build           # 重新编译
idf.py -p COM8 -b 115200 flash
```

四个可改参数（`menuconfig → Inverter Collector`）：

| 参数 | 当前值 | 影响 |
|---|---|---|
| Wi-Fi SSID / Password | 你的热点 | 连不上就全断 |
| MQTT Broker URI | `mqtt://192.168.137.1:1883` | 换主机要改 |
| MQTT Dev ID | `intern01` | 同时决定四个主题和 Client ID |
| MQTT Username / Password | `esp32` / 见配置 | 认证失败会反复重连 |
