# 逆变器数据采集器（ESP32-C3 + Modbus RTU + Wi-Fi UDP/TCP/MQTT）

基于 ESP-IDF 的嵌入式数据采集程序。通过 UART 以 **Modbus RTU** 协议读取逆变器保持寄存器，解析出电压/电流/功率，再通过 **Wi-Fi** 以 **UDP / TCP / MQTT** 上报，上报协议由 `menuconfig` 切换，无需改代码。

三种上报通道均已实测跑通：UDP 直发（JSON）、TCP 长连接（JSON）、MQTT 发布。

MQTT 通道当前使用 **protobuf 二进制**载荷（阶段三），四个主题对应的四份 message 定义在 `proto/telemetry.proto`；阶段二的 JSON 版本保留在 `stage2` tag。

---

## 1. 功能特性

| 功能 | 实现说明 |
|---|---|
| Modbus RTU 采集 | 功能码 `03`，9600-8-N-1，从机地址 1 |
| CRC16 校验 | 手动实现，多项式 `0x8005`（反转形式 `0xA001`），初值 `0xFFFF`；发送与接收均校验 |
| 多寄存器连续读取 | 单次请求读取多个连续寄存器，按大端序解析 |
| 超时重试 | 单次超时 200 ms，失败重试 2 次，重试间隔 20 ms |
| 异常帧处理 | 识别从机异常应答（功能码最高位置 1），报错但不崩溃 |
| Wi-Fi 连接 | STA 模式，事件循环驱动，`IP_EVENT_STA_GOT_IP` 才置连接标志 |
| 断线自动重连 | 收到 `WIFI_EVENT_STA_DISCONNECTED` 立即重连，不限次数 |
| 启动时序控制 | `wifi_wait_connected(20s)` 先等 IP 再启动上报通道，避免无网络时的无效连接重试；超时也继续采集 |
| UDP 上报（默认） | JSON 格式，周期 5 秒；无连接，一包即一条消息 |
| TCP 上报（可选） | JSON + `\n` 分隔；带连接超时、断线重建，Wi-Fi 重连后自动重连服务器 |
| MQTT 上报（可选） | QoS1 发布至 `dev/<工号>/data`，载荷为 `Telemetry` protobuf 二进制；Client ID 即工号；用户名/密码走 Kconfig |
| MQTT 下行命令 | 订阅 `dev/<工号>/read`、`dev/<工号>/write`，载荷为 `ReadRequest`/`WriteRequest`，执行 Modbus 读写后回 `Resp`；`req_id` 配对 |
| Protobuf 编码 | `get_packed_size` → 定长分配 → `pack`，发布时传真实字节数（不经过 `strlen`） |
| Protobuf 解码 | 长度预检 → `unpack` 判空 → `req_id` 字符白名单 → 字段范围校验；`free_unpacked` 全路径覆盖 |
| 时间戳 | SNTP 对时（`ntp.aliyun.com`），`ts_ms` 为真实 Unix 毫秒；未对时前退化为开机毫秒 |
| 上报协议切换 | `idf.py menuconfig` → Inverter Collector → 上报通道，编译期三选一 |
| 上报门控 | 连续成功采集 3 次后才启动上报，任何一次失败即重新计数 |
| 开机功能码自检 | 7 项 / 4 阶段，覆盖 `03` `06` `10` `04` `02` `01` 与异常码，结果自动汇总 |
| 任务调度 | FreeRTOS 任务，栈 4096 |
| 参数外部化 | Wi-Fi SSID/密码、目标 IP/端口全部通过 Kconfig 配置，源码不含凭据 |

---

## 2. 硬件清单

| 器件 | 说明 |
|---|---|
| ESP32-C3 开发板 | 主控（PIEX-0032-12） |
| TTL 转 RS485 模块 | 板载或外接，用于 UART ↔ 485 差分信号转换 |
| USB 转 RS485 模块 | 电脑侧接收（CH340 / PL2303 等） |
| USB 转 TTL 调试线 | 仅用于烧录与查看日志（接 UART0） |
| 杜邦线 | 若干 |

---

## 3. 接线说明

### 3.1 Modbus 通信（UART1）

| ESP32-C3 | TTL 转 RS485 模块 | 说明 |
|---|---|---|
| `GPIO5` | `DI`（TXD） | 主机发送 |
| `GPIO6` | `RO`（RXD） | 从机应答 |
| `GND` | `GND` | **必须共地** |
| — | `A` / `B` | 接 485 总线 |

> 若模块为自动收发型（无 DE/RE 引脚），无需方向控制脚；当前板卡配置即为自动方向，`MODBUS_DE_PIN` 设为 `-1`。

### 3.2 485 总线（两个模块之间）

```
TTL转485 模块 A  ──────────  USB转485 模块 A
TTL转485 模块 B  ──────────  USB转485 模块 B
```

