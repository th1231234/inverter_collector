# 开发过程记录

> 本文档按时间顺序记录逆变器数据采集器从环境准备到提交验收的完整过程，包含每次遇到的问题、定位思路与解决方式，可作为复盘材料或代码审查的补充说明。

---

## 0. 项目概览

| 项目 | 内容 |
|---|---|
| 目标 | ESP32-C3 通过 Modbus RTU 读取逆变器数据，经 Wi-Fi 以 UDP + JSON 上报 |
| 开发框架 | ESP-IDF v5.5.5 |
| 目标芯片 | ESP32-C3（`idf.py set-target esp32c3`）|
| 硬件 | PIEX-0032-12 开发板 + 板载 RS485 + USB 转 485 模块（CH340）|
| 成果 | 7 次功能提交，完整 README，实测通过全部功能项 |

---

## 1. 环境准备与基础验证

### 1.1 验证 ESP-IDF 环境

先确认工具链是否可用，避免后续问题混在一起难以定位。使用官方最小示例验证编译链路：

```powershell
cd examples\get-started\hello_world
idf.py set-target esp32
idf.py build
```

结果：出现 `Project build complete`，说明 CMake、交叉编译器、链接器全部正常。**这一步的价值是把"环境问题"和"代码问题"彻底隔离开。**

### 1.2 遇到的第一个问题：`CMakeLists.txt not found`

创建项目后直接执行 `set-target` 报错，原因是 `idf.py create-project` 会在指定目录下再建一层同名子目录，而命令仍在上一层执行。

```powershell
idf.py create-project inverter_collector   # 实际建在 espidf_projects\inverter_collector
idf.py set-target esp32                    # ❌ 此时仍在 espidf_projects
```

**结论**：所有 `idf.py` 命令必须在包含 `CMakeLists.txt` 的目录下执行。养成习惯——执行前先看提示符路径。

---

## 2. 串口驱动修复（PL2303）

### 2.1 现象

设备管理器中烧录线显示为：

```
PL2303HXA PHASED OUT SINCE 2012. PLEASE CONTACT YOUR SUPPLIER.
```

且**没有分配 COM 号**，无法烧录。

### 2.2 原因分析

这不是线坏，而是驱动不兼容。Prolific 在 2012 年后停产 PL2303HXA，并在新驱动中加入了仿冒检测，检测到老芯片就拒绝加载，只在设备管理器显示上述提示。

硬件 ID 确认为 `USB\VID_067B&PID_2303`，说明是真 Prolific 芯片，可以救。

### 2.3 解决过程

1. 下载开源工具 **PL2303 Legacy Driver Updater 1.1.0**（自带 2010 年驱动 `3.3.11.152`）
2. 安装后运行，界面显示 `Device not connected` —— 原因是未以管理员身份运行，且需先拔插一次设备
3. 以管理员身份重开 → 点 `Scan Drivers` → 选择 `Legacy PL2303 HXA/XA 3.3.11.152` → `Update Driver`

### 2.4 结果

设备管理器变为正常条目：

```
Prolific USB-to-Serial Comm Port (COM8)
```

**关键点**：PL2303 的 DTR/RTS 会驱动 ESP32 的 GPIO9（启动模式）与 EN（复位），这根线的 DTR/RTS 行为直接决定芯片能否正常启动，后续第 4 节的问题就源于此。

---

## 3. 工程创建

```powershell
mkdir D:\work\espidf_projects
cd D:\work\espidf_projects
idf.py create-project inverter_collector
cd inverter_collector
idf.py set-target esp32c3
idf.py build
```

### 3.1 芯片型号的确认过程

过程中曾出现两次判断偏差：

1. 早期烧录日志显示 `Chip is ESP32-D0WD-V3`，据此按 ESP32 配置引脚（TX=23/RX=39）
2. 后续串口日志出现 `ESP-ROM:esp32c3-api1-20210207`，且板卡 `sdkconfig` 提供了 `CONFIG_SUB_DEVICE_UART_*` 配置，才确认实际芯片为 **ESP32-C3**

**教训**：芯片型号必须以 esptool 的 `Chip is ...` 输出和 ROM 日志为准，不能靠板子丝印或历史印象推断。C3 只有 UART0/UART1，`UART_NUM_2` 在 C3 上根本不存在，写死会导致编译失败。

**处理方式**：用条件编译同时兼容两种芯片：

