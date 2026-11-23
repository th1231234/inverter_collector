# 阶段二实现总结：MQTT 双向通信

> 对应考核指导书第二部分「MQTT 接入：查询、上报、修改」。
> 所有功能已上板实测通过（M-01 ~ M-08 全 PASS），固件 1,004,560 字节。

---

## 一、阶段二到底做了哪些事

阶段一只有一个方向：板子读 Modbus → 通过 UDP/TCP 单向发出去。阶段二加了**反方向**——上位机能发命令进来，板子执行后必须回执。这批工作拆成八项：

| 编号 | 要求 | 实现方式 | 关键代码位置 |
|---|---|---|---|
| M-01 | 连上 Broker，订阅两个下行主题 | `MQTT_EVENT_CONNECTED` 里调 `subscribe_cmd_topics()` | `mqtt_report.c` |
| M-02 | 每 5 秒上报电压/电流/功率 | `mqtt_report_send_json()` 发到 `data` 主题，载荷带 `state` | `mqtt_report.c` |
| M-03 | 上位机查寄存器，板子回报数值 | 订阅 `read` → 解析 → 入队 → `cmd_task` 执行功能码 03 | `mqtt_cmd.c` |
| M-04 | 上位机改寄存器，板子执行 | 订阅 `write` → 功能码 06 → 回 `{"result":"ok"}` | `mqtt_cmd.c` |
| M-05 | 每条应答能对上请求 | `req_id` 原样透传到 `resp` | `mqtt_cmd.c` |
| M-06 | Broker 挂了采集不能停 | esp-mqtt 自动重连 + 发布失败只记日志 | `mqtt_report.c` |
| M-07 | 坏数据不能把程序搞崩 | 三层校验：长度 → 主题 → 字段类型/范围 | `mqtt_cmd.c` |
| M-08 | 超长载荷要挡掉 | 512 字节上限 + 分片投递检测 | `mqtt_cmd.c` / `mqtt_report.c` |

再加一条综合验收要求：**从机掉线时 `state` 变 1，恢复后回 0**。这靠 `inverter_collector.c` 里缓存上次数值实现。

---

## 二、文件级改动总览

| 文件 | 状态 | 行数 | 干了什么 |
|---|---|---|---|
| `main/mqtt_cmd.c` | **新增** | 410 | 命令通道：订阅处理、JSON 校验、队列、Modbus 执行、应答组包 |
| `main/mqtt_cmd.h` | **新增** | 38 | 两个对外接口 |
| `main/mqtt_report.c` | **重写** | 195 | MQTT 客户端、事件回调、订阅、发布 |
| `main/mqtt_report.h` | **重写** | 68 | 四个主题宏 + 三个接口 |
| `main/modbus.c` | 大改 | 535（+440） | 总线互斥锁、通用读写接口、异常码记录 |
| `main/modbus_selftest.c` | **新增** | 368 | 七种功能码自检（调试用，默认关闭） |
| `main/modbus_selftest.h` | **新增** | 21 | 自检入口 |
| `main/inverter_collector.c` | 改 | 208 | 缓存上次数值、`state` 字段、等 Wi-Fi 就绪 |
| `main/wifi.c` / `.h` | 改 | 150 | 新增等连接、取 IP 两个接口 |
| `main/Kconfig.projbuild` | 改 | 115 | 删旧 `MQTT_TOPIC`，新增 `MQTT_DEV_ID` 与 Broker 参数 |
| `main/CMakeLists.txt` | 改 | — | 加入 `mqtt_cmd.c` |

---

## 三、新增函数清单

### 3.1 `mqtt_cmd.c` —— 命令通道（12 个函数，全新）