桌上短距离测试无需 120 Ω 终端电阻。

### 3.3 调试日志（UART0，与 Modbus 完全独立）

| ESP32-C3 | USB 转 TTL 线 |
|---|---|
| `GPIO21`（TX0） | `RXD` |
| `GPIO20`（RX0） | `TXD` |
| `GND` | `GND` |

> ⚠️ UART0（日志/烧录）与 UART1（Modbus）是两组互不干扰的引脚，接线时不要混淆。

---

## 4. 软件环境

| 项目 | 版本 |
|---|---|
| ESP-IDF | v5.5.5（兼容 v5.1.2） |
| 目标芯片 | `esp32c3` |
| 调试工具 | 任意串口助手（115200-8-N-1） |
| 从机仿真 | Modbus Slave（Witte Software） |
| 上报接收 | UDP/TCP：`tools/udp_receiver.py`、`tools/tcp_receiver.py`；MQTT：EMQX + MQTTX |
| MQTT Broker | EMQX 5.2.1（Windows，`D:\work\MQTT\emqx-5.2.1-windows-amd64`） |
| MQTT 客户端 | MQTTX（图形化，兼做订阅验证） |
| 配置方式 | `idf.py menuconfig`（Kconfig 参数化，`sdkconfig` 不入库） |

---

## 5. 编译与烧录

```powershell
# 1. 进入项目目录
cd D:\work\espidf_projects\inverter_collector

# 2. 设置目标芯片（首次或更换板卡时执行）
idf.py set-target esp32c3

# 3. 配置 Wi-Fi 与上报参数（首次必做）
idf.py menuconfig
#   → Inverter Collector → Wi-Fi 参数：SSID / 密码
#   → Inverter Collector → 上报通道：UDP 或 TCP、目标 IP、端口

# 4. 编译
idf.py build

# 5. 烧录（COM8 请替换为实际串口号）
idf.py -p COM8 flash

# 6. 查看日志
idf.py -p COM8 monitor        # 退出：Ctrl + ]
```

> ⚠️ 烧录前请关闭占用该串口的串口助手，否则会提示 `port is busy`。
>
> ⚠️ `sdkconfig` 含真实 Wi-Fi 凭据，已加入 `.gitignore` 不提交。仓库只提供 `sdkconfig.defaults` 模板（占位符），克隆后必须执行第 3 步填自己的参数。

---

## 6. 运行步骤

### 6.1 从机侧（电脑）

1. 打开 Modbus Slave，`Connection → Connect`
2. 连接类型 **Serial Port**，端口选 USB 转 485 对应的 COM 口
3. 参数 **9600 / 8 / None / 1 / RTU**
4. `Setup → Slave Definition`：Address mode 选 **Hex**，Address 填 `3000`，Quantity 填 `10`；Slave ID = `1`，Function = `03`
5. 双击寄存器格子写入预设值：`3000` → `3500`、`3002` → `500`、`3004` → `1750`

### 6.2 上报接收侧（电脑）

先确认 `menuconfig` 里选的是哪种协议，再启动对应的接收端：

**UDP（默认）**

```powershell
cd tools
python udp_receiver.py
```

首次运行如弹出防火墙提示，**必须允许访问**，否则收不到 UDP 包。

**TCP**

```powershell
cd tools
python tcp_receiver.py
```

> 两者都监听 `0.0.0.0:8080`，不要同时启动（会端口冲突）。

### 6.3 上电运行

烧录后按复位键，程序自动执行：连接 Wi-Fi → 初始化上报通道 → 初始化 Modbus → 启动采集任务。

日志中会打印当前生效的通道，用于确认配置是否生效：

```
I (812) MAIN: report channel: UDP -> 192.168.137.1:8080
I (327) TCP: TCP target 192.168.1.200:8080 (connect on first send)
```

---

### 6.4 MQTT 通道：本地 Broker 搭建与验证

外网公共 Broker（`broker.emqx.io`）已弃用，改为**本机自建 EMQX**，数据不出局域网。

> 完整的逐步操作、MQTTX 各字段含义与故障排查见 **[`docs/EMQX与MQTTX操作手册.md`](docs/EMQX与MQTTX操作手册.md)**（同目录有网页版 `.html`，双击即可阅读）。以下为速查版。
>
> 想弄清"MQTT 与 EMQX 是什么关系""1883 起什么作用""热点与防火墙各管哪一段"这类概念问题，见 **[`docs/MQTT概念澄清.md`](docs/MQTT概念澄清.md)**。
>
> 阶段二（查询 / 上报 / 修改）的完整实测数据与架构说明，见 **[`docs/STAGE2_TEST_REPORT.md`](docs/STAGE2_TEST_REPORT.md)**。

