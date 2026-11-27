#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "modbus.h"
#include "wifi.h"

#if CONFIG_MODBUS_SELFTEST
#include "modbus_selftest.h"
#endif

/* 上报通道三选一，由 menuconfig 决定（Kconfig.projbuild → 上报通道） */
#if CONFIG_REPORT_PROTO_TCP
#include "tcp.h"
#define REPORT_CHANNEL_NAME   "TCP"
#elif CONFIG_REPORT_PROTO_MQTT
#include "mqtt_report.h"
#define REPORT_CHANNEL_NAME   "MQTT"
#else
#include "udp.h"
#define REPORT_CHANNEL_NAME   "UDP"
#endif

/* 测点类型来自编解码层；UDP/TCP 通道也用它承载一个测点，
 * 只用到 addr/raw/value 三个字段，不涉及 protobuf。 */
#include "telemetry_codec.h"

static const char *TAG = "MAIN";

/* ==================================================================
 * 从机寄存器映射（逆变器正式映射，Modbus Slave 需按 Hex 地址 3000 配置）
 *  0x3000 电网电压  = 0x0DAC = 3500 → 350.0V  (÷10.0)
 *  0x3001 未使用
 *  0x3002 电网电流  = 0x01F4 = 500  → 5.00A   (÷100.0)
 *  0x3003 未使用
 *  0x3004 瞬时功率  = 0x06D6 = 1750 → 1750W   (÷1)
 * ------------------------------------------------------------------
 * 注意：三个参数地址步长为 2，因此一次连续读 5 个寄存器，
 *       再按偏移 0 / 2 / 4 取值。
 * ================================================================== */
#define REG_BASE        0x3000
#define REG_COUNT       5
#define IDX_VOLTAGE     0      /* 0x3000 */
#define IDX_CURRENT     2      /* 0x3002 */
#define IDX_POWER       4      /* 0x3004 */

/* ------------------------------------------------------------------
 * 测点表：一台从机的三个量，合成一张表
 *
 * 原来这里是"地址 / 下标 / 系数"三张平行数组，靠下标隐式对应。
 * 加站号之后字段变多，平行数组就变成四个，很容易改错一个漏改三个，
 * 编译器也查不出来。合并成一个结构体数组后，一个测点的所有属性
 * 写在同一行，增删测点就是增删一行。
 *
 * decimals 而不是 printf 格式串：格式串传给 snprintf 属于"非字面量格式"，
 * 打开 -Wformat-nonliteral 会报警；用"小数位数"表达同样清楚且没有这个坑。
 *
 * 换算必须用浮点常量：写成整数 10 会变成整数除法，3500/10 得 350 没问题，
 * 但 3549/10 会变成 354 而不是 354.9。
 * ------------------------------------------------------------------ */
typedef struct {
    const char *name;      /* 日志里显示的名字 */
    uint16_t    addr;      /* 寄存器地址 */
    uint16_t    reg_idx;   /* 在 regs[] 里的下标 */
    float       scale;     /* 物理值 = raw / scale */
    const char *unit;      /* 单位 */
    int         decimals;  /* 打印物理值时保留几位小数 */
} point_cfg_t;

static const point_cfg_t s_points[] = {
    /* name       地址                   下标          系数    单位  小数 */
    { "Voltage", REG_BASE + IDX_VOLTAGE, IDX_VOLTAGE,  10.0f, "V",  1 },
    { "Current", REG_BASE + IDX_CURRENT, IDX_CURRENT, 100.0f, "A",  2 },
    { "Power",   REG_BASE + IDX_POWER,   IDX_POWER,     1.0f, "W",  0 },
};
#define POINT_PER_SLAVE  ((int)(sizeof(s_points) / sizeof(s_points[0])))

