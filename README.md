# 逆变器数据采集器（ESP32-C3 + Modbus RTU + Wi-Fi UDP）

基于 ESP-IDF 的嵌入式数据采集程序。通过 UART 以 **Modbus RTU** 协议读取逆变器保持寄存器，解析出电压/电流/功率，再通过 **Wi-Fi** 以 **UDP + JSON** 格式周期上报至指定服务器。

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
| UDP 上报 | JSON 格式，周期 5 秒 |
| 上报门控 | 连续成功采集 3 次后才启动上报，任何一次失败即重新计数 |
| 任务调度 | FreeRTOS 任务，栈 4096 |

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
| 上报接收 | Socket 调试工具 / 附带 `tools/udp_receiver.py` |

---

## 5. 编译与烧录

```powershell
# 1. 进入项目目录
cd D:\work\espidf_projects\inverter_collector

# 2. 设置目标芯片（首次或更换板卡时执行）
idf.py set-target esp32c3

# 3. 编译
idf.py build

# 4. 烧录（COM8 请替换为实际串口号）
idf.py -p COM8 flash

# 5. 查看日志
idf.py -p COM8 monitor        # 退出：Ctrl + ]
```

> ⚠️ 烧录前请关闭占用该串口的串口助手，否则会提示 `port is busy`。

---

## 6. 运行步骤

### 6.1 从机侧（电脑）

1. 打开 Modbus Slave，`Connection → Connect`
2. 连接类型 **Serial Port**，端口选 USB 转 485 对应的 COM 口
3. 参数 **9600 / 8 / None / 1 / RTU**
4. `Setup → Slave Definition`：Address mode 选 **Hex**，Address 填 `3000`，Quantity 填 `10`；Slave ID = `1`，Function = `03`
5. 双击寄存器格子写入预设值：`3000` → `3500`、`3002` → `500`、`3004` → `1750`

### 6.2 上报接收侧（电脑）

```powershell
cd tools
python udp_receiver.py
```

首次运行如弹出防火墙提示，**必须允许访问**，否则收不到 UDP 包。

### 6.3 上电运行

烧录后按复位键，程序自动执行：连接 Wi-Fi → 初始化 UDP → 初始化 Modbus → 启动采集任务。

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

## 8. 参数配置（自测 ↔ 验收切换清单）

程序当前状态：**寄存器映射已切换为逆变器正式映射**，Wi-Fi 与 UDP 目标仍是本地自测配置。验收前只需修改以下两处：

| 序号 | 文件 | 宏 | 当前（自测） | 验收值 |
|---|---|---|---|---|
| 1 | `main/wifi.c` | `WIFI_SSID` / `WIFI_PASSWORD` | `LAPTOP` / `12345678` | 公司路由器（由导师提供） |
| 2 | `main/udp.c` | `UDP_TARGET_IP` | `192.168.137.1` | `192.168.1.200` |

寄存器映射已按测验要求配置（`main/inverter_collector.c`）：

| 宏 | 值 | 说明 |
|---|---|---|
| `REG_BASE` | `0x3000` | 起始地址 |
| `REG_COUNT` | `5` | 一次连续读 5 个（覆盖 0x3000~0x3004） |
| `IDX_VOLTAGE` | `0` | 电压位于 0x3000 |
| `IDX_CURRENT` | `2` | 电流位于 0x3002 |
| `IDX_POWER` | `4` | 功率位于 0x3004 |

> 说明：逆变器寄存器为 `0x3000`（电压）、`0x3002`（电流）、`0x3004`（功率），**步长为 2**，因此验收配置需一次读取 5 个寄存器再按偏移 `0/2/4` 取值。

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
├── sdkconfig                   芯片与工程配置
├── main/
│   ├── CMakeLists.txt          组件构建（列出所有源文件）
│   ├── inverter_collector.c    app_main + 采集任务 + 上报逻辑
│   ├── modbus.c / modbus.h     Modbus RTU 协议 + CRC16 校验
│   ├── wifi.c / wifi.h         Wi-Fi STA 连接 + 断线重连
│   └── udp.c / udp.h           UDP socket + JSON 打包发送
├── tools/
│   └── udp_receiver.py         电脑端 UDP 接收测试脚本
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