```c
#if CONFIG_IDF_TARGET_ESP32C3
  #define MODBUS_UART_PORT   UART_NUM_1
  #define MODBUS_TX_PIN      5
  #define MODBUS_RX_PIN      6
  #define MODBUS_DE_PIN      (-1)
#else
  #define MODBUS_UART_PORT   UART_NUM_2
  ...
#endif
```

---

## 4. 烧录链路排障（三个问题）

这一阶段耗时最长，问题层层叠加，逐个剥离。

### 4.1 问题一：`Invalid head of packet (0x00)`

```
A fatal error occurred: Failed to connect to ESP32:
Invalid head of packet (0x00): Possible serial noise or corruption.
```

**原因**：`idf.py flash` 未指定 `-p` 时自动猜串口，电脑上存在多个串口设备（CH340、CH343、PL2303、蓝牙虚拟口），猜错了目标。

**解决**：显式指定 `idf.py -p COM8 flash`，并养成习惯——每次插拔后先查实际 COM 号：

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' }
```

### 4.2 问题二：`Could not open COM5` / `No serial data received`

串口号在插拔后发生变化（COM5 → COM8）。**结论**：COM 号不固定，每次烧录前必须重新确认，不能沿用上一次的值。

### 4.3 问题三：`boot:0x4 (DOWNLOAD)` + `waiting for download`

烧录成功后板子不运行程序，串口只有：

```
rst:0xf (BROWN_OUT_RST),boot:0x4 (DOWNLOAD(USB/UART0/1))
waiting for download
```

两个独立原因：

| 现象 | 原因 | 处理 |
|---|---|---|
| `boot:0x4` | 串口助手的 DTR 保持有效，把 GPIO9 拉低，芯片进下载模式 | 关闭串口助手，改用 `idf.py flash` 让 esptool 自行控制 DTR/RTS 时序 |
| `BROWN_OUT_RST` | 供电不足导致反复掉电复位 | 改用 USB 直接供电、换短而粗的 USB 线、插主板后置口 |

**关键认知**：串口助手只适合"看日志"，不适合参与烧录。它不会按烧录时序操作 DTR/RTS，会与 esptool 抢占同一组控制线。

正常启动的标志：

```
boot:0x8 (SPI_FAST_FLASH_BOOT)
```

---

## 5. Modbus 模块开发

### 5.1 协议实现

| 项目 | 实现 |
|---|---|
| 功能码 | `0x03` 读保持寄存器 |
| 串口参数 | 9600-8-N-1 |
| CRC16 | 手动实现，多项式 `0x8005`（反转 `0xA001`），初值 `0xFFFF` |
| 字节序 | 大端序（高字节在前），CRC 低字节在前 |
| 超时重试 | 200 ms 超时，重试 2 次，间隔 20 ms |
| 异常处理 | 功能码最高位为 1 时解析异常码，报错返回但不崩溃 |

### 5.2 引脚确认

引脚最初按 C3 默认的 GPIO4/5/6 配置，后依据板卡配置修正：

```
CONFIG_SUB_DEVICE_UART_TX_PIN=5
CONFIG_SUB_DEVICE_UART_RX_PIN=6
```

因板卡未定义 DE 脚，判定板载 485 为自动收发方向，`MODBUS_DE_PIN` 设为 `-1`，走普通全双工模式。缓冲区大小同步对齐配置文件的 2304 字节。

### 5.3 CRC 验证方法

不依赖"跑起来看结果"，而是先用标准测试向量交叉验证算法：

```
01 03 00 00 00 01 → CRC 84 0A   ✅ 与公开资料一致
01 03 00 00 00 0A → CRC C5 CD   ✅ 与公开资料一致
```

实机请求帧 `01 03 30 00 00 05 8A C9` 也与离线独立计算结果一致。**这种双向验证能在硬件未通时就确认软件逻辑正确。**

### 5.4 偶发丢包的处理

初版实测 `ok=32, err=4`（成功率 89%），失败时连续 3 次全超时。定位到两个问题：

1. **日志与实际不符**：TX 打印在循环外，实际发了 3 帧却只记录 1 次
2. **无重试间隔**：上一帧总线未释放就发下一帧，自动方向切换的 485 电路容易丢帧头

**修正**：TX 打印移入循环内（每帧一条日志），并在重试之间加入 20 ms 间隔。修正后 `err=0`。

---

## 6. Wi-Fi 模块开发

### 6.1 实现要点

| 要求 | 实现方式 |
|---|---|
| 事件循环处理 GOT_IP | 注册 `IP_EVENT_STA_GOT_IP` 回调，收到才置 `s_connected = true` |
| 断线自动重连 | 在 `WIFI_EVENT_STA_DISCONNECTED` 回调中直接 `esp_wifi_connect()`，不限次数 |
| 不阻塞采集 | 上报前判断 `wifi_is_connected()`，未连接仅打印日志跳过 |

### 6.2 遇到的 `reason=2`

```
I wifi:state: init -> auth (0xb0)
I wifi:state: auth -> init (0x200)
W WIFI: disconnected (reason=2), reconnect attempt 1 ...
```

`reason=2` 为 AUTH_EXPIRE，认证阶段 AP 未响应。处理步骤：

1. 将认证方式从「仅 WPA2」放宽为 `WIFI_AUTH_WPA_WPA2_PSK`，并声明 PMF 能力（`pmf_cfg.capable = true`）
2. 交叉验证：改用手机热点测试，结果同样在首次认证失败后自动重连成功

**最终结论**：这是**首次认证的瞬时失败**，并非持续故障——自动重连在 1~3 秒内即恢复。后续日志也证明连接稳定（`rssi: -60`），且实测断线后能在 2.6 秒内自动恢复。

---

## 7. UDP 模块开发

```c
{"voltage":350.0,"current":5.00,"power":1750}
```

- 电压 `%.1f`、电流 `%.2f`、功率 `%.0f`，与题目示例格式一致
- 目标地址做成宏：自测用热点网关 `192.168.137.1`，验收改 `192.168.1.200`
- 配套编写 `tools/udp_receiver.py`，在电脑端监听 8080 验证上报

**注意点**：电脑上的 UDP 接收端必须与设备处于同一网段。使用手机热点时电脑也要连该热点，否则数据包无法到达。

---

## 8. 主逻辑与上报门控

| 项 | 实现 |
|---|---|
| 任务模型 | `xTaskCreate`，栈 4096，优先级 5 |
| 采集周期 | 5000 ms |
| 上报门控 | `consecutive_ok >= 3` 才置 `reporting`，任何一次失败即清零 |
| 初始化顺序 | Wi-Fi → UDP → Modbus → 创建任务 |

日志覆盖题目要求的四类关键信息：Wi-Fi 连接状态、原始寄存器值（Hex）、解析后物理值、UDP 发送状态。

---

## 9. 寄存器映射切换（验收配置）

自测阶段使用 Modbus Slave 的 `20000` 起始地址（连续 3 个寄存器），验收阶段切换为逆变器正式映射：

| 宏 | 自测 | 验收 |
|---|---|---|
| `REG_BASE` | `20000` | `0x3000` |
| `REG_COUNT` | `3` | `5` |
| `IDX_VOLTAGE` | `0` | `0`（0x3000）|
| `IDX_CURRENT` | `1` | `2`（0x3002）|
| `IDX_POWER` | `2` | `4`（0x3004）|

**关键点**：电压、电流、功率地址步长为 2，不是连续排列。必须一次读取 5 个寄存器（含 0x3001、0x3003 两个空位），再按偏移 0/2/4 取值。若按"读 3 个连续"实现，会导致电流、功率全部错位。

对应 Modbus Slave 配置需为：`Address mode = Hex`、`Address = 3000`、`Quantity = 10`，预设值 `3500 / 500 / 1750`。

---

## 10. 验证结果

### 10.1 正常采集

```
I MODBUS: TX[8]: 01 03 30 00 00 05 8A C9
I MODBUS: RX[15]: 01 03 0A 0D AC 00 00 01 F4 00 00 06 D6 2A 71
I MODBUS: resp: addr=1 func=0x03 bytes=10 crc=0x712A (ok)
I MODBUS:   reg[0x3000] = 0x0DAC (3500)
I MODBUS:   reg[0x3002] = 0x01F4 (500)
I MODBUS:   reg[0x3004] = 0x06D6 (1750)
I MAIN: Value: Voltage=350.0V, Current=5.00A, Power=1750W
I UDP: Send to ...: {"voltage":350.0,"current":5.00,"power":1750}
```

### 10.2 故障注入（从机 ID 改为 2）

```
I MODBUS: TX[8]: 01 03 30 00 00 05 8A C9
W MODBUS: timeout (attempt 1/3)
I MODBUS: TX[8]: 01 03 30 00 00 05 8A C9
W MODBUS: timeout (attempt 2/3)
I MODBUS: TX[8]: 01 03 30 00 00 05 8A C9
W MODBUS: timeout (attempt 3/3)
E MODBUS: read reg[0x3000] x5 failed after 3 attempts
E MAIN: read failed, retry next cycle (ok=0, err=1)
```

表现：报错明确、重试完整、任务不崩溃；改回 ID=1 后下个周期自动恢复。

### 10.3 断线重连

```
W WIFI: disconnected (reason=201), reconnect attempt 1 ...
W WIFI: disconnected (reason=201), reconnect attempt 2 ...
I WIFI: Wi-Fi connected, IP: 192.168.137.xxx
```

2.6 秒内自动恢复，采集任务全程未中断。

---

## 11. 版本管理与文档

### 11.1 提交拆分

按模块拆分提交，便于代码审查逐块追溯：

```
8ea5b1e feat(config): 切换为逆变器正式寄存器映射 0x3000/0x3002/0x3004
faadaf2 docs: 添加 README 说明与 UDP 接收测试脚本
923bd3a feat(main): 集成采集任务、上报门控与运行日志
74e24b3 feat(udp): 实现 UDP JSON 数据上报
6e16373 feat(wifi): 实现 Wi-Fi STA 连接与断线自动重连
aebc9d7 feat(modbus): 实现 Modbus RTU 采集与 CRC16 校验
69894df chore: 初始化 ESP-IDF 工程骨架与构建配置
```

### 11.2 仓库

```
https://github.com/th1231234/inverter_collector    （main 分支，Public）
```

`.gitignore` 已排除 `build/`、`*.elf`、`*.bin` 等产物，共 13 个文件纳入版本管理。

---

## 12. 追加改造：上报通道 UDP / TCP 双实现

### 12.1 起因：文档里的术语矛盾

测验文档「3.2 Wi-Fi 网络模块」中：

- 网络参数写的是「**TCP**服务器IP：192.168.1.200」「**TCP**端口：8080」
- 功能要求写的是「通过 **UDP** 发送 JSON 格式数据」「**UDP** 发送数据格式为 JSON」

同一节内协议名不一致。两种解读都可能，若只实现一种，验收时存在被判定「未按需求」的风险。

### 12.2 决策：默认 UDP，TCP 作为可切换通道

| 方案 | 符合文档 | 风险 |
|---|---|---|
| 只做 UDP | 功能要求（写了两次）✅ | 导师若按标题要 TCP 则无法演示 ⚠️ |
| 只做 TCP | 标题（写了一次）✅ | 按功能要求验收直接丢分 ❌ |
| **UDP 默认 + TCP 可切换** | 两者均可现场演示 ✅✅ | 工作量增加约 50 行 |

选择第三种：把不确定性用配置项消化掉，验收现场按导师要求切换，不改代码。

### 12.3 具体改动

| 文件 | 改动 |
|---|---|
| `main/Kconfig.projbuild` | 新增。定义 `WIFI_SSID`、`WIFI_PASSWORD`、`REPORT_PROTO`（UDP/TCP 二选一）、`REPORT_TARGET_IP`、`REPORT_TARGET_PORT` |
| `main/tcp.c` / `tcp.h` | 新增。TCP 客户端：非阻塞 connect + `select` 限时 3 秒、`SO_SNDTIMEO` 限制发送阻塞、发送失败自动关闭连接 |
| `main/udp.c` | 目标 IP/端口改为读取 `CONFIG_REPORT_TARGET_*` |
| `main/wifi.c` | SSID/密码改为读取 `CONFIG_WIFI_*`，源码不再硬编码凭据 |
| `main/inverter_collector.c` | `#if CONFIG_REPORT_PROTO_TCP` 条件编译切换发送函数；检测 Wi-Fi 由断到通时调用 `tcp_client_close()` 重建连接 |
| `main/CMakeLists.txt` | 加入 `tcp.c` |
| `tools/tcp_receiver.py` | 新增。`listen`/`accept` 接收，按 `\n` 切分消息 |
| `sdkconfig.defaults` | 新增。配置模板（占位符），保证克隆后可编译 |
| `.gitignore` | 加入 `sdkconfig`，真实凭据不入库 |

