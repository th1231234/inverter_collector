#include "modbus_selftest.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "modbus.h"

static const char *TAG = "SELFTEST";

#define SELFTEST_SLAVE              1

/* 每次切换给的操作时间上限（秒）；到点无论如何都测一次 */
#define SELFTEST_SWITCH_WAIT_SEC    15

/* 等待期间每 2 秒试发一次请求：
 * 一旦从机不再回「非法功能码」，说明已经切好，立刻开测，不用干等满 15 秒 */
#define SELFTEST_PROBE_INTERVAL_SEC 2

/* 同一项最多给几轮机会（每轮 = 一次等待 + 一次测试）。
 * 改成 2 轮：连续两次仍不认功能码，说明用户根本没切，
 * 再给第三轮只是白耗 15 秒，不如直接判 FAIL 进下一个阶段。 */
#define SELFTEST_SWITCH_ROUNDS      2

#define MAX_ITEMS                   8

/* Modbus 标准异常码 */
#define EXC_ILLEGAL_FUNCTION        0x01    /* 从机不实现该功能码 */
#define EXC_ILLEGAL_DATA_ADDRESS    0x02    /* 地址越界 */

/* 写测试地址：要落在 Address=3000 且 Quantity>=16 的窗口内（0x3000~0x300F），
 * 同时避开 0x3000~0x3007 的采集数据区 */
#define SELFTEST_WRITE_ADDR_SINGLE  0x3008
#define SELFTEST_WRITE_ADDR_MULTI   0x3009

typedef struct {
    const char *name;
    bool        pass;
} selftest_item_t;

static selftest_item_t s_items[MAX_ITEMS];
static int s_count = 0;

static void record(const char *name, bool pass)
{
    if (s_count >= MAX_ITEMS) {
        return;
    }
    s_items[s_count].name = name;
    s_items[s_count].pass = pass;
    s_count++;

    ESP_LOGI(TAG, "[%s] %s", pass ? "PASS" : "FAIL", name);
}

/* 上一次失败是不是「从机不认这个功能码」。
 * 只有这种失败才值得重试——说明 Function 下拉框还没切过去。 */
static bool last_fail_is_func_error(void)
{
    return modbus_last_exception() == EXC_ILLEGAL_FUNCTION;
}

/* 提示用户切换 Function，round 表示第几轮。
 *
 * 第 5 个参数 probe：等待期间用真实请求去探，切好了就提前返回。
 * 传 NULL 表示只做固定倒计时（阶段 1 用不到）。 */
static void wait_for_switch(const char *func_item, int round, bool (*probe)(void))
{
    ESP_LOGE(TAG, "");
    ESP_LOGE(TAG, ">>> 第 %d 轮等待：请把 Modbus Slave 改成 [%s]", round, func_item);
    ESP_LOGE(TAG, "    只需要改 Function = %s", func_item);
    ESP_LOGE(TAG, "    Address 保持 3000 就行（地址 0 不通会自动重试 0x3000）");
    ESP_LOGE(TAG, "    Quantity 保持 16，否则写测试会报地址越界");
    ESP_LOGE(TAG, "    如果窗口显示 Disconnected，记得重新点 Connect");
    ESP_LOGE(TAG, "    切好就行，每 %d 秒自动探测一次，无需干等",
             SELFTEST_PROBE_INTERVAL_SEC);
    ESP_LOGE(TAG, "");

    int waited = 0;

    while (waited < SELFTEST_SWITCH_WAIT_SEC) {
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_PROBE_INTERVAL_SEC * 1000));
        waited += SELFTEST_PROBE_INTERVAL_SEC;

        if (probe != NULL) {
            /* 探通了：从机已经切好 */
            if (probe()) {
                ESP_LOGW(TAG, "    第 %d 秒探测成功，提前开始测试 <<<", waited);
                return;
            }
            /* 不再是「非法功能码」= Function 已切对（可能只是地址还不对），
             * 也别再等了，直接把问题报到日志里 */
            if (!last_fail_is_func_error()) {
                ESP_LOGW(TAG, "    第 %d 秒功能码已切好，提前开始测试 <<<", waited);
                return;
            }
        }

        ESP_LOGW(TAG, "    倒计时 %d 秒 ...", SELFTEST_SWITCH_WAIT_SEC - waited);
    }
    ESP_LOGW(TAG, "    >>> 开始测试 <<<");
    ESP_LOGW(TAG, "");
}

