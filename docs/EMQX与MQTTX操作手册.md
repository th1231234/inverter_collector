# EMQX 与 MQTTX 操作手册

本文说明如何在本机启动 EMQX 作为 MQTT Broker，并用 MQTTX 图形客户端订阅 ESP32-C3 上报的逆变器数据。

---

## 1. 角色分工

| 角色 | 由谁承担 | 职责 |
|---|---|---|
| Broker（服务器） | EMQX 5.2.1（本机 Windows） | 接收发布、转发订阅，不保存历史消息 |
| 发布端 | ESP32-C3 `inverter_collector` | 每 5 秒发布 `data`；收到 `read`/`write` 后回 `resp` |
| 订阅端 | MQTTX（图形化） | 订阅 `dev/intern01/#`，看上报并下发命令 |

**四个主题**（工号 `intern01`，改工号见 `MQTT_DEV_ID`）

| 方向 | 主题 | 谁发 |
|---|---|---|
| 下行 | `dev/intern01/read` / `dev/intern01/write` | 上位机 → 设备 |
| 上行 | `dev/intern01/resp` / `dev/intern01/data` | 设备 → 上位机 |

> MQTTX 只是客户端，**不能替代 Broker**。没有 Broker 运行时，MQTTX 只会显示连接失败。

> 概念层面（协议与软件的区别、1883 端口、热点与防火墙三个关卡）的解释见 **[`MQTT概念澄清.md`](MQTT概念澄清.md)**；本文只讲操作步骤。

---

## 2. 前置条件

| 项目 | 值 |
|---|---|
| EMQX 安装目录 | `D:\work\MQTT\emqx-5.2.1-windows-amd64` |
| MQTT 端口 | `1883`（明文） |
| Dashboard 端口 | `18083`（网页） |
| MQTT 账号 | `esp32` / `123456` |
| 订阅主题 | `dev/intern01/#` |

启动前确认 1883 未被占用（若曾用 `tools/mqtt_lab` 的 amqtt 启动过，请先停掉）：

```powershell
netstat -ano | findstr :1883
```

无输出表示端口空闲，可以继续。

---

## 3. 启动 EMQX

### 3.1 打开 PowerShell

普通权限即可，**不需要管理员**。只有 `emqx.cmd install`（注册系统服务）才需要提权，本场景不用。

### 3.2 进入 bin 目录

```powershell
cd D:\work\MQTT\emqx-5.2.1-windows-amd64\bin
```

> 上面的路径如与本机不符，请以实际安装位置为准；目录内应有 `emqx.cmd`（约 7.3 KB）。

### 3.3 启动

```powershell
.\emqx.cmd start
```

预期输出：

```
Starting emqx on node emqx@127.0.0.1
Start ok
```

- 首次启动约需 5~15 秒（Erlang 虚拟机上要加载 100 多个应用）。
- 命令返回后窗口回到提示符是**正常现象**，EMQX 已在后台常驻，无需保持窗口开启。
- 调试期若想实时看启动日志，改用 `.\emqx.cmd console`（前台运行，关闭窗口即停止）。

---

## 4. 验证启动结果

```powershell
.\emqx.cmd ctl status
```

预期：

```
Node 'emqx@127.0.0.1' 5.2.1 is started
```

再确认端口监听：

```powershell
netstat -ano | findstr ":1883 :18083"
```

应看到两条 `LISTENING` 记录：`1883` 负责 MQTT 报文，`18083` 负责 Dashboard 网页。

---

## 5. 登录 Dashboard（可选）

浏览器打开 `http://localhost:18083`，账号 `admin`，密码 `admin12345`。

| 用途 | 地址 |
|---|---|
| 客户端列表（确认板子是否连上） | `http://localhost:18083/#/clients` |
| 订阅关系 | `http://localhost:18083/#/subscriptions` |
| 认证配置（建 MQTT 用户） | `http://localhost:18083/#/authentication` |
| 客户端授权（ACL） | `http://localhost:18083/#/authorization` |

> 认证（Authentication）与授权（Authorization）是两件事：前者决定"能不能连"，后者决定"连上后能发哪些主题"。本项目只需要前者。

若提示密码错误（该安装可能被他人改过密码）：

```powershell
.\emqx.cmd ctl admins passwd admin admin12345
```

返回 `ok` 即重置成功。

---

## 6. MQTTX 建立连接

打开 MQTTX → 左下角 **新建连接**，按下表填写：

| 字段 | 填写值 | 说明 |
|---|---|---|
| Name | `local-emqx` | 任意备注名 |
| Host | `127.0.0.1` | 协议选 `mqtt://` |
| Port | `1883` | 勿填 8883（TLS 专用） |
| Username | `esp32` | MQTT 账号，不是 Dashboard 的 admin |
| Password | `123456` | 同上 |
| Client ID | 留空自动生成 | 板子用 MAC 生成，二者不冲突 |

点击 **Connect**，右上角显示绿色状态即连接成功。

> Dashboard 账号（`admin`）与 MQTT 账号（`esp32`）是两套独立表，不能混用。

---

## 7. 订阅主题

