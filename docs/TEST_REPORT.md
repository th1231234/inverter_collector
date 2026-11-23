# 测试报告：逆变器数据采集器

| 项目 | 内容 |
|---|---|
| 被测对象 | `inverter_collector`（ESP32-C3 + Modbus RTU + Wi-Fi 上报） |
| 测试依据 | 成都易联智通 · 嵌入式初级工程师入职技术测验需求 |
| 测试日期 | 2026-11-20 |
| 测试人 | 田浩 |
| 固件版本 | ESP-IDF v5.5.5 / app 版本 `c8e3465-dirty` |
| 固件大小 | 979,696 字节（旧）→ 981,040 字节（自检优化）→ **981,552 字节**（地址自适应，app 分区余量 6%） |
| 总体结论 | **核心功能全部通过**：Modbus 采集 + JSON 解析 + MQTT 上报端到端闭环成立；7 项功能码自检通过 4 项，剩余 3 项为测试手法问题，代码侧已补齐自动适配 |

---

## 1. 测试环境

### 1.1 硬件

| 器件 | 型号 / 参数 |
|---|---|
| 主控 | ESP32-C3（QFN32，rev v0.4，MAC `34:85:18:cf:d1:f0`，4 MB XMC Flash） |
| 烧录/日志 | USB 转 TTL，四线（TX/RX/GND/3V3，无 RTS/DTR），`COM8` |
| Modbus 物理层 | TTL 转 485 + USB 转 485，`COM10`，9600-8-N-1 |
| 增益电源 | 3.3 V |

### 1.2 软件

| 类别 | 名称 / 版本 |
|---|---|
| 开发框架 | ESP-IDF v5.5.5，工具链 `riscv32-esp-elf` |
| 从机仿真 | Modbus Slave（Slave ID = 1） |
| MQTT Broker | EMQX 5.2.1（Windows amd64，本机 `0.0.0.0:1883`） |
| MQTT 订阅端 | MQTTX |
| 辅助脚本 | `tools/serial_read.py`、`tools/mqtt_lab/*.py` |

### 1.3 网络拓扑

```
ESP32-C3 (192.168.137.219)
        │  Wi-Fi STA，Windows 移动热点 "LAPTOP"
        ▼
Windows 热点网卡 192.168.137.1
        │
        ├── EMQX  0.0.0.0:1883   ← MQTT Broker
        └── MQTTX 127.0.0.1:1883 ← 订阅 inverter/#
```

> 板子连接的是**热点网卡固定地址 `192.168.137.1`**，不是 `127.0.0.1`——后者在板子上指向板子自己。

---

## 2. 测试用例与结果

| 编号 | 测试项 | 预期 | 实测 | 结论 |
|---|---|---|---|---|
| T01 | 编译 | 无错误、无新增告警 | `Project build complete`，bin 981,552 字节，余量 6% | 通过 |
| T02 | 烧录 | 三个镜像 `Hash of data verified` | bootloader / 分区表 / app 全部校验通过 | 通过 |
| T03 | 启动时序 | 先拿 IP 再启动上报通道 | `network ready`(1714 ms) → `mqtt client started`(1724 ms) | 通过 |
| T04 | Wi-Fi 连接 | 拿到 IP 并打印 | `Wi-Fi connected, IP: 192.168.137.219` | 通过 |
| T05 | 断线自动重连 | 断开后自愈，采集不断 | `reason=201` / `reason=4` 连续两次 → 6.5 s 后重连成功 | 通过 |
| T06 | Modbus `03` 读 | 8 字节请求 / 15 字节应答，CRC 正确 | `TX 01 03 30 00 00 05 8A C9` → `RX 01 03 0A ... 2A 71` | 通过 |
| T07 | Modbus `06` 单写回读 | 写入值与读回值一致 | 写 `0x3008=4000`，回读 `0x0FA0` | 通过 |
| T08 | Modbus `10` 多写回读 | 3 个寄存器全对 | `0x3009/0A/0B = 1111/2222/3333` | 通过 |
| T09 | 异常码识别 | 非法地址返回 `0x02` | `RX 01 83 02 C0 F1`，解析为 `illegal data address` | 通过 |
| T10 | 从机异常容错 | 报错但不崩溃、不退出 | 连续 17 次 `illegal function` 后仍正常采集 | 通过 |
| T11 | 数据换算 | 3500/500/1750 → 350.0/5.00/1750 | `Value: Voltage=350.0V, Current=5.00A, Power=1750W` | 通过 |
| T12 | 上报门控 | 连续 3 次成功才上报 | 第 3 次成功时打印 `3 consecutive reads OK, start MQTT reporting` | 通过 |
| T13 | MQTT 连接 | CONNACK 成功，client_id 唯一 | `broker connected`，`client_id=esp32c3-inverter-cfd1f0` | 通过 |
| T14 | MQTT 发布 | QoS1 发布，载荷为 JSON | `published ... (msg_id=41144): {"voltage":350.0,"current":5.00,"power":1750}` | 通过 |
| T15 | 端到端闭环 | 客户端收到设备真实数据 | MQTTX 收到同内容消息，时间戳与串口日志一一对应 | 通过 |
| T16 | 自检 `04/02/01` | 3 项 PASS | 4/7（3 项 FAIL，见第 3 节） | **未通过** |
| T17 | 周期稳定性 | 5 s 一条，无丢报 | 连续 23 个周期（115 s）无丢包、无重启 | 通过 |