/* probe_read_holding 定义在下面，这里先声明，ensure_func_03 要用 */
static bool probe_read_holding(void);

/* ------------------------------------------------------------------
 * 阶段 1 的前置校验：确认 Function 停在 03
 * 03 不通的话，后面的 06/16 一定也通不了，白跑一遍
 * ------------------------------------------------------------------ */
static void ensure_func_03(void)
{
    for (int round = 1; round <= SELFTEST_SWITCH_ROUNDS; round++) {
        uint16_t regs[5] = {0};

        if (modbus_read_holding(SELFTEST_SLAVE, 0x3000, 5, regs)) {
            return;
        }
        if (!last_fail_is_func_error()) {
            return;     /* 不是功能码问题，交给正式测试去判定 */
        }
        if (round >= SELFTEST_SWITCH_ROUNDS) {
            return;
        }

        ESP_LOGE(TAG, "从机不认功能码 03，Function 现在多半不是 03 Holding Registers");
        wait_for_switch("03 Holding Registers", round + 1, probe_read_holding);
    }
}

/* ------------------------------------------------------------------
 * 各项探测：只返回结果，不负责记录
 * ------------------------------------------------------------------ */

static bool probe_read_holding(void)
{
    uint16_t regs[5] = {0};
    return modbus_read_holding(SELFTEST_SLAVE, 0x3000, 5, regs);
}

static bool probe_write_single(void)
{
    const uint16_t want = 4000;
    uint16_t back = 0;

    if (!modbus_write_register(SELFTEST_SLAVE, SELFTEST_WRITE_ADDR_SINGLE, want)) {
        if (modbus_last_exception() == EXC_ILLEGAL_DATA_ADDRESS) {
            ESP_LOGW(TAG, "异常码 0x02：0x3008 超出从机显示范围，请把 Quantity 改到 16 以上");
        }
        return false;
    }
    if (!modbus_read_holding(SELFTEST_SLAVE, SELFTEST_WRITE_ADDR_SINGLE, 1, &back)) {
        return false;
    }
    if (back != want) {
        ESP_LOGE(TAG, "读回值不符：期望 %u，实际 %u",
                 (unsigned)want, (unsigned)back);
        return false;
    }
    return true;
}

static bool probe_write_multi(void)
{
    const uint16_t want[3] = {1111, 2222, 3333};
    uint16_t back[3] = {0};

    if (!modbus_write_registers(SELFTEST_SLAVE, SELFTEST_WRITE_ADDR_MULTI, 3, want)) {
        if (modbus_last_exception() == EXC_ILLEGAL_DATA_ADDRESS) {
            ESP_LOGW(TAG, "异常码 0x02：0x3009~0x300B 超出从机显示范围，请把 Quantity 改到 16 以上");
        }
        return false;
    }
    if (!modbus_read_holding(SELFTEST_SLAVE, SELFTEST_WRITE_ADDR_MULTI, 3, back)) {
        return false;
    }
    if (back[0] != want[0] || back[1] != want[1] || back[2] != want[2]) {
        ESP_LOGE(TAG, "读回值不符：期望 %u/%u/%u，实际 %u/%u/%u",
                 (unsigned)want[0], (unsigned)want[1], (unsigned)want[2],
                 (unsigned)back[0], (unsigned)back[1], (unsigned)back[2]);
        return false;
    }
    return true;
}