**启动 Broker**（EMQX 5.2.1，安装目录 `D:\work\MQTT\emqx-5.2.1-windows-amd64`）

```powershell
cd D:\work\MQTT\emqx-5.2.1-windows-amd64\bin
.\emqx.cmd start          # 后台启动；调试想看日志用 console
.\emqx.cmd ctl status     # 确认节点已启动
```

> 该机器缺少 Erlang `os_mon` 应用，`emqx_telemetry` 会刷错误日志但不影响报文转发；已在 `etc/emqx.conf` 关闭 `telemetry { enable = false }`。

**创建 MQTT 用户**（Dashboard 账号与 MQTT 账号是两套，不要混淆）

```powershell
# Dashboard 密码（只管 18083 网页登录）
.\emqx.cmd ctl admins passwd admin admin12345

# MQTT 客户端账号（板子连接用）
curl -X POST http://localhost:18083/api/v5/authentication/password_based%3Abuilt_in_database/users `
  -H "Authorization: Bearer <登录拿到的 token>" -H "Content-Type: application/json" `
  -d "{\"user_id\":\"esp32\",\"password\":\"123456\"}"
```

Dashboard：`http://localhost:18083`，认证配置页 `#/authentication`（**不是** `#/authorization`，后者是 ACL 授权）。

**板子侧配置**（`menuconfig → Inverter Collector → 上报通道`）

| 配置项 | 值 |
|---|---|
| `REPORT_PROTO` | `MQTT` |
| `MQTT_BROKER_URI` | `mqtt://192.168.137.1:1883`（热点网卡固定地址，不是 127.0.0.1） |
| `MQTT_DEV_ID` | `intern01`（工号；拼四个主题，同时作 Client ID） |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | `esp32` / `123456` |

**四个主题**（`dev/<工号>/用途`，指导书 2.3）

> **注意：本节表格里的 JSON 载荷是阶段二的报文格式**，对应代码在 `stage2` tag。
> 当前 `main` 分支的 MQTT 四个主题改用 protobuf 二进制，字段含义相同但载荷不是文本，
> 详见第 12 章；用 MQTTX 手工发命令需要切到十六进制模式，用 `tools/proto_decode.py` 更省事。

| 方向 | 主题 | 载荷 | 谁发 |
|---|---|---|---|
| 下行 | `dev/intern01/read` | `{"req_id":"1","slave":1,"addr":12288,"num":3}` | 上位机 → 设备 |
| 下行 | `dev/intern01/write` | `{"req_id":"2","slave":1,"addr":12288,"value":3000}` | 上位机 → 设备 |
| 上行 | `dev/intern01/resp` | `{"req_id":"1","result":"ok","values":[3500,500,1750]}` | 设备 → 上位机 |
| 上行 | `dev/intern01/data` | `{"voltage":350.0,"current":5.00,"power":1750,"state":0}` | 设备周期上报 |

- `addr` 用**十进制**：`0x3000 = 12288`；
- `read` 走功能码 `03`，`write` 走功能码 `06`；
- 每个请求必有**且只有一条** `resp`，靠 `req_id` 配对，原样回传；
- 失败也回：`{"req_id":"7","result":"error","msg":"modbus_timeout"}`，`msg` 取值
  `modbus_timeout` / `modbus_illegal_function` / `modbus_illegal_address` / `invalid_param` / `request_busy`；
- `state`：`0` 正常，`1` 故障（Modbus 读失败，此时数值为上一次缓存值）。

**订阅验证**：打开 MQTTX → 新建连接（`127.0.0.1:1883`，账号 `esp32`/`123456`）→ 订阅 `dev/intern01/#`，即可看到周期上报：

```json
{"voltage":350.0,"current":5.00,"power":1750,"state":0}
```

查询一次当前寄存器（发到 `dev/intern01/read`）：

```json
{"req_id":"1","slave":1,"addr":12288,"num":3}
```

在 `dev/intern01/resp` 收到：

```json
{"req_id":"1","result":"ok","values":[3500,500,1750]}
```

改电压为 300.0V（发到 `dev/intern01/write`，`3000 = 300.0V × 10`）：

```json
{"req_id":"2","slave":1,"addr":12288,"value":3000}
```

`resp` 立即回 `{"req_id":"2","result":"ok"}`，下一条 `data` 的 `voltage` 变为 `300.0`。

**纯命令行验证**（不依赖 GUI，见 `tools/mqtt_lab/`）

```powershell
cd tools\mqtt_lab
python lab_sub.py 127.0.0.1 1883 dev/intern01/# esp32 123456     # 订阅上行
python lab_pub.py 127.0.0.1 1883 dev/intern01/test 5 1 esp32 123456  # 发布
python mqtt_frame_demo.py 127.0.0.1 1883                     # 裸 socket 手拼报文，看原始字节
```