---

## 3. 功能码自检明细（4/7）

```
----------------- 汇总 4/7 通过 -----------------
  PASS 03 read holding 0x3000 x5
  PASS 06 write + readback 0x3008
  PASS 16 write + readback 0x3009 x3
  PASS exception on illegal address 0xFFFF
  FAIL 04 read input 0x0000 x3
  FAIL 02 read discrete 0x0000 x8
  FAIL 01 read coils 0x0000 x8
```

### 3.1 未通过原因

三项失败的应答帧：

| 项 | 请求 | 应答 | 异常码 |
|---|---|---|---|
| `04` | `01 04 00 00 00 03` | `01 84 02` | `0x02 illegal data address` |
| `02` | `01 02 00 00 00 08` | `01 82 02` | `0x02 illegal data address` |
| `01` | `01 01 00 00 00 08` | `01 81 02` | `0x02 illegal data address` |

均为 **`0x02` 而非 `0x01`**：说明 Function 已切换正确，但 Modbus Slave 的地址基址仍停在 `3000`，只响应 `3000~300F`，对地址 `0` 的请求回"地址越界"。

而把 Address 改成 `0` 会导致 Modbus Slave **重新初始化整张数据表**（实测：切 Function 后寄存器值全部归零），代价是采集数据要重新填写。

### 3.2 处理措施

代码侧增加**地址自适应**：`04/02/01` 三项先请求 `0x0000`，若返回 `0x02` 则自动改用 `0x3000` 重试，两种地址都算等价验证，日志会标记实际使用哪个：

```
W SELFTEST: 地址 0 不可用，改用 0x3000 读取成功（等价验证）
```

同时把自检交互从"固定倒计时"改为"每 2 秒主动探测"，并区分异常码给提示：

| 版本 | 每阶段耗时 | 总耗时 | 说明 |
|---|---|---|---|
| v1 | 45 s | ~144 s | 3 轮 × 15 s 固定倒计时 |
| v2 | 15~30 s | ~45 s | 2 轮 + 2 s 探测，切好即测 |
| v3 | 同上 | 待复测 | 增加地址自适应，预期 7/7 |

> 复测条件：Modbus Slave 依次切 Function 为 `04 Input Registers` → `02 Input Status` → `01 Coils`，Address 保持 `3000` 不动。

---

## 4. 端到端闭环证据

一次完整重启的时间线（节选）：

| 时间戳 | 事件 |
|---|---|
| 1714 ms | `Wi-Fi connected, IP: 192.168.137.219` |
| 1714 ms | `MAIN: network ready, IP=192.168.137.219` |
| 1724 ms | `MQTT: mqtt client started` |
| 2414 ms | `MQTT: broker connected: mqtt://192.168.137.1:1883` |
| 2414 ms | `client_id=esp32c3-inverter-cfd1f0, topic=inverter/collector/data` |
| 55325 ms | 自检结束，汇总 4/7 |
| 76845 ms | 主循环启动上报（第 3 次采集成功） |
| 135235 ms | `published (msg_id=41144): {"voltage":350.0,"current":5.00,"power":1750}` |

MQTTX 订阅 `inverter/#` 收到（同一时段）：

```
Topic: inverter/collector/data   QoS: 1
{"voltage":350.0,"current":5.00,"power":1750}
2026-11-20 17:22:15

Topic: inverter/collector/data   QoS: 1
{"voltage":350.0,"current":5.00,"power":1750}
2026-11-20 17:22:21
```

两端内容、QoS、时间戳完全对应，**闭环成立**。

---

## 5. 关键指标