| 函数 | 职责 | 一句话说明 |
|---|---|---|
| `mqtt_cmd_init()` | 对外 | 创建队列 + 启动 `cmd_task` |
| `mqtt_cmd_handle_message()` | 对外 | MQTT 回调的入口，判长度、判主题、补 `\0` |
| `handle_cmd()` | 内部 | 解析 JSON、逐字段校验、入队或回错误 |
| `cmd_task()` | 内部 | 出队 → 调 Modbus → 按结果回应答 |
| `req_id_is_valid()` | 内部 | 白名单 `[A-Za-z0-9_-]`，长度 < 32 |
| `json_get_uint()` | 内部 | 取数字并校验范围，非数字类型直接拒 |
| `topic_is()` | 内部 | 按长度比主题（主题不带 `\0`） |
| `publish_resp()` | 内部 | cJSON 序列化后发到 `resp` 主题 |
| `send_resp_error()` | 内部 | 组 `{"req_id","result":"error","msg"}` |
| `send_resp_ok()` | 内部 | 组 `{"req_id","result":"ok"}` |
| `send_resp_ok_values()` | 内部 | 组 `{"req_id","result":"ok","values":[...]}` |
| `modbus_err_msg()` | 内部 | 把 Modbus 异常码翻译成可读字符串 |

### 3.2 `mqtt_report.c` —— MQTT 客户端（5 个函数）

| 函数 | 职责 |
|---|---|
| `mqtt_report_init()` | 配置并启动客户端，先初始化命令模块再启动 |
| `mqtt_report_is_connected()` | 查连接状态（发 `state=1` 前要判） |
| `mqtt_report_publish()` | 通用发布，QoS 1，长度用真实字节数 |
| `mqtt_report_send_json()` | 组业务载荷发到 `data` |
| `subscribe_cmd_topics()` | 订阅 `read` / `write` |
| `mqtt_event_handler()` | 五个事件分支：连上/断开/数据/错误 |

### 3.3 `modbus.c` —— 通用化 + 加锁（新增 9 个）

| 函数 | 职责 |
|---|---|
| `modbus_transact()` | **新增加锁包装**，原实现改名 `_locked` |
| `modbus_transact_locked()` | 原 `modbus_transact` 改的名，内部实现不变 |
| `modbus_last_exception()` | 返回最近一次从机异常码（0 = 超时） |
| `modbus_read_coils()` | 功能码 01 |
| `modbus_read_discrete_inputs()` | 功能码 02 |
| `modbus_read_holding()` | 功能码 03，带 `slave` 参数 |
| `modbus_read_input()` | 功能码 04 |
| `modbus_write_register()` | 功能码 06 |
| `modbus_write_registers()` | 功能码 16（0x10） |
| `modbus_read_bits()` / `modbus_read_regs()` | 位/字读取的公共实现 |
| `modbus_build_read_request()` | 四个读功能码共用的 8 字节帧构造 |
| `modbus_exception_str()` | 异常码转文字 |

### 3.4 `wifi.c` —— 等待就绪（新增 3 个）

| 函数 | 职责 |
|---|---|
| `wifi_wait_connected(timeout_ms)` | 阻塞等拿到 IP，最多 20 秒 |
| `wifi_is_connected()` | 查当前是否已连 |
| `wifi_get_ip()` | 取本机 IP 字符串 |

### 3.5 `modbus_selftest.c` —— 功能码自检（12 个，调试用）

`modbus_selftest_run()` 主入口 + `probe_read_holding()` / `probe_write_single()` / `probe_write_multi()` / `probe_read_input()` / `probe_read_discrete()` / `probe_read_coils()` / `probe_exception()` 七个探针 + 记录与切窗口的辅助函数。只用来验证协议栈，交付版本关闭。

---

## 四、核心函数代码

### 4.1 命令入口：先验长度，再验主题，最后补 `\0`

```c
void mqtt_cmd_handle_message(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    if (topic == NULL || data == NULL || topic_len <= 0 || data_len <= 0) {
        ESP_LOGW(TAG, "empty message ignored");
        return;
    }

    /* 先判长度再拷贝：1KB 超长载荷在这里被挡掉，不会撑爆本地缓冲 */
    if (data_len >= CMD_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "payload too long (%d >= %d), dropped", data_len, CMD_MAX_PAYLOAD);
        return;
    }

    bool is_write = false;
    if (topic_is(topic, topic_len, MQTT_TOPIC_WRITE)) {
        is_write = true;
    } else if (topic_is(topic, topic_len, MQTT_TOPIC_READ)) {
        is_write = false;
    } else {
        ESP_LOGW(TAG, "unexpected topic (len=%d), ignored", topic_len);
        return;
    }

    /* MQTT 载荷不保证以 '\0' 结尾，拷贝后手动补，再交给 cJSON */
    char payload[CMD_MAX_PAYLOAD];
    memcpy(payload, data, (size_t)data_len);
    payload[data_len] = '\0';

    handle_cmd(is_write, payload);
}
```

