# MQTT 本地实验台

一套可以在自己电脑上跑通的 MQTT 收发环境，不依赖任何外网服务器。

## 文件说明

| 文件 | 作用 |
|---|---|
| `broker.yaml` | Broker（服务器）配置：监听 0.0.0.0:1883，允许匿名登录 |
| `lab_sub.py` | 订阅端，连上后一直挂在后台收消息 |
| `lab_pub.py` | 发布端，往主题发 JSON，模拟 ESP32 上报 |

## 三个终端，按顺序开

**终端 1 —— 启动服务器（这个窗口别关）**

```powershell
cd D:\work\espidf_projects\inverter_collector
D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\amqtt.exe -c tools\mqtt_lab\broker.yaml
```

出现类似下面的输出说明启动成功，端口 1883 开始监听：

```
[INFO] amqtt.broker: Listener 'default' bind to 0.0.0.0:1883 (max_connections=-1)
```

**终端 2 —— 启动订阅端（这个窗口也别关）**

```powershell
cd D:\work\espidf_projects\inverter_collector
D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe tools\mqtt_lab\lab_sub.py
```

**终端 3 —— 发数据**

```powershell
cd D:\work\espidf_projects\inverter_collector
D:\work\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe tools\mqtt_lab\lab_pub.py
```

发完立刻回到**终端 2**，应该能看到 5 条消息。同时在**终端 1** 里能看到
broker 打印的连接、订阅、发布事件 —— 这是理解"broker 只是中转站"最直观的证据。

## 参数自定义

```powershell
python lab_sub.py 127.0.0.1 1883 "inverter/#"
python lab_pub.py 127.0.0.1 1883 "inverter/collector/data" 10 2
#                 主机      端口  主题                      条数 间隔秒
```

## 接 ESP32 时要改的地方

ESP32 连本机 broker，地址用**电脑在热点网段里的那个 IP**，不是 127.0.0.1
（127.0.0.1 在板子看来是"它自己"，永远连不上）。

Windows 开热点后，本机的热点网卡地址固定是 `192.168.137.1`，板子会拿到
`192.168.137.x`。所以：

```powershell
idf.py menuconfig
```

`上报通道 → MQTT Broker 地址(URI)` 改成：

```
mqtt://192.168.137.1:1883
```

## 排查清单

| 现象 | 原因 | 解决 |
|---|---|---|
| `OSError: [WinError 10048] 通常每个套接字地址只允许使用一次` | 1883 端口已被占用（上次的 broker 没关） | `netstat -ano \| findstr :1883` 找到 PID，`taskkill /F /PID <pid>` |
| 订阅端连上了但屏幕一片空白 | Python 输出缓冲（已在本脚本中修复） | 若用别的脚本，改用 `python -u xxx.py` |
| 板子连不上 broker | Windows 防火墙拦了入站 1883 | 见下方命令 |
| 订阅不到消息 | 主题写错，或 `#` 被 shell 当成注释 | Windows cmd 下给主题加双引号 |

防火墙放行（管理员 PowerShell 执行一次即可）：

```powershell
netsh advfirewall firewall add rule name="MQTT 1883" dir=in action=allow protocol=TCP localport=1883
```

## 为什么不用公网 broker.emqx.io

公网 broker 谁都能连、谁都能看到你发的主题，而且必须能上外网。自己搭的 broker：

- 断网也能用，验收演示不怕现场没网
- 数据不出局域网
- 可以随便改端口、加密码，理解完整链路

生产环境才会考虑 TLS（8883）+ 用户名密码 + ACL 权限控制。
