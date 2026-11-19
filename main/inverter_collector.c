#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "modbus.h"
#include "udp.h"
#include "wifi.h"

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

#define COLLECT_PERIOD_MS       5000   /* 采集周期 5 秒 */
#define REPORT_START_THRESHOLD  3      /* 连续成功 3 次后启动上报 */

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

    while (1) {
        bool ok = modbus_read_registers(REG_BASE, REG_COUNT, regs);

        if (ok) {
            ok_cnt++;
            consecutive_ok++;

            /* 原始寄存器值（Hex） */
            ESP_LOGI(TAG, "Raw: [0x%04X]=0x%04X [0x%04X]=0x%04X [0x%04X]=0x%04X",
                     (unsigned)(REG_BASE + IDX_VOLTAGE), regs[IDX_VOLTAGE],
                     (unsigned)(REG_BASE + IDX_CURRENT), regs[IDX_CURRENT],
                     (unsigned)(REG_BASE + IDX_POWER),   regs[IDX_POWER]);

            /* 物理值：必须用浮点除法，避免整数除法丢精度 */
            float voltage = regs[IDX_VOLTAGE] / 10.0f;
            float current = regs[IDX_CURRENT] / 100.0f;
            float power   = (float)regs[IDX_POWER];

            ESP_LOGI(TAG, "Value: Voltage=%.1fV, Current=%.2fA, Power=%.0fW",
                     voltage, current, power);

            /* 连续成功 3 次后，才启动上报 */
            if (!reporting && consecutive_ok >= REPORT_START_THRESHOLD) {
                reporting = true;
                ESP_LOGI(TAG, "%d consecutive reads OK, start UDP reporting",
                         REPORT_START_THRESHOLD);
            }

            /* 上报：Wi-Fi 未连接时跳过，不影响采集 */
            if (reporting) {
                if (wifi_is_connected()) {
                    udp_send_json(voltage, current, power);
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
        }

        vTaskDelay(pdMS_TO_TICKS(COLLECT_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "===== inverter collector start =====");

    /* 1. Wi-Fi：连接路由器并自动重连 */
    wifi_init_sta();

    /* 2. UDP：创建 socket，指向上报服务器 */
    udp_client_init();

    /* 3. Modbus：初始化采集串口 */
    modbus_init();

    /* 4. 采集任务，栈 4096（题目建议值） */
    xTaskCreate(collector_task, "collector", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "collector task created, period=%d ms", COLLECT_PERIOD_MS);
}