三个细节值得说：

- **`data_len >= 512` 就丢**，不等 cJSON 报错。缓冲区是栈上定长的，长度先卡死就不会溢出。
- **主题必须按长度比较**。`event->topic` 不以 `\0` 结尾，用 `strcmp` 会读越界。
- **`payload[data_len] = '\0'`**。MQTT 载荷是二进制安全的字节流，cJSON 需要 `\0` 结尾才能解析。

### 4.2 字段校验：坏 JSON 和越界分成两条路

```c
handle_cmd() 里三个关键判断：

/* ① 坏 JSON：取不到 req_id，无法组出合法应答，只能丢弃 */
cJSON *root = cJSON_Parse(payload);
if (root == NULL) {
    ESP_LOGE(TAG, "bad json on %s, dropped", topic);
    return;
}

/* ② req_id 不合法：同样丢弃，因为不知道回给谁 */
cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "req_id");
if (!cJSON_IsString(j_id) || j_id->valuestring == NULL ||
    !req_id_is_valid(j_id->valuestring)) {
    ESP_LOGE(TAG, "missing or illegal req_id on %s, dropped", topic);
    cJSON_Delete(root);
    return;
}

/* ③ 其他字段越界：req_id 已经有了，可以回错误 */
if (!json_get_uint(j_addr, 0, MODBUS_ADDR_MAX, &u)) {
    err = MSG_INVALID_PARAM;
    goto done;
}
```

这个区分是 M-07 的核心：**知道回给谁就回错误，不知道就只记日志**。坏 JSON 如果硬要回应答，只能编一个假 `req_id`，反而让上位机误以为对应上了。

配套的 `json_get_uint` 不接受字符串：

```c
static bool json_get_uint(const cJSON *item, unsigned min, unsigned max, unsigned *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;          /* "12288" 这种字符串也被拒 */
    }
    double d = item->valuedouble;
    if (d < (double)min || d > (double)max) {
        return false;
    }
    *out = (unsigned)d;
    return true;
}
```

### 4.3 执行任务：真正的 Modbus 读写放这里

```c
static void cmd_task(void *arg)
{
    cmd_req_t req;

    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (req.is_write) {
            bool ok = modbus_write_register(req.slave, req.addr, req.value);
            if (ok) {
                ESP_LOGI(TAG, "write ok: req_id=%s addr=0x%04X value=%u", ...);
                send_resp_ok(req.req_id);
            } else {
                send_resp_error(req.req_id, modbus_err_msg());
            }
            continue;
        }

        /* read：req.num 已在入队前限制在 1~16，缓冲区定长安全 */
        uint16_t regs[CMD_MAX_READ_NUM] = {0};
        bool ok = modbus_read_holding(req.slave, req.addr, req.num, regs);
        if (ok) {
            send_resp_ok_values(req.req_id, regs, (int)req.num);
        } else {
            send_resp_error(req.req_id, modbus_err_msg());
        }
    }
}
```

**为什么不直接在 MQTT 回调里读 Modbus**：

一次 Modbus 读最坏要 `200ms 超时 × 3 次重试 ≈ 600ms`。回调跑在 esp-mqtt 自己的任务里，在这里阻塞 600ms，协议栈就没法处理心跳包，超过 keepalive（60 秒）会被 Broker 踢下线，表现为「一发命令就掉线重连」。

所以回调只当**传达室**：验一下、拆一下、塞进队列，立刻返回。

### 4.4 事件回调：订阅写在 CONNECTED 里

```c
case MQTT_EVENT_CONNECTED:
    s_connected = true;
    ESP_LOGI(TAG, "broker connected: %s", CONFIG_MQTT_BROKER_URI);
    subscribe_cmd_topics();          /* 每次连上都要重新订阅 */
    break;

case MQTT_EVENT_DATA:
    /* 分片投递：data_len 只是分片长度，total_data_len 才是整条 */
    if (event->total_data_len != event->data_len) {
        ESP_LOGW(TAG, "fragmented payload (%d/%d bytes), dropped",
                 event->data_len, event->total_data_len);
        break;
    }
    mqtt_cmd_handle_message(event->topic, event->topic_len,
                            event->data, event->data_len);
    break;
```