/* ==================================================================
 * 从机拓扑：一台 ESP32 下挂几个 Modbus 从机，就在这张表里加几行
 * ------------------------------------------------------------------
 * 现场常见做法是一条 RS-485 总线并多台设备，靠站号区分。ESP32 侧
 * 仍然只需要一条 UART：帧里带站号，从机自己判断该不该应答 ——
 * 站号不匹配的从机完全沉默，不会抢占总线。
 *
 * 加一台从机 = 加一行，采集循环和上报代码都不用动。
 *
 * ⚠️ 只接 1 台从机做阶段一/二验收时，把 2 号、3 号两行注释掉：
 *     只留 1 号 → 行为和阶段一完全一致，state 永远是 0
 *     三台都在 → 少一台就会报 state=1（"配置了但没读到"是真故障）
 * ================================================================== */
typedef struct {
    uint8_t  slave;      /* 站号 1~247，必须与从机上设置的一致 */
    uint16_t reg_base;   /* 起始寄存器地址 */
    uint16_t reg_count;  /* 从起始地址连续读几个寄存器 */
} slave_cfg_t;

static const slave_cfg_t s_slaves[] = {
    { 1, REG_BASE, REG_COUNT },
    { 2, REG_BASE, REG_COUNT },
    { 3, REG_BASE, REG_COUNT },
};
#define SLAVE_COUNT   ((int)(sizeof(s_slaves) / sizeof(s_slaves[0])))

/* 一帧要装下"所有从机的所有测点"，所以总数是相乘而不是单台的 3 个 */
#define POINT_COUNT   (POINT_PER_SLAVE * SLAVE_COUNT)

/* tcp_send_json / udp_send_json 的 JSON 固定是「电压/电流/功率」三个字段，
 * 直接取 pts[0..2]。所以测点表不能少于 3 项 —— 少于 3 项就在编译期报错，
 * 而不是等到运行期读出越界的内存。 */
_Static_assert(POINT_PER_SLAVE >= 3, "tcp/udp json needs at least 3 points");

#define COLLECT_PERIOD_MS       5000   /* 采集周期 5 秒 */
#define REPORT_START_THRESHOLD  3      /* 连续成功 3 次后启动上报 */

/* 把一台从机读回的寄存器，填进测点数组从 base 开始的连续几项 */
static void fill_points(uint8_t slave, const uint16_t *regs,
                        codec_metric_t *pts, int base)
{
    for (int i = 0; i < POINT_PER_SLAVE; i++) {
        codec_metric_t *p = &pts[base + i];
        p->slave = slave;
        p->addr  = s_points[i].addr;
        p->raw   = regs[s_points[i].reg_idx];
        p->value = (float)p->raw / s_points[i].scale;
    }
}

/* ------------------------------------------------------------------
 * 拼一行日志，例：
 *   raw 0x3000=0x0DAC 0x3002=0x01F4 0x3004=0x06D6 | 350.0V 5.00A 1750W
 *
 * 每个 snprintf 都要判返回值：它返回的是"本该写入的长度"，缓冲不够时
 * 会大于剩余空间。直接累加 off 就会越界，所以不够就地截断。
 * ------------------------------------------------------------------ */
static void format_slave_line(const uint16_t *regs, const codec_metric_t *pts,
                              int base, char *buf, size_t size)
{
    size_t off = 0;
    int    n;

    buf[0] = '\0';

    n = snprintf(buf, size, "raw");
    if (n > 0 && (size_t)n < size) {
        off = (size_t)n;
    }

    for (int i = 0; i < POINT_PER_SLAVE && off < size - 1; i++) {
        n = snprintf(buf + off, size - off, " 0x%04X=0x%04X",
                     (unsigned)s_points[i].addr,
                     (unsigned)regs[s_points[i].reg_idx]);
        if (n < 0 || (size_t)n >= size - off) {
            return;                 /* 缓冲已满，剩下的不写了 */
        }
        off += (size_t)n;
    }

    if (off < size - 1) {
        n = snprintf(buf + off, size - off, " |");
        if (n > 0 && (size_t)n < size - off) {
            off += (size_t)n;
        }
    }

    for (int i = 0; i < POINT_PER_SLAVE && off < size - 1; i++) {
        n = snprintf(buf + off, size - off, " %.*f%s",
                     s_points[i].decimals, (double)pts[base + i].value,
                     s_points[i].unit);
        if (n < 0 || (size_t)n >= size - off) {
            return;
        }
        off += (size_t)n;
    }
}

