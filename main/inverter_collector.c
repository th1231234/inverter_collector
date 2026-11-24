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

#define POINT_COUNT     3      /* 上报的测点个数 */

/* ------------------------------------------------------------------
 * 测点表：地址 / 在 regs[] 里的下标 / 物理值换算系数
 *
 * 三张表按下标一一对应，改寄存器映射时只动这里，业务代码不用改。
 * 换算系数用浮点常量：写成整数 10 会变成整数除法，3500/10 得 350 没问题，
 * 但 3549/10 会变成 354 而不是 354.9。
 * ------------------------------------------------------------------ */
static const uint16_t s_pt_addr[POINT_COUNT]  = {
    REG_BASE + IDX_VOLTAGE, REG_BASE + IDX_CURRENT, REG_BASE + IDX_POWER
};
static const int      s_pt_idx[POINT_COUNT]   = {
    IDX_VOLTAGE, IDX_CURRENT, IDX_POWER
};
static const float    s_pt_scale[POINT_COUNT] = {
    10.0f, 100.0f, 1.0f
};

#define COLLECT_PERIOD_MS       5000   /* 采集周期 5 秒 */
#define REPORT_START_THRESHOLD  3      /* 连续成功 3 次后启动上报 */

/* 把一次读到的寄存器填成测点数组 */
static void fill_points(const uint16_t *regs, codec_metric_t *pts)
{
    for (int i = 0; i < POINT_COUNT; i++) {
        pts[i].addr  = s_pt_addr[i];
        pts[i].raw   = regs[s_pt_idx[i]];
        pts[i].value = (float)pts[i].raw / s_pt_scale[i];
    }
}

/* ------------------------------------------------------------------
 * 采集任务：周期读取寄存器 → 解析 → UDP 上报
 * ------------------------------------------------------------------ */
static void collector_task(void *arg)
{
    uint16_t regs[REG_COUNT] = {0};
    uint32_t ok_cnt = 0;
    uint32_t err_cnt = 0;
    int consecutive_ok = 0;      /* 连续成功次数 */
    bool reporting = false;      /* 是否已启动上报 */
#if CONFIG_REPORT_PROTO_TCP
    bool prev_wifi_ok = false;   /* 上一周期的 Wi-Fi 状态，用于检测重连 */
#endif
#if CONFIG_REPORT_PROTO_MQTT
    /* 读失败时沿用上一次成功的测点，配合 state=1 告诉上位机「数据已过期」 */
    codec_metric_t last_pts[POINT_COUNT] = {0};
    bool     has_last = false;
    uint32_t seq      = 0;       /* 帧序号，成功发出一条才 +1，便于上位机发现丢帧 */
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

        bool ok = modbus_read_registers(REG_BASE, REG_COUNT, regs);

        if (ok) {
            ok_cnt++;
            consecutive_ok++;

            /* 原始寄存器值（Hex） */
            ESP_LOGI(TAG, "Raw: [0x%04X]=0x%04X [0x%04X]=0x%04X [0x%04X]=0x%04X",
                     (unsigned)(REG_BASE + IDX_VOLTAGE), regs[IDX_VOLTAGE],
                     (unsigned)(REG_BASE + IDX_CURRENT), regs[IDX_CURRENT],
                     (unsigned)(REG_BASE + IDX_POWER),   regs[IDX_POWER]);

            /* 填测点：地址 + 原始值 + 物理值。
             * 换算必须用浮点除法，避免整数除法丢精度（3549/10 要得 354.9）。 */
            codec_metric_t pts[POINT_COUNT];
            fill_points(regs, pts);

            ESP_LOGI(TAG, "Value: Voltage=%.1fV, Current=%.2fA, Power=%.0fW",
                     pts[0].value, pts[1].value, pts[2].value);

#if CONFIG_REPORT_PROTO_MQTT
            /* 缓存本次结果，供读失败时带 state=1 上报 */
            memcpy(last_pts, pts, sizeof(pts));
            has_last = true;
#endif

            /* 连续成功 3 次后，才启动上报 */
            if (!reporting && consecutive_ok >= REPORT_START_THRESHOLD) {
                reporting = true;
                ESP_LOGI(TAG, "%d consecutive reads OK, start %s reporting",
                         REPORT_START_THRESHOLD, REPORT_CHANNEL_NAME);
            }

            /* 上报：Wi-Fi 未连接时跳过，不影响采集 */
            if (reporting) {
                if (wifi_ok) {
#if CONFIG_REPORT_PROTO_TCP
                    tcp_send_json(pts[0].value, pts[1].value, pts[2].value);
#elif CONFIG_REPORT_PROTO_MQTT
                    if (mqtt_report_send_telemetry(seq, 0, pts, POINT_COUNT)) {
                        seq++;      /* 只有真正发出去了才推进序号 */
                    }
#else
                    udp_send_json(pts[0].value, pts[1].value, pts[2].value);
#endif
                } else {
                    ESP_LOGW(TAG, "Wi-Fi not connected, skip this report");
                }
            }

            ESP_LOGI(TAG, "Stats: ok=%lu, err=%lu",
                     (unsigned long)ok_cnt, (unsigned long)err_cnt);

        } else {
            /* 采集失败：不崩溃、不退出，下个周期继续（故障注入要求） */
            err_cnt++;
            consecutive_ok = 0;
            ESP_LOGE(TAG, "read failed, retry next cycle (ok=%lu, err=%lu)",
                     (unsigned long)ok_cnt, (unsigned long)err_cnt);

#if CONFIG_REPORT_PROTO_MQTT
            /* 读失败也要让上位机知道设备异常:带 state=1 照常上报,
             * 数值沿用上一次成功读取的缓存,恢复后自动回到 state=0。
             * 这是综合验收第 4 条「拔掉从机 → state=1，恢复后回 0」的要求。 */
            if (reporting && wifi_ok && has_last && mqtt_report_is_connected()) {
                ESP_LOGW(TAG, "report stale values with state=1 (modbus down)");
                if (mqtt_report_send_telemetry(seq, 1, last_pts, POINT_COUNT)) {
                    seq++;
                }
            }
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