**订阅必须写在 CONNECTED 回调**：默认 clean session 不保留会话状态，断线重连后订阅关系全部丢失。如果只在 `app_main` 里订阅一次，重连之后就变成「能上报但收不到命令」——这个故障现象很有迷惑性。

### 4.5 分片保护：一个容易漏掉的坑

esp-mqtt 的接收缓冲是 1024 字节，超过的消息会被拆成多片，**每片单独触发一次 `MQTT_EVENT_DATA`**。实测 1061 字节的载荷被拆成 1000 + 61 两片。

问题在于：单片 1000 字节的 JSON 是残缺的，cJSON 会解析失败；但**残片有可能恰好构成一个合法 JSON**，一旦被解析成功，就会执行一条本该被拒绝的命令。

所以判断依据不能只看长度，要看这两个字段是否相等：

```c
if (event->total_data_len != event->data_len) {   /* 不相等 = 这是碎片 */
    ESP_LOGW(TAG, "fragmented payload (%d/%d bytes), dropped", ...);
    break;                                        /* 整条丢弃，不尝试拼接 */
}
```

实测日志：

```text
W (12345) MQTT: fragmented payload (1000/1061 bytes), dropped
W (12345) MQTT: fragmented payload (61/1061 bytes), dropped
```

### 4.6 总线互斥锁：采集与命令不能同时动串口

阶段二之前只有采集任务用串口，没人抢。加了命令通道后，两个任务会同时操作同一个 UART：

- 采集任务每 5 秒读一次
- 命令任务随时可能插入

如果命令任务在采集任务「发完帧、还没收完」时插进去，两个响应会串在一起，CRC 校验必然失败。

解法是在事务层加锁：

```c
static bool modbus_transact(uint8_t slave, uint8_t func,
                            const uint8_t *tx, int tx_len,
                            uint8_t *rx, int rx_cap, int rx_expect, int *rx_len)
{
    if (s_bus_lock == NULL) {
        /* 还没初始化就调用属于编程错误，退化为无锁执行并留日志 */
        ESP_LOGW(TAG, "bus lock not init, run without lock");
        return modbus_transact_locked(slave, func, tx, tx_len, rx, rx_cap, rx_expect, rx_len);
    }

    if (xSemaphoreTake(s_bus_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "take bus lock failed");
        return false;
    }

    bool ok = modbus_transact_locked(slave, func, tx, tx_len, rx, rx_cap, rx_expect, rx_len);

    xSemaphoreGive(s_bus_lock);
    return ok;
}
```

**锁的粒度必须覆盖整个事务**：清缓冲 → 发帧 → 收帧 → CRC 校验 → 重试。只锁「发帧」是不够的，因为接收阶段同样共享同一个缓冲区。

用互斥量而不是二值信号量，是为了拿到优先级继承——命令任务持锁时如果采集任务抢 CPU，FreeRTOS 会临时把命令任务的优先级提到采集任务之上，避免优先级反转。

### 4.7 状态上报：读失败也要告诉上位机

```c
/* 成功时缓存本次结果 */
last_voltage = voltage;
last_current = current;
last_power   = power;
has_last     = true;

...

/* 失败时带上 state=1 和上次的数值 */
if (reporting && wifi_ok && has_last && mqtt_report_is_connected()) {
    ESP_LOGW(TAG, "report stale values with state=1 (modbus down)");
    mqtt_report_send_json(last_voltage, last_current, last_power, 1);
}
```

为什么不干脆不发：上位机分不清「设备掉线」和「网络抖动」。带上 `state=1` 和上次的数值，云端能立刻判断是「采集侧故障」而不是「设备失联」，同时还能看到故障前的最后读数。

---

## 五、三个关键设计决策

### 决策一：命令走队列，不走回调

| 方案 | 后果 |
|---|---|
| 回调里直接读 Modbus | 阻塞 600ms → 心跳超时 → 被 Broker 踢下线 |
| 回调只入队，独立任务执行 | 回调 <1ms 返回，协议栈正常工作 |

