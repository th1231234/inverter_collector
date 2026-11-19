# 逆变器数据采集器（ESP32-C3 + Modbus RTU + Wi-Fi UDP/TCP）

基于 ESP-IDF 的嵌入式数据采集程序。通过 UART 以 **Modbus RTU** 协议读取逆变器保持寄存器，解析出电压/电流/功率，再通过 **Wi-Fi** 以 **UDP（默认）或 TCP** 上报 JSON 格式数据至指定服务器。上报协议由 `menuconfig` 切换，无需改代码。

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
| UDP 上报（默认） | JSON 格式，周期 5 秒；无连接，一包即一条消息 |
| TCP 上报（可选） | JSON + `\n` 分隔；带连接超时、断线重建，Wi-Fi 重连后自动重连服务器 |
| 上报协议切换 | `idf.py menuconfig` → Inverter Collector → 上报通道，编译期二选一 |
| 上报门控 | 连续成功采集 3 次后才启动上报，任何一次失败即重新计数 |
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
| 从机仿真 | Modbus Slave |
| 上报接收 | Socket 调试工具 / 附带 `tools/udp_receiver.py`、`tools/tcp_receiver.py` |
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

---

## 8. 参数配置（menuconfig 界面）

程序当前状态：**寄存器映射已切换为逆变器正式映射**；网络参数已从源码宏迁移到 Kconfig，源码中不再硬编码任何凭据。

执行 `idf.py menuconfig` → `Inverter Collector`，四个参数全部在图形界面里改，无需碰代码：

| 配置项 | 路径 | 自测值（电脑热点） | 验收值（公司网络） |
|---|---|---|---|
| `WIFI_SSID` | Wi-Fi 参数 | `LAPTOP` | 公司路由器（导师提供） |
| `WIFI_PASSWORD` | Wi-Fi 参数 | 见本地 `sdkconfig` | 公司密码（导师提供） |
| `REPORT_PROTO` | 上报通道 | `UDP`（默认） | UDP 或 TCP，按导师要求 |
| `REPORT_TARGET_IP` | 上报通道 | `192.168.137.1`（热点网关） | `192.168.1.200` |
| `REPORT_TARGET_PORT` | 上报通道 | `8080` | `8080` |

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
│   ├── wifi.c / wifi.h         Wi-Fi STA 连接 + 断线重连
│   ├── udp.c / udp.h           UDP socket + JSON 打包发送（默认）
│   └── tcp.c / tcp.h           TCP 连接管理 + JSON 发送（可选）
├── tools/
│   ├── udp_receiver.py         电脑端 UDP 接收测试脚本
│   └── tcp_receiver.py         电脑端 TCP 接收测试脚本（按行切分）
├── docs/
│   └── DEVELOPMENT_LOG.md      开发过程记录
└── README.md
```

---

## 10. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 烧录报 `port is busy` | 串口助手占用端口 | 关闭串口助手后重新烧录 |
| 日志无输出、显示 `waiting for download` | 芯片停在下载模式（GPIO9 被拉低） | 关闭串口助手，用 `idf.py flash` 让 esptool 自行复位 |
| 一直 `timeout` | A/B 接反（最常见） | 对调 A、B 两线 |
| `CRC mismatch` | 波特率不一致或未共地 | 确认两侧均为 9600-8-N-1，接好 GND |
| Wi-Fi 报 `reason=2` | 首次认证瞬时失败 | 自动重连会恢复，无需干预 |
| UDP 发不出去 | 电脑与设备不在同一网段 | 确认电脑与设备连接同一热点/路由器 |
| 功率恒为 0 | 从机 0x3004 未填值 | 在 Modbus Slave 中写入预设值 |

---

## 11. 已知说明

- 晶振提示 `Detected crystal freq ... Unsupported crystal in use` 为 ESP32-C3 模组的常见提示，不影响功能。
- Flash 实际容量 4 MB，工程默认配置为 2 MB，可通过 `idf.py menuconfig → Serial flasher config → Flash size` 调整为 4 MB 以消除启动警告。