### 12.4 TCP 相对 UDP 增加的三个处理点

1. **连接超时必须可控**。直接阻塞 `connect()` 在目标不可达时会被 lwIP 重传 SYN 拖住十几秒，把整个采集任务卡死。改用「非阻塞 connect + `select` 等待 3 秒 + `getsockopt(SO_ERROR)` 判定结果」。
2. **Wi-Fi 掉线后连接变「半死」**。TCP 连接状态在 Wi-Fi 重连后不会自动恢复，`send` 可能返回成功但数据不出去。处理方式：采集任务每周期比对 Wi-Fi 状态，检测到「断 → 通」时主动 `close` socket，下次发送时重建。
3. **TCP 是字节流，没有消息边界**。UDP 一包即一条消息；TCP 连续发送可能粘包，两条 JSON 挤在一起导致解析失败。处理方式：每条末尾追加 `\n`，接收端按行切分。

### 12.5 验证

| 项 | 结果 |
|---|---|
| UDP 模式编译 | 通过，0 error 0 warning |
| TCP 模式编译 | 通过，0 error 0 warning；镜像 0xccaa0 字节，分区剩余 20% |
| `idf.py reconfigure` 后配置保留 | 确认保留手工修改值，menuconfig 工作流可靠 |
| 源码明文凭据 | 已清除，源码中不再出现任何 Wi-Fi 密码字符串（改为 Kconfig 配置项） |