代价是多一个任务（栈 4096）和一个队列（深度 8）。队列满时明确回 `request_busy`，而不是静默丢弃——上位机至少知道请求没被接受。

### 决策二：主题在编译期拼好

```c
#define MQTT_TOPIC_READ   "dev/" CONFIG_MQTT_DEV_ID "/read"
```

C 语言的字面量拼接，编译期完成，运行时零开销。改工号只需改 menuconfig 里的 `CONFIG_MQTT_DEV_ID`，四个主题一起变，不会出现「改了三个漏了一个」。

### 决策三：`req_id` 严格白名单

```c
static bool req_id_is_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= CMD_MAX_REQ_ID) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}
```

只允许字母、数字、下划线、连字符。这样 `req_id` 拼进 JSON 时**不可能注入引号或反斜杠**，也就不需要为了转义去动态分配内存——在只有 400KB 可用堆的 MCU 上，这个约束换来的是「不会被构造出的恶意 `req_id` 撑爆内存」。

---

## 六、实测结果速查

| 编号 | 验证方式 | 结果 |
|---|---|---|
| M-01 | 串口日志 `subscribed "dev/intern01/read" (msg_id=41151)` | PASS |
| M-02 | MQTTX 每 5 秒一条 `{"voltage":350.0,...,"state":0}` | PASS |
| M-03 | `{"req_id":"m03","result":"ok","values":[3500,0,500,0,1750]}` | PASS |
| M-04 | write 回 ok，回读 0x3000 由 3500 → 3000 | PASS |
| M-05 | a/b/c 三条 `req_id` 一一对应 | PASS |
| M-06 | 停 EMQX 60 秒，`ok=39, err=0` 采集不停；重启后自动重连 | PASS |
| M-07 | 坏 JSON → `bad json ... dropped`；`addr=70000` → `invalid_param` | PASS |
| M-08 | 1061 字节拆成 1000+61 两片，均 `fragmented payload ... dropped` | PASS |

---

## 七、遗留事项

| 项目 | 状态 | 说明 |
|---|---|---|
| `app` 分区余量仅 4% | **已解决** | 改用 `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`，app 分区 1MB → 1500K，余量 4% → **35%**（531KB），已重新烧录验证 |
| M-07 超时项证据 | **已补齐** | 用不存在的站号 `slave=9` 造超时，3 次重试共 660ms，返回 `modbus_timeout`，采集不受影响 |
| `cmd_probe.py` 偶发丢应答 | **已修复** | subscribe 与 publish 分母，改在 SUBACK 回调里发请求；同时支持第 5 参数指定站号 |
| `git push` 未成功 | 待网络 | 代理返回 502、直连 443 超时，属环境限制 |

### 分区扩容说明

原来用 IDF 默认的 `partitions_singleapp.csv`，app 只有 1MB（0x100000）。阶段二固件已占 1,004,560 字节，只剩 4%。

改用 `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` 后的布局：

| Name | Type | SubType | Offset | Size |
|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 24K |
| phy_init | data | phy | 0xf000 | 4K |
| factory | app | factory | 0x10000 | **1500K** |

`nvs` 位置没变（仍在 0x9000），所以**已有 Wi-Fi 凭据不会丢**，不用重新配网。

代价：分区表内容变了，必须把 `partition-table.bin` 一起重烧，只烧 app 不够。

### git push 为何推不上去

两条路都试过了：

```text
走代理   → CONNECT tunnel failed, response 502
绕代理直连 → Failed to connect to github.com:443 after 21102 ms
```

不是仓库配置问题，是这台机器到 GitHub 的网络不通。等网络恢复后在**普通终端**（非沙箱）执行：

```powershell
cd D:\work\espidf_projects\inverter_collector
git push origin main
git push origin stage1 stage2
```

### EMQX 运行模式

当前 Broker 是 `emqx.cmd console` 启的（PID 24140，监听 0.0.0.0:1883）。这个实例在非交互会话里启不了新窗口，所以只能用 console。

验收前想换回 `start`：先 `emqx.cmd stop`，再到**桌面的 cmd 窗口**双击 `emqx.cmd start`（需要窗口站，沙箱里开不出来）。或者干脆继续用 console，功能完全一样，只是会占一个终端。