> `mqtt_frame_demo.py` 不使用任何 MQTT 库，直接拼 `CONNECT`/`SUBSCRIBE`/`PUBLISH` 字节，用于理解协议帧结构。

---

### 6.5 开机功能码自检（Modbus Slave 配合）

`menuconfig → Inverter Collector → 开启 Modbus 自检` 打开后，启动时先跑 7 项功能码自检，再进入正常采集。

| 阶段 | 项数 | 内容 | 从机侧需要的 Function |
|---|---|---|---|
| 1/4 | 4 | `03` 读保持、`06` 单写回读、`10` 多写回读、非法地址异常码 | `03 Holding Registers`（Address 3000，Qty 16） |
| 2/4 | 1 | `04` 读输入寄存器 | `04 Input Registers` |
| 3/4 | 1 | `02` 读离散输入 | `02 Input Status` |
| 4/4 | 1 | `01` 读线圈 | `01 Coils` |

设计要点：

- **每 2 秒自动探测**，Function 切好就立即开测，不必等满倒计时；
- 失败按异常码区分原因：`0x01` = Function 未切换，`0x02` = 地址越界；
- 每项最多 2 轮，连续两次不对即跳过，整体耗时约 45 秒（旧版固定 45 秒/阶段，共 144 秒）；
- **地址自适应**：`04/02/01` 先试 `0x0000`，若被拒自动重试 `0x3000`，因此无需改动 Modbus Slave 的 Address（改 Address 会清空整张数据表）；
- 自检结束后务必把 Function 切回 `03 Holding Registers`，否则主循环采集会持续报 `illegal function`。

---

## 7. 运行日志示例

```
I (547)  MODBUS: UART1 init ok, TX=5 RX=6, 9600-8-N-1
I (7657) WIFI: Wi-Fi connected, IP: 192.168.137.252
I (10837) MODBUS: TX[8]: 01 03 30 00 00 05 8A C9
I (10847) MODBUS: RX[15]: 01 03 0A 0D AC 00 00 01 F4 00 00 06 D6 2A 71
I (10847) MODBUS: resp: addr=1 func=0x03 bytes=10 crc=0x712A (ok)
I (10847) MODBUS:   reg[0x3000] = 0x0DAC (3500)
I (10847) MODBUS:   reg[0x3001] = 0x0000 (0)
I (10847) MODBUS:   reg[0x3002] = 0x01F4 (500)
I (10847) MODBUS:   reg[0x3003] = 0x0000 (0)
I (10847) MODBUS:   reg[0x3004] = 0x06D6 (1750)
I (10847) MAIN: Raw: [0x3000]=0x0DAC [0x3002]=0x01F4 [0x3004]=0x06D6
I (10847) MAIN: Value: Voltage=350.0V, Current=5.00A, Power=1750W
I (10847) MAIN: 3 consecutive reads OK, start UDP reporting
I (10847) UDP: Send to 192.168.137.1:8080 success: {"voltage":350.0,"current":5.00,"power":1750}
I (15947) MAIN: Stats: ok=4, err=0
```

MQTT 通道（实测日志，节选）：

```
I (1714) WIFI: Wi-Fi connected, IP: 192.168.137.219
I (1714) MAIN: network ready, IP=192.168.137.219
I (1724) MQTT_CMD: cmd ready: sub "dev/intern01/read" & "dev/intern01/write", pub "dev/intern01/resp"
I (1724) MQTT: mqtt client started, waiting for broker ...
I (2414) MQTT: broker connected: mqtt://192.168.137.1:1883
I (2414) MQTT: client_id=intern01, pub topic=dev/intern01/data
I (2414) MQTT: subscribed "dev/intern01/read" (msg_id=1), "dev/intern01/write" (msg_id=2)
I (113975) MODBUS:   reg[0x3000] = 0x0DAC (3500)
I (113995) MAIN: Value: Voltage=350.0V, Current=5.00A, Power=0W
I (114005) MQTT: data -> "dev/intern01/data": {"voltage":350.0,"current":5.00,"power":0,"state":0}
I (135225) MAIN: Value: Voltage=350.0V, Current=5.00A, Power=1750W
I (135235) MQTT: data -> "dev/intern01/data": {"voltage":350.0,"current":5.00,"power":1750,"state":0}
I (152310) MQTT_CMD: read ok: req_id=1 slave=1 addr=0x3000 num=3
I (152318) MQTT: resp -> "dev/intern01/resp": {"req_id":"1","result":"ok","values":[3500,500,1750]}
```

> 日志里的数值是本机实测片段，`req_id` 与 `values` 是阶段二新增的下行通道示例。
> **Client ID 现在等于工号**（`intern01`）：同一 Broker 上工号不能重复，两台设备用同一工号会互相踢下线。

阶段三（protobuf）实测日志，节选：