---

## 13. 问题汇总

| # | 现象 | 根因 | 解决 |
|---|---|---|---|
| 1 | `CMakeLists.txt not found` | 命令在项目上级目录执行 | 先 `cd` 进工程目录 |
| 2 | `PL2303HXA PHASED OUT` | 新驱动拒绝老芯片 | 安装 3.3.11.152 旧驱动 |
| 3 | `Device not connected` | 未以管理员身份运行、未拔插设备 | 管理员运行 + 重新插拔 |
| 4 | `Invalid head of packet (0x00)` | 自动选择了错误的串口 | 显式 `-p COMx` |
| 5 | `Could not open COM5` | 插拔后 COM 号变化 | 每次烧录前重新查询 |
| 6 | `boot:0x4 (DOWNLOAD)` | 串口助手 DTR 拉低 GPIO9 | 关闭串口助手，交给 esptool |
| 7 | `BROWN_OUT_RST` | 供电不足 | USB 直供、短粗线、后置口 |
| 8 | 串口日志乱码 | 波特率不匹配（厂商固件为 921600）| 统一使用 115200 |
| 9 | 一直 `timeout` | A/B 接反（最常见）| 对调 A、B |
| 10 | 偶发踢包 | 无重试间隔、TX 日志位置错误 | 加 20 ms 间隔 + 日志入循环 |
| 11 | Wi-Fi `reason=2` | 首次认证瞬时失败 | 放宽认证方式；自动重连自恢复 |
| 12 | 电流/功率错位 | 按"读 3 个连续"实现 | 读 5 个，按偏移 0/2/4 取值 |