static bool probe_read_input(void)
{
    uint16_t regs[3] = {0};

    if (modbus_read_input(SELFTEST_SLAVE, 0x0000, 3, regs)) {
        return true;
    }
    /* 地址 0 被拒（0x02）时，退一步用采集区的基址再试一次。
     * 理由：Modbus Slave 里 Address 往往一直停在 3000，
     * 为了自检专门去改成 0 会把整张表清零，代价太大。
     * 标准要求地址 0，但这里两种都能接受，日志会显示实际用了哪个。 */
    if (modbus_last_exception() == EXC_ILLEGAL_DATA_ADDRESS) {
        if (modbus_read_input(SELFTEST_SLAVE, 0x3000, 3, regs)) {
            ESP_LOGW(TAG, "地址 0 不可用，改用 0x3000 读取成功（等价验证）");
            return true;
        }
    }
    return false;
}

static bool probe_read_discrete(void)
{
    uint8_t bits[1] = {0};

    if (modbus_read_discrete_inputs(SELFTEST_SLAVE, 0x0000, 8, bits)) {
        return true;
    }
    if (modbus_last_exception() == EXC_ILLEGAL_DATA_ADDRESS) {
        if (modbus_read_discrete_inputs(SELFTEST_SLAVE, 0x3000, 8, bits)) {
            ESP_LOGW(TAG, "地址 0 不可用，改用 0x3000 读取成功（等价验证）");
            return true;
        }
    }
    return false;
}

static bool probe_read_coils(void)
{
    uint8_t bits[1] = {0};

    if (modbus_read_coils(SELFTEST_SLAVE, 0x0000, 8, bits)) {
        return true;
    }
    if (modbus_last_exception() == EXC_ILLEGAL_DATA_ADDRESS) {
        if (modbus_read_coils(SELFTEST_SLAVE, 0x3000, 8, bits)) {
            ESP_LOGW(TAG, "地址 0 不可用，改用 0x3000 读取成功（等价验证）");
            return true;
        }
    }
    return false;
}

/* 异常应答：故意读一个远超从机范围的地址，
 * 正确行为是返回 0x83 + 异常码 0x02（非法数据地址）。
 * 如果拿到的是 0x01（非法功能），说明 Function 根本没停在 03，这项不能算过。 */
static bool probe_exception(void)
{
    uint16_t regs[5] = {0};

    bool ok = modbus_read_holding(SELFTEST_SLAVE, 0xFFFF, 5, regs);
    uint8_t code = modbus_last_exception();

    if (ok) {
        ESP_LOGW(TAG, "从机返回了正常数据，说明 0xFFFF 仍在它支持的地址范围内");
        return false;
    }
    if (code == 0) {
        ESP_LOGW(TAG, "没收到异常帧（可能是超时），检查从机是否在线");
        return false;
    }
    if (code == EXC_ILLEGAL_FUNCTION) {
        ESP_LOGW(TAG, "收到 0x01 而不是 0x02：从机不认 03，Function 没停在 03");
        return false;
    }

    ESP_LOGI(TAG, "收到异常应答，异常码 = 0x%02X（期望 0x02）", (unsigned)code);
    return code == EXC_ILLEGAL_DATA_ADDRESS;
}

/* ------------------------------------------------------------------
 * 单项阶段：失败时先看异常码，再决定要不要多给一轮
 *   0x01 illegal function     → Function 没切，值得再等一轮
 *   0x02 illegal data address → Function 已经对了，是 Address 没改成 0
 * ------------------------------------------------------------------ */