```
I (226678) MODBUS: func 0x03 ok: addr=12288 num=5 crc=0x8C16
I (226698) MAIN: Raw: [0x3000]=0x0000 [0x3002]=0x01F4 [0x3004]=0x06D6
I (226698) MAIN: Value: Voltage=0.0V, Current=5.00A, Power=1750W
I (226708) MQTT: data -> "dev/intern01/data": seq=16 state=0 3 metric(s), 50 bytes
I (227128) MQTT_CMD: write ok: req_id=3 slave=1 addr=0x3000 value=3500
I (227128) MQTT_CMD: resp ok: req_id=3
E (251278) MQTT_CMD: bad ReadRequest (unpack_failed), len=3, dropped
```

`data` 行里的 `50 bytes` 是这一帧 `pack` 出来的真实长度（`raw=0` 时比正常帧短 8 字节），不是 `strlen` 的结果。

---

## 8. 参数配置（menuconfig 界面）

程序当前状态：**寄存器映射已切换为逆变器正式映射**；网络参数已从源码宏迁移到 Kconfig，源码中不再硬编码任何凭据。

执行 `idf.py menuconfig` → `Inverter Collector`，四个参数全部在图形界面里改，无需碰代码：

| 配置项 | 路径 | 自测值（电脑热点） | 验收值（公司网络） |
|---|---|---|---|
| `WIFI_SSID` | Wi-Fi 参数 | `LAPTOP` | 公司路由器（导师提供） |
| `WIFI_PASSWORD` | Wi-Fi 参数 | 见本地 `sdkconfig` | 公司密码（导师提供） |
| `REPORT_PROTO` | 上报通道 | `UDP`（默认）/ `TCP` / `MQTT` | 按导师要求 |
| `REPORT_TARGET_IP` | 上报通道 | `192.168.137.1`（热点网关） | `192.168.1.200` |
| `REPORT_TARGET_PORT` | 上报通道 | `8080` | `8080` |
| `MQTT_BROKER_URI` | 上报通道 → MQTT | `mqtt://192.168.137.1:1883` | 按需 |
| `MQTT_DEV_ID` | 上报通道 → MQTT | `intern01` | 换成真实工号 |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | 上报通道 → MQTT | `esp32` / `123456` | 按需 |

> 改完保存退出即可，`idf.py build` 会自动读取新值。启动日志会打印实际生效的协议与目标地址，可直接核对。

寄存器映射已按测验要求配置（`main/inverter_collector.c`）：

| 宏 | 值 | 说明 |
|---|---|---|
| `REG_BASE` | `0x3000` | 起始地址 |
| `REG_COUNT` | `5` | 一次连续读 5 个（覆盖 0x3000~0x3004） |
| `IDX_VOLTAGE` | `0` | 电压位于 0x3000 |
| `IDX_CURRENT` | `2` | 电流位于 0x3002 |
| `IDX_POWER` | `4` | 功率位于 0x3004 |

> 说明：逆变器寄存器为 `0x3000`（电压）、`0x3002`（电流）、`0x3004`（功率），**步长为 2**，因此验收配置需一次读取 5 个寄存器再按偏移 `0/2/4` 取值。

### 关于文档中 TCP / UDP 术语不一致的说明

测验文档「3.2 Wi-Fi 网络模块」中，标题写的是「TCP服务器IP：192.168.1.200」「TCP端口：8080」，但功能要求写的是「通过 UDP 发送 JSON 格式数据」「UDP 发送数据格式为 JSON」，两者术语不一致。

本项目的处理方式：**默认走 UDP**，同时实现了完整的 TCP 通道，通过 `menuconfig` 一行切换即可。理由：

1. 功能要求两次明确指定 UDP，且 UDP 无连接、语义上更贴合「周期上报」；
2. 文档标题的 TCP 可能是模板残留，但为稳妥起见两者都实现，验收时按导师要求现场切换；
3. 若改为 TCP，接收端需按 `\n` 切分消息（TCP 是字节流，无消息边界），`tools/tcp_receiver.py` 已按此实现。

### 数据转换公式

| 参数 | 寄存器 | 单位 | 转换 |
|---|---|---|---|
| 电网电压 | `0x3000` | 0.1 V | 实际值 = 寄存器值 / 10.0 |
| 电网电流 | `0x3002` | 0.01 A | 实际值 = 寄存器值 / 100.0 |
| 瞬时功率 | `0x3004` | 1 W | 实际值 = 寄存器值 |

---

## 9. 目录结构