---

## 14. 关键参数汇总

| 类别 | 参数 | 值 |
|---|---|---|
| Modbus | 串口 | UART1，TX=GPIO5，RX=GPIO6 |
| Modbus | 参数 | 9600-8-N-1，从机 1，功能码 03 |
| Modbus | 超时/重试 | 200 ms / 2 次 / 间隔 20 ms |
| 寄存器 | 地址 | 0x3000（电压）、0x3002（电流）、0x3004（功率）|
| 换算 | 系数 | 电压 ÷10.0，电流 ÷100.0，功率 ×1 |
| 任务 | 周期/栈 | 5000 ms / 4096 |
| 上报 | 门控 | 连续成功 3 次后启动 |
| 网络 | 上报协议 | UDP（默认）/ TCP，menuconfig 切换 |
| 网络 | 自测 | SSID `LAPTOP`，目标 `192.168.137.1:8080` |
| 网络 | 验收 | 公司 Wi-Fi，目标 `192.168.1.200:8080` |

---

## 15. 复盘要点

1. **分层验证**：先证明环境没问题（hello_world），再证明驱动没问题（能烧录），最后才写业务代码。每一步只引入一个新变量。
2. **交叉验证**：CRC 用标准向量和独立计算双重确认，硬件未通就能确定软件正确。
3. **日志要诚实**：日志记录的必须等于实际发生的（TX 打印次数 = 实际发送帧数），否则会误导排查。
4. **协议细节优先于代码技巧**：寄存器步长为 2 这种细节，比代码写得漂亮更关键。
5. **失败路径要设计**：超时、CRC 错、从机异常、Wi-Fi 断开，每一种都要有明确行为，且不中断主任务。