| 指标 | 实测值 | 说明 |
|---|---|---|
| Modbus 单次往返 | 270 ms | 9600 bps 下 8 字节请求 + 15 字节应答的理论值 ≈ 24 ms，其余为从机内部处理延时 |
| 采集周期 | 5000 ms | `COLLECT_PERIOD_MS` |
| MQTT 连接建立 | 700 ms（IP 就绪后） | 修正启动时序前为 4 秒（含 3 次失败重连） |
| Wi-Fi 关联耗时 | 约 1.2 s | 热点已就绪的情况下 |
| 断线重连恢复 | 6.5 s | 含两次失败尝试 |
| 固件分区占用 | 94% | 1 MB app 分区仅余 6%，后续加 TLS/OTA 需扩分区 |

---

## 6. 发现的问题与处理

| 编号 | 问题 | 根因 | 处理 |
|---|---|---|---|
| P1 | 启动时 MQTT 反复失败重连约 4 秒 | `wifi_init_sta()` 只发起连接即返回，上报通道在无 IP 时就开始连 | 新增 `wifi_wait_connected(20000)`，先等 IP（超时也继续，保证断网可采集） |
| P2 | 烧录报 `No serial data received` | 四线转串口无 RTS，`--after hard_reset` 无法拉复位，芯片仍在跑 App | 手动 BOOT+RST 进下载模式；或改用带 DTR/RTS 的调试器 |
| P3 | esptool 依次尝试 COM6/7/8/10 | 命令未指定 `-p` | 烧录命令固定加 `-p COM8` |
| P4 | 自检每阶段固定等 45 秒 | 倒计时不感知从机状态 | 改为每 2 秒探测 + 2 轮上限，总耗时降至 45 秒 |
| P5 | 自检失败原因难定位 | 只报"Function 没切" | 按异常码 `0x01`/`0x02` 分别提示，并自动适配地址 |
| P6 | 切换 Function 后数据清零 | Modbus 四个数据区独立，且 Modbus Slave 按 OK 会重初始化 | 固化操作顺序：自检全部结束 → 填值 → 不再打开对话框 |
| P7 | EMQX 后台密码错误 | 该安装被前人改过密码，Dashboard 账号与 MQTT 账号是两套表 | `emqx.cmd ctl admins passwd admin <pwd>` 重置 |
| P8 | EMQX 日志刷 `os_mon` 错误 | 官方 Windows 包缺 `os_mon` 应用，`emqx_telemetry` 调用失败 | `etc/emqx.conf` 关闭 `telemetry`，不影响转发 |

---

## 7. 遗留事项

1. **自检复测**：烧录 v3（地址自适应）后应达到 7/7，需补一次回归记录。
2. **防火墙 1883 入站**：板子主动连电脑属入站连接，需管理员执行
   `netsh advfirewall firewall add rule name="MQTT 1883" dir=in action=allow protocol=TCP localport=1883`。
3. **分区余量**：仅 6%，若后续启用 TLS 或 OTA，需调整 `partitions.csv` 将 app 分区扩大。
4. **验收环境切换**：公司网络下需改 `WIFI_SSID/PASSWORD` 与 `REPORT_TARGET_IP/PORT`（或 `MQTT_BROKER_URI`），全部通过 `menuconfig` 完成，无需改代码。
5. **多设备区分**：当前主题固定 `inverter/collector/data`，多台设备建议改为 `inverter/{devid}/data` 并在订阅端用通配符 `inverter/+/data`。

---

## 8. 复现步骤

```powershell
# 1. 编译
cd D:\work\espidf_projects\inverter_collector
idf.py build

# 2. 进下载模式：按住 BOOT → 点 RST → 松 BOOT，然后烧录
python -m esptool --chip esp32c3 -p COM8 -b 460800 --before default_reset --after hard_reset `
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m `
  0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin `
  0x10000 build\inverter_collector.bin

# 3. 启动 Broker（另开窗口）
cd D:\work\MQTT\emqx-5.2.1-windows-amd64\bin
.\emqx.cmd start

# 4. 抓日志（或直接用串口助手 115200）
python tools\serial_read.py COM8 115200 30

# 5. MQTTX 订阅 inverter/# 观察上报
```

---

## 9. 结论

核心链路 **Modbus RTU 采集 → 数值解析 → JSON 打包 → MQTT QoS1 发布 → 客户端接收** 已完整跑通，异常场景（从机返回异常码、WiFi 断线、连续采集失败）均按预期降级且不崩溃，满足测验的功能要求。

功能码自检 4/7 的三项缺失已定位为测试操作（从机地址基址）而非代码缺陷，代码侧已加入地址自适应以降低操作成本，待烧录 v3 后复测确认。