```
inverter_collector/
├── CMakeLists.txt              顶层构建文件
├── sdkconfig.defaults          配置模板（入库，含占位符）
├── sdkconfig                   本地实际配置（含凭据，不入库）
├── main/
│   ├── CMakeLists.txt          组件构建（列出所有源文件）
│   ├── Kconfig.projbuild       menuconfig 配置项定义
│   ├── inverter_collector.c    app_main + 采集任务 + 上报调度
│   ├── modbus.c / modbus.h     Modbus RTU 协议 + CRC16 校验
│   ├── modbus_selftest.c/.h    开机 7 项功能码自检（可选）
│   ├── wifi.c / wifi.h         Wi-Fi STA 连接 + 断线重连 + 等待 IP
│   ├── udp.c / udp.h           UDP socket + JSON 打包发送（默认）
│   ├── tcp.c / tcp.h           TCP 连接管理 + JSON 发送（可选）
│   ├── mqtt_report.c/.h        MQTT 客户端 + 四主题 + QoS1 发布（可选）
│   ├── mqtt_cmd.c/.h           下行命令：订阅 read/write → 队列 → Modbus → resp
│   ├── telemetry_codec.c/.h    protobuf 编解码封装（上层不接触 pb-c.h）
│   └── telemetry.pb-c.c/.h     由 proto 生成，入库（见第 12 章）
├── proto/
│   └── telemetry.proto         四份 message 定义（唯一数据源）
├── tools/
│   ├── udp_receiver.py         电脑端 UDP 接收测试脚本
│   ├── tcp_receiver.py         电脑端 TCP 接收测试脚本（按行切分）
│   ├── serial_read.py          只读串口抓日志（不触发 cmake 重配置）
│   ├── build_run.py            编译并把输出直接写文件（避开 PowerShell 管道卡顿）
│   ├── boot_run.py             软复位 + 抓启动日志
│   ├── proto_decode.py         protobuf 收发与解析（阶段三验证用）
│   ├── telemetry_pb2.py        由 proto 生成的 Python 绑定
│   ├── modbus_slave_sim.py     Modbus RTU 从机模拟器（03/06，脚本化控制寄存器）
│   └── mqtt_lab/               本地 MQTT 实验台
│       ├── broker.yaml         amqtt Broker 配置（0.0.0.0:1883 匿名）
│       ├── lab_sub.py          订阅端
│       ├── lab_pub.py          发布端
│       ├── mqtt_frame_demo.py  裸 socket 手拼 MQTT 报文（教学用）
│       ├── cmd_probe.py        发一条 read/write 并等 resp（阶段二验证用）
│       ├── bad_payload.py      坏 JSON + 1KB 超长载荷（边界测试）
│       └── README.md           实验台使用说明
├── docs/
│   ├── DEVELOPMENT_LOG.md      开发过程记录
│   ├── TEST_REPORT.md          测试报告（含实测数据与证据）
│   ├── TEST_REPORT.html        测试报告网页版（可导航）
│   ├── EMQX与MQTTX操作手册.md   Broker 启动 + 客户端订阅逐步指南
│   ├── EMQX与MQTTX操作手册.html 手册网页版
│   ├── MQTT概念澄清.md          协议/软件/端口/通路的概念对照与排查
│   ├── MQTT概念澄清.html        概念澄清网页版
│   ├── STAGE2_TEST_REPORT.md    阶段二（MQTT 收发）测试报告
│   └── STAGE2_TEST_REPORT.html  阶段二报告网页版
└── README.md
```

---

## 10. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 烧录报 `port is busy` | 串口助手占用端口 | 关闭串口助手后重新烧录 |
| 烧录报 `No serial data received` | 芯片在跑 App，没进下载模式（四线转串口无 RTS，`--after hard_reset` 空操作） | 按住 BOOT → 点 RST → 松 BOOT，再执行烧录 |
| esptool 依次尝试多个 COM 口 | 未指定 `-p` | 加 `-p COM8`（或实际端口号） |
| 日志无输出、显示 `waiting for download` | 芯片停在下载模式（GPIO9 被拉低） | 手动点 RST 或重新上电 |
| 一直 `timeout` | A/B 接反（最常见） | 对调 A、B 两线 |
| `CRC mismatch` | 波特率不一致或未共地 | 确认两侧均为 9600-8-N-1，接好 GND |
| 主循环持续 `illegal function` | 自检后没把 Function 切回 `03` | 切回 `03 Holding Registers`，无需重新烧录 |
| 自检报 `0x02 illegal data address` | 从机表范围不含请求地址 | 本版已自适应 `0x0000`/`0x3000`；仍失败则把 Quantity 改到 16 以上 |
| 改了 Modbus Slave 的 Function 后数据全变 0 | 四个数据区（0x/1x/3x/4x）彼此独立，切 Function 等于换表且会重新初始化 | 属正常现象；填值放在自检全部结束之后，填完不要再按 OK |
| Wi-Fi 报 `reason=201` | 热点尚未就绪（网卡热启动） | 自动重连会恢复；本版最多等 20 秒再启动上报通道 |
| Wi-Fi 报 `reason=2` | 首次认证瞬时失败 | 自动重连会恢复，无需干预 |
| UDP 发不出去 | 电脑与设备不在同一网段 | 确认电脑与设备连接同一热点/路由器 |
| MQTT 连不上、反复 `MQTT_EVENT_DISCONNECTED` | 防火墙未放行 1883 入站（板子主动连电脑，属入站连接） | 管理员执行 `netsh advfirewall firewall add rule name="MQTT 1883" dir=in action=allow protocol=TCP localport=1883` |
| MQTT 连上后立刻被踢 | 用户名/密码错误，或 client_id 与其他设备冲突 | 用 `esp32`/`123456`；client_id 已由 MAC 生成，冲突时改源码后缀 |
| Dashboard 提示密码错误 | Dashboard 账号与 MQTT 账号是两套独立表 | `emqx.cmd ctl admins passwd admin <新密码>` 重置网页密码 |
| 功率恒为 0 | 从机 0x3004 未填值 | 在 Modbus Slave 中写入预设值 |