static void stage_single(const char *name, const char *func_item, bool (*fn)(void))
{
    bool ok = false;

    for (int round = 1; round <= SELFTEST_SWITCH_ROUNDS; round++) {
        ok = fn();
        if (ok) {
            break;
        }

        uint8_t code = modbus_last_exception();

        /* 既不是功能码问题、也不是地址问题（超时 / CRC 错），重试没意义 */
        if (code != EXC_ILLEGAL_FUNCTION && code != EXC_ILLEGAL_DATA_ADDRESS) {
            ESP_LOGE(TAG, "异常码 0x%02X，不是切换问题，跳过本轮重试", (unsigned)code);
            break;
        }

        if (code == EXC_ILLEGAL_DATA_ADDRESS) {
            ESP_LOGE(TAG, "从机报「地址越界」(0x02)：Function 已经切对了，");
            ESP_LOGE(TAG, "但地址 0 和 0x3000 都被拒 —— 把 Quantity 改到 16 以上");
        } else {
            ESP_LOGE(TAG, "从机不认这个功能码 (0x01)：Function 还没切过去");
        }

        if (round < SELFTEST_SWITCH_ROUNDS) {
            wait_for_switch(func_item, round + 1, fn);
        } else {
            ESP_LOGE(TAG, "连续 %d 轮都没切对，跳过本项，进入下一阶段",
                     SELFTEST_SWITCH_ROUNDS);
        }
    }

    record(name, ok);
}

/* ------------------------------------------------------------------
 * 阶段 1：03 / 06 / 16 / 异常，全部作用在 Holding Registers 上
 * ------------------------------------------------------------------ */
static void stage_holding(void)
{
    ensure_func_03();

    record("03 read holding 0x3000 x5",        probe_read_holding());
    record("06 write + readback 0x3008",       probe_write_single());
    record("16 write + readback 0x3009 x3",    probe_write_multi());
    record("exception on illegal address 0xFFFF", probe_exception());
}

/* ------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------ */
void modbus_selftest_run(void)
{
    s_count = 0;

    ESP_LOGI(TAG, "=============== Modbus 功能码自检开始 ===============");
    ESP_LOGI(TAG, "开始前请把 Modbus Slave 摆成：");
    ESP_LOGI(TAG, "  Slave ID = %d", SELFTEST_SLAVE);
    ESP_LOGI(TAG, "  Function = 03 Holding Registers");
    ESP_LOGI(TAG, "  Address  = 3000，Quantity = 16，地址显示选 Hex");
    ESP_LOGI(TAG, "共 7 项，分 4 个阶段；后 3 个阶段只需要切 Function");
    ESP_LOGI(TAG, "地址 0 和 0x3000 会自动都试一遍，不用改 Address");
    ESP_LOGI(TAG, "每项最多给 %d 轮机会；切好后会自动探测，不用干等",
             SELFTEST_SWITCH_ROUNDS);
    ESP_LOGI(TAG, "====================================================");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "--- 阶段 1/4：Function = 03 Holding Registers ---");
    stage_holding();

    ESP_LOGI(TAG, "--- 阶段 2/4：准备测试 04 读输入寄存器 ---");
    wait_for_switch("04 Input Registers", 1, probe_read_input);
    stage_single("04 read input 0x0000/0x3000 x3", "04 Input Registers", probe_read_input);

    ESP_LOGI(TAG, "--- 阶段 3/4：准备测试 02 读离散输入 ---");
    wait_for_switch("02 Input Status", 1, probe_read_discrete);
    stage_single("02 read discrete 0x0000/0x3000 x8", "02 Input Status", probe_read_discrete);

    ESP_LOGI(TAG, "--- 阶段 4/4：准备测试 01 读线圈 ---");
    wait_for_switch("01 Coils", 1, probe_read_coils);
    stage_single("01 read coils 0x0000/0x3000 x8", "01 Coils", probe_read_coils);

    int passed = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].pass) {
            passed++;
        }
    }

    ESP_LOGI(TAG, "----------------- 汇总 %d/%d 通过 -----------------", passed, s_count);
    for (int i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, "  %-4s %s", s_items[i].pass ? "PASS" : "FAIL", s_items[i].name);
    }
    ESP_LOGI(TAG, "====================================================");
    ESP_LOGI(TAG, "提醒：自检结束后把 Function 切回 03 Holding Registers，");
    ESP_LOGI(TAG, "      否则主循环的采集会一直报 illegal function");
}