/* ------------------------------------------------------------------
 * 采集任务：周期轮询所有从机 → 解析 → 上报
 *
 * 和单从机版本的唯一区别在 while 里那个 for：
 * 原来读一次、填 3 个点；现在按 s_slaves[] 逐台读、分别填各自那一段。
 * 上报仍然是一帧发完所有测点，上位机拿到的永远是同一时刻的整机快照。
 * ------------------------------------------------------------------ */
static void collector_task(void *arg)
{
    (void)arg;

    uint32_t ok_cnt = 0;
    uint32_t err_cnt = 0;
    int  consecutive_ok = 0;     /* 连续"至少读到一台"的周期数 */
    bool reporting = false;      /* 是否已启动上报 */

    /* 整机快照（跨所有从机）。
     * 某一台掉线时，它那一段保留上一次的值，配合 state=1 告诉上位机
     * "这几个数已过期"，其余从机照常是新值 —— 一帧就能表达完整现场。 */
    codec_metric_t last_pts[POINT_COUNT] = {0};
    bool           has_snapshot = false;

#if CONFIG_REPORT_PROTO_TCP
    bool prev_wifi_ok = false;   /* 上一周期的 Wi-Fi 状态，用于检测重连 */
#endif
#if CONFIG_REPORT_PROTO_MQTT
    uint32_t seq = 0;            /* 帧序号，成功发出一条才 +1，便于上位机发现丢帧 */
#endif

    while (1) {
        /* --- Wi-Fi 状态变化检测 -------------------------------------
         * UDP 无连接，掉线后直接续发即可；
         * TCP 有连接状态，掉线后 socket 会变成「半死」，必须主动关闭，
         * 下个周期发送时再重建，否则 send 会一直失败。
         * ------------------------------------------------------------ */
        bool wifi_ok = wifi_is_connected();
#if CONFIG_REPORT_PROTO_TCP
        if (wifi_ok && !prev_wifi_ok) {
            ESP_LOGI(TAG, "Wi-Fi (re)connected, reset TCP link");
            tcp_client_close();
        }
        prev_wifi_ok = wifi_ok;
#endif

        /* 本轮快照先整体继承上一轮，读成功的从机再覆盖自己那一段 */
        codec_metric_t pts[POINT_COUNT];
        memcpy(pts, last_pts, sizeof(pts));

        int  fresh  = 0;         /* 本轮读成功的从机数 */
        int  failed = 0;         /* 本轮读失败的从机数 */
        char line[128];

        for (int s = 0; s < SLAVE_COUNT; s++) {
            const slave_cfg_t *cfg  = &s_slaves[s];
            const int          base = s * POINT_PER_SLAVE;
            uint16_t           regs[REG_COUNT] = {0};

            if (modbus_read_holding(cfg->slave, cfg->reg_base,
                                    cfg->reg_count, regs)) {
                ok_cnt++;
                fresh++;
                fill_points(cfg->slave, regs, pts, base);
                format_slave_line(regs, pts, base, line, sizeof(line));
                ESP_LOGI(TAG, "slave %-3u %s", (unsigned)cfg->slave, line);
            } else {
                /* 采集失败：不崩溃、不退出，下个周期继续（故障注入要求） */
                err_cnt++;
                failed++;
                ESP_LOGE(TAG, "slave %-3u read failed, keep last values",
                         (unsigned)cfg->slave);
            }
        }

        /* 只要有一台读到就算"本轮有数据"，全灭才清零。
         * 这样只挂 1 台从机时也能正常启动上报（与阶段一行为一致）。 */
        if (fresh > 0) {
            consecutive_ok++;
            memcpy(last_pts, pts, sizeof(last_pts));
            has_snapshot = true;
        } else {
            consecutive_ok = 0;
        }

        /* 配置了几台就要几台全读到才算正常；少一台是真故障。
         * 这是验收第 4 条「拔掉从机 → state=1，恢复后回 0」的多从机版。 */
        int32_t state = (failed == 0) ? 0 : 1;

        ESP_LOGI(TAG, "Stats: slave %d/%d ok, state=%d, read ok=%lu err=%lu",
                 fresh, SLAVE_COUNT, (int)state,
                 (unsigned long)ok_cnt, (unsigned long)err_cnt);

        /* 连续 3 个周期都有数据，才启动上报 */
        if (!reporting && consecutive_ok >= REPORT_START_THRESHOLD) {
            reporting = true;
            ESP_LOGI(TAG, "%d consecutive cycles with data, start %s reporting",
                     REPORT_START_THRESHOLD, REPORT_CHANNEL_NAME);
        }

        if (!reporting) {
            /* 还没攒够连续次数，本周期不上报 */
        } else if (!wifi_ok) {
            ESP_LOGW(TAG, "Wi-Fi not connected, skip this report");
        } else if (!has_snapshot) {
            ESP_LOGW(TAG, "no valid sample yet, skip this report");
        } else {
#if CONFIG_REPORT_PROTO_TCP
            /* TCP/UDP 是阶段一/二的旧通道，只承载第一台从机的三个量 */
            tcp_send_json(pts[0].value, pts[1].value, pts[2].value);
#elif CONFIG_REPORT_PROTO_MQTT
            if (mqtt_report_send_telemetry(seq, state, pts, POINT_COUNT)) {
                seq++;           /* 只有真正发出去了才推进序号 */
            }
#else
            udp_send_json(pts[0].value, pts[1].value, pts[2].value);
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(COLLECT_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== inverter collector start =====");

    /* 1. Wi-Fi：连接路由器并自动重连
     *    wifi_init_sta() 只是"发起连接"，真正的连接结果由事件回调异步给出
     * ------------------------------------------------------------------ */
    wifi_init_sta();

    /* 1.5 等 Wi-Fi 真正拿到 IP 再往下走
     *     否则 mqtt_report_init() 会在没有网络的情况下白连失败几轮，
     *     日志里会出现一串容易误判成"WiFi 没连上"的 transport error。
     *     最多等 20 秒；超时也继续（断网不影响本地采集，恢复后自动续传）。 */
    if (wifi_wait_connected(20000)) {
        ESP_LOGI(TAG, "network ready, IP=%s", wifi_get_ip());
    } else {
        ESP_LOGW(TAG, "Wi-Fi not ready in 20s, continue anyway (collector keeps retrying)");
    }

    /* 2. 上报通道：UDP 直接建 socket；TCP 首次发送时建立连接；
     *    MQTT 启动客户端，连接与重连由 esp-mqtt 内部处理 */
#if CONFIG_REPORT_PROTO_TCP
    tcp_client_init();
    ESP_LOGI(TAG, "report channel: %s -> %s:%d",
             REPORT_CHANNEL_NAME, CONFIG_REPORT_TARGET_IP, CONFIG_REPORT_TARGET_PORT);
#elif CONFIG_REPORT_PROTO_MQTT
    mqtt_report_init();
    ESP_LOGI(TAG, "report channel: %s -> %s (pub \"%s\")",
             REPORT_CHANNEL_NAME, CONFIG_MQTT_BROKER_URI, MQTT_TOPIC_DATA);
#else
    udp_client_init();
    ESP_LOGI(TAG, "report channel: %s -> %s:%d",
             REPORT_CHANNEL_NAME, CONFIG_REPORT_TARGET_IP, CONFIG_REPORT_TARGET_PORT);
#endif

    /* 3. Modbus：初始化采集串口 */
    modbus_init();

#if CONFIG_MODBUS_SELFTEST
    /* 训练用：先跑功能码自检，跑完再进入正常采集 */
    modbus_selftest_run();
#endif

    /* 4. 采集任务，栈 4096（题目建议值） */
    xTaskCreate(collector_task, "collector", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "collector task created, period=%d ms", COLLECT_PERIOD_MS);
}