---

## 11. 已知说明

- 晶振提示 `Detected crystal freq ... Unsupported crystal in use` 为 ESP32-C3 模组的常见提示，不影响功能。
- Flash 实际容量 4 MB，工程默认配置为 2 MB，可通过 `idf.py menuconfig → Serial flasher config → Flash size` 调整为 4 MB 以消除启动警告。

---

## 12. Protobuf 编解码（阶段三）

### 12.1 使用的工具链版本

| 组件 | 版本 | 来源 |
|---|---|---|
| `protoc`（编译器前端） | 6.30.2（`libprotoc 30.2`） | pip 包 `protoc-wheel-0`，装到 IDF 的 Python 环境 |
| `protoc-gen-c`（C 插件） | 1.5.2 | MSYS2 官方仓库包 `mingw-w64-x86_64-protobuf-c-1.5.2`，从 `pkg.tar.zst` 里解出 |
| 固件运行时 `protobuf-c` | 1.4.1 | ESP-IDF 5.5.5 自带组件 `protobuf-c` |

版本兼容性说明：生成的代码里有

```c
#if PROTOBUF_C_VERSION_NUMBER < 1003000
#error This file was generated by a newer version of protoc-c ...
#endif
```

IDF 自带的运行时是 1.4.1（`1004001`），大于 `1003000`，**不需要改任何一行生成代码**，也不需要升级 IDF 组件。

### 12.2 生成命令

在工程根目录执行（`protoc-gen-c` 需在 `PATH` 里，或用 `--plugin` 指定绝对路径）：

```bash
# 1) 生成固件端 C 代码 → main/telemetry.pb-c.c / main/telemetry.pb-c.h
protoc --plugin=protoc-gen-c=/path/to/protoc-gen-c.exe \
       --proto_path=proto --c_out=main proto/telemetry.proto

# 2) 生成 PC 端 Python 代码 → tools/telemetry_pb2.py
protoc --proto_path=proto --python_out=tools proto/telemetry.proto

# 3) 重新编译（CMakeLists.txt 里已列出 telemetry.pb-c.c）
idf.py build
```

Windows 下的实际路径（本机）：

```text
protoc        : D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\protoc.exe
protoc-gen-c  : D:\work\tools\protoc-c\extracted\mingw64\bin\protoc-gen-c.exe
```

**生成文件入库**：`main/telemetry.pb-c.c`、`main/telemetry.pb-c.h`、`tools/telemetry_pb2.py` 都提交到仓库。这样别人拉下代码只需 `idf.py build` 即可，不会因为本机没装 `protoc-c` 而编译失败。只有改了 `proto/telemetry.proto` 才需要重新执行上面的生成命令。

### 12.3 报文结构

| 主题 | 方向 | message | 主要字段 |
|---|---|---|---|
| `dev/<工号>/read` | 下行 | `ReadRequest` | `req_id`, `slave`, `addr`, `num` |
| `dev/<工号>/write` | 下行 | `WriteRequest` | `req_id`, `slave`, `addr`, `value` |
| `dev/<工号>/resp` | 上行 | `Resp` | `req_id`, `ok`, `msg`, `values` |
| `dev/<工号>/data` | 上行 | `Telemetry` | `device_id`, `seq`, `ts_ms`, `state`, `metrics`（`addr`/`raw`/`value`） |

字段号规划原则：`req_id` 永远占字段 1，四份 message 保持一致；其余按使用频率排在 2~5，全部落在 1~15 区间内，每个字段号只占 1 字节 tag。发布后字段号不可改、不可复用。

### 12.4 代码分层

```
mqtt_report.c  ──┐
mqtt_cmd.c     ──┼──► telemetry_codec.c ──► telemetry.pb-c.c（生成）──► protobuf-c（IDF 组件）
inverter_coll. ──┘         （封装层）
```