1. 左下角点击 **「+ New Subscription」**
2. **Topic** 填 `dev/intern01/#`
3. **QoS** 选 `1`
4. 点击 **Confirm**

`#` 为多层通配符，可匹配 `dev/intern01/data`、`dev/intern01/resp` 及后续新增的任何子主题。

---

## 8. 板子侧配置对照

固件通过 `menuconfig → Inverter Collector → 上报通道` 配置，参数应与 Broker 一致：

| 配置项 | 值 |
|---|---|
| `REPORT_PROTO` | `MQTT` |
| `MQTT_BROKER_URI` | `mqtt://192.168.137.1:1883` |
| `MQTT_DEV_ID` | `intern01`（拼四个主题，同时作 Client ID） |
| `MQTT_USERNAME` | `esp32` |
| `MQTT_PASSWORD` | `123456` |

> 板子内填写的是 `192.168.137.1`（Windows 热点网卡固定地址），**不是 `127.0.0.1`** —— 后者在板子上指向板子自己。MQTTX 跑在电脑上，因此用 `127.0.0.1`。

---

## 9. 数据验证

板子上电或按一下 RST，约 10 秒内 MQTTX 消息列表出现：

```json
{"voltage":350.0,"current":5.00,"power":1750}
```

每 5 秒一条，QoS 1。

### 9.1 显示为 0 时

说明 Modbus Slave 侧的寄存器未填值。在 **03 Holding Registers** 表中填写：

| 地址（Hex） | 填入值 | 板子换算结果 |
|---|---|---|
| `3000` | `3500` | 350.0 V |
| `3002` | `500` | 5.00 A |
| `3004` | `1750` | 1750 W |

> 填值必须在自检全部结束之后进行。Modbus 的 `0x`/`1x`/`3x`/`4x` 是四个独立数据区，切换 Function 或按 OK 会重新初始化当前表，之前填的值会清零。

### 9.2 数值偏小 10 倍 / 100 倍时

换算公式固定：电压 = 寄存器值 ÷ 10.0，电流 = 寄存器值 ÷ 100.0，功率 = 寄存器值。

填 `350` 得到 `35.0V` 属于公式正确、数值填错，应填 `3500`。

---

## 10. 停止 EMQX

```powershell
.\emqx.cmd stop
```

不要直接关电源或强杀进程，可能残留锁文件导致下次启动报节点冲突。

---

## 11. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `Start failed` / `Address already in use` | 1883 被占用（amqtt 仍在运行） | `netstat -ano \| findstr :1883` 取 PID，`taskkill /PID <pid> /F` |
| 命令瞬间返回，但 1883 无监听 | 启动失败未提示 | 用 `.\emqx.cmd console` 前台运行查看报错 |
| Dashboard 密码错误 | 密码被更改 | `.\emqx.cmd ctl admins passwd admin admin12345` |
| MQTTX 能连，板子连不上 | 板子连接属入站流量，被防火墙拦截 | 管理员执行：`netsh advfirewall firewall add rule name="MQTT 1883" dir=in action=allow protocol=TCP localport=1883` |
| 板子日志反复 `MQTT_EVENT_DISCONNECTED` | 地址或账号不符 | 核对 `192.168.137.1`、`esp32/123456`；确认热点已开启 |
| 板子连上后立刻被踢 | client_id 冲突或密码错 | client_id 由 MAC 生成，正常不冲突；核对密码 |
| 日志刷 `os_mon` / `memsup` 错误 | 官方 Windows 包缺少该 Erlang 应用 | 已在 `etc/emqx.conf` 关闭 telemetry，可忽略，不影响报文转发 |
| 重启电脑后连不上 | 未注册为系统服务，不会自启 | 每次开机重新执行第 3 步（`install` 需管理员且易报 1060，不建议） |

---

## 12. 与 amqtt 实验台的关系

`tools/mqtt_lab/broker.yaml` 对应的是另一套 Broker（amqtt，纯 Python 实现），用于理解协议帧，**与正式验收链路无关**。

| 对比项 | EMQX（本文） | amqtt（实验台） |
|---|---|---|
| 启动方式 | `emqx.cmd start` | `amqtt.exe -c tools\mqtt_lab\broker.yaml` |
| 认证 | 用户名密码 `esp32/123456` | 匿名（`allow_anonymous: true`） |
| 端口 | 1883 | 1883 |
| 适用场景 | 正式联调、演示 | 学习协议、抓原始字节 |

两者都占用 1883，**不能同时运行**。使用 MQTTX 观察板子数据时，用 EMQX 即可，无需启动 amqtt。

---

## 13. 附：命令行验证（不依赖图形界面）

```powershell
cd D:\work\espidf_projects\inverter_collector\tools\mqtt_lab

# 订阅（参数：host port topic user pass）
python lab_sub.py 127.0.0.1 1883 dev/intern01/# esp32 123456

# 发布测试（参数：host port topic 条数 间隔 用户 密码）
python lab_pub.py 127.0.0.1 1883 inverter/test 5 1 esp32 123456
```

> `lab_sub.py` / `lab_pub.py` 依赖 `paho-mqtt`，已装在 ESP-IDF 的 Python 环境
> `D:\work\Espressif\python_env\idf5.5_py3.11_env` 中。