上层只调用 `codec_encode_telemetry` / `codec_decode_read` / `codec_encode_resp_*`，**不直接包含 `telemetry.pb-c.h`**。这样三类易错点被关在一层里：

1. 忘记 `free_unpacked` —— 解码函数内部用 `goto done` 保证任何返回路径都释放；
2. 用 `strlen` 量二进制长度 —— 长度一律用 `pack` 返回的真实字节数；
3. 先 `unpack` 后判长度 —— `precheck()` 在 `unpack` 之前拦掉空包和超长包。

### 12.5 实测结果（本机，从机模拟器 0x3000=3500 / 0x3002=500 / 0x3004=1750）

周期上报（P-02 / P-03）：

```text
[data] device=intern01 seq=6 state=0  (58 字节)
       ts_ms=1795500198599 -> 2026-11-24 14:03:18
       addr=0x3000 raw=3500 value=350.00
       addr=0x3002 raw=500 value=5.00
       addr=0x3004 raw=1750 value=1750.00
       完整性 OK（重新序列化 58 字节，逐字节一致）
```

双向读写（P-04）：

```text
PUB  dev/intern01/read  (10 字节)  hex=0a013110011880602005
[resp] req_id=1 ok=true values=[3500(0x0DAC), 0(0x0000), 500(0x01F4), 0(0x0000), 1750(0x06D6)]

PUB  dev/intern01/write (11 字节)  hex=0a0133100118806020ac1b     # value=3500
[resp] req_id=3 ok=true  (5 字节)  hex=0a01321001
```

写 `0x3000=3000` 后下一帧 `raw=3000 value=300.00`，与指导书样例一致；再写回 3500 恢复。

读失败（拔掉从机 / 停掉模拟器）：

```text
[data] device=intern01 seq=27 state=1  (60 字节)
       addr=0x3000 raw=3500 value=350.00      # 沿用上次成功值
```

恢复从机后 `state` 自动回到 0（`seq=32`）。

含 `0x00` 的报文（P-05，`raw=0`）：

```text
[data] device=intern01 seq=14 state=0  (50 字节)
       addr=0x3000 raw=0 value=0.00
       hex=0a08696e7465726e3031100e1881df9de1a0342a030880602a0b08826010f4031d0000a040...
       完整性 OK（重新序列化 50 字节，逐字节一致）
```

报文里有多个 `0x00`（`raw=0` 在 proto3 里直接不编码；`5.0` 的浮点尾数是 `1d 00 00 a0 40`）。发布时传的是 `pack` 返回的真实长度，`strlen` 会在第一个 `0x00` 处截断成半条报文。

边界安全（P-06）：

```text
# 发 0a0531（字段 1 声明 5 字节，实际只有 1 字节）
E MQTT_CMD: bad ReadRequest (unpack_failed), len=3, dropped
```

不回应答（取不到 `req_id` 就无法配对），不重启，下一帧 `data` 照常上报。

补充一条实测结论：指导书里用来演示 P-06 的 `0a0131` 在 **proto3 下是一条合法报文**——它等价于 `req_id="1"`、其余字段取默认值 `0`，所以 `unpack` 会成功，随后被「`slave` 必须在 1~247」的范围校验拦下，回 `resp{req_id="1", ok=false, msg="invalid_param"}`。要真正触发 `unpack` 返回 `NULL`，必须让长度字段与实际字节数不符（如 `0a0531`）。这两种输入的处理路径都已覆盖。

### 12.6 PC 端解析脚本

```bash
# 边收边解码（收到 3 条后退出）
python tools/proto_decode.py --count 3

# 加 --hex 打印原始字节
python tools/proto_decode.py --count 1 --hex

# 用字段拼请求（不用手算十六进制）
python tools/proto_decode.py read  --id 1 --addr 0x3000 --num 5
python tools/proto_decode.py write --id 2 --addr 0x3000 --value 3000

# 发一段原始十六进制（边界测试）
python tools/proto_decode.py send 0a0531
```

脚本会把解析出的对象**重新序列化一遍并与收到的字节逐字节比对**，输出 `完整性 OK` 即证明载荷没有被截断、没有多余填充。

### 12.7 从机模拟器

图形版 Modbus Slave 无法脚本化控制寄存器，因此附带一个 RTU 从机模拟器：

```bash
python tools/modbus_slave_sim.py COM10 9600 1
python tools/modbus_slave_sim.py COM10 9600 1 0x3000=3000 0x3004=0
```

支持功能码 `03`（读保持寄存器）和 `06`（写单个寄存器），其它功能码回异常码 `01`，地址越界回 `02`。板子发来的写命令会直接改模拟器里的值，下一帧 `data` 立即反映。
