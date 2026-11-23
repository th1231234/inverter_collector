#include "modbus.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "MODBUS";

/* 最近一次通信收到的从机异常码，0 表示无异常 */
static uint8_t s_last_exception = 0;

/* ------------------------------------------------------------------
 * 总线互斥锁
 *
 * 阶段二起，采集任务（每 5 秒读一次）和命令任务（收到 read/write 就执行）
 * 会同时访问同一个 UART。两帧交错发出去，双方都会因为地址/功能码/CRC
 * 对不上而失败，表现为随机的 read failed。
 *
 * 锁的粒度是「一次完整事务」：清缓冲 → 发帧 → 收帧 → 校验 → 重试。
 * 只锁 UART 读写是不够的，中间被插入一次别的请求同样会污染接收缓冲。
 * ------------------------------------------------------------------ */
static SemaphoreHandle_t s_bus_lock = NULL;

uint8_t modbus_last_exception(void)
{
    return s_last_exception;
}

/* ------------------------------------------------------------------
 * 十六进制帧日志：每行最多 16 字节，避免日志过长被截断
 * ------------------------------------------------------------------ */
static void modbus_log_hex(const char *dir, const uint8_t *buf, int len)
{
    char line[16 * 3 + 1];

    for (int off = 0; off < len; off += 16) {
        int n = (len - off > 16) ? 16 : (len - off);
        int pos = 0;

        for (int i = 0; i < n; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", buf[off + i]);
        }

        if (off == 0) {
            ESP_LOGI(TAG, "%s[%d]: %s", dir, len, line);
        } else {
            ESP_LOGI(TAG, "%s[+%d]: %s", dir, off, line);
        }
    }
}

/* ==================================================================
 * 一、硬件配置（按目标芯片自动切换）
 * ==================================================================
 * ESP32C3：只有 UART0/UART1，没有 UART_NUM_2，因此用 UART1
 * ESP32  ：用 UART2（UART1 与 Flash 引脚复用，不推荐）
 */
#if CONFIG_IDF_TARGET_ESP32C3
  #define MODBUS_UART_PORT   UART_NUM_1
  #define MODBUS_TX_PIN      5        /* 板卡 SUB_DEVICE UART TX */
  #define MODBUS_RX_PIN      6        /* 板卡 SUB_DEVICE UART RX */
  #define MODBUS_DE_PIN      (-1)     /* 板卡未定义 DE 脚，485 为自动收发方向 */
#else
  #define MODBUS_UART_PORT   UART_NUM_2
  #define MODBUS_TX_PIN      23       /* 板卡 SUB_DEVICE UART TX */
  #define MODBUS_RX_PIN      39       /* 板卡 SUB_DEVICE UART RX（仅输入脚） */
  #define MODBUS_DE_PIN      (-1)     /* 无方向控制脚（自动收发模块） */
#endif

/* ==================================================================
 * 二、串口与协议参数
 * ================================================================== */
#define MODBUS_BAUDRATE        9600
#define MODBUS_RX_BUF_SIZE     2304     /* 与板卡 CONFIG_SUB_DEVICE_UART_RX_BUFFER_SIZE 一致 */
#define MODBUS_TX_BUF_SIZE     2304     /* 与板卡 CONFIG_SUB_DEVICE_UART_TX_BUFFER_SIZE 一致 */

#define MODBUS_DEFAULT_SLAVE   1        /* 默认从机地址 */

#define MODBUS_TIMEOUT_MS      200      /* 单次等待超时 200ms */
#define MODBUS_RETRY_COUNT     2        /* 失败后重试 2 次 */
#define MODBUS_RETRY_GAP_MS    20       /* 两次尝试之间的间隔，给总线留恢复时间 */

/* 各功能码的单次数量上限，取自 Modbus 应用协议规范 */
#define MODBUS_MAX_BITS_READ   2000     /* 01 / 02 */
#define MODBUS_MAX_REGS_READ   125      /* 03 / 04 */
#define MODBUS_MAX_REGS_WRITE  123      /* 16 */

/* 收帧缓冲区：最大响应 5 + 125*2 = 255 字节，取 256 足够 */
#define MODBUS_RX_FRAME_MAX    256

/* 逐条打印的上限，避免一次读 2000 位时刷屏 */
#define MODBUS_LOG_ITEM_LIMIT  32

/* ==================================================================
 * 三、CRC16-Modbus：多项式 0x8005（反转 0xA001），初值 0xFFFF
 * ================================================================== */
uint16_t modbus_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];

        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }

    return crc;
}

/* ==================================================================
 * 四、初始化串口
 * ================================================================== */
bool modbus_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = MODBUS_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,   /* N：无校验 */
        .stop_bits  = UART_STOP_BITS_1,      /* 1：1 位停止位 */
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (s_bus_lock == NULL) {
        s_bus_lock = xSemaphoreCreateMutex();
        if (s_bus_lock == NULL) {
            ESP_LOGE(TAG, "create bus mutex failed");
            return false;
        }
    }

    ESP_ERROR_CHECK(uart_driver_install(MODBUS_UART_PORT,
                                       MODBUS_RX_BUF_SIZE,
                                       MODBUS_TX_BUF_SIZE,
                                       0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_PORT, &cfg));

#if MODBUS_DE_PIN >= 0
    /* 有方向控制脚：RTS 复用为 DE，开启半双工 RS485 自动收发 */
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_PORT,
                                 MODBUS_TX_PIN, MODBUS_RX_PIN,
                                 MODBUS_DE_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(MODBUS_UART_PORT, UART_MODE_RS485_HALF_DUPLEX));
    ESP_LOGI(TAG, "UART%d init ok, TX=%d RX=%d DE=%d, 9600-8-N-1 (RS485 half-duplex)",
             MODBUS_UART_PORT, MODBUS_TX_PIN, MODBUS_RX_PIN, MODBUS_DE_PIN);
#else
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_PORT,
                                 MODBUS_TX_PIN, MODBUS_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d init ok, TX=%d RX=%d, 9600-8-N-1",
             MODBUS_UART_PORT, MODBUS_TX_PIN, MODBUS_RX_PIN);
#endif

    return true;
}

/* ==================================================================
 * 五、异常码转文字
 * ================================================================== */
static const char *modbus_exception_str(uint8_t code)
{
    switch (code) {
    case 0x01: return "illegal function";
    case 0x02: return "illegal data address";
    case 0x03: return "illegal data value";
    case 0x04: return "slave device failure";
    default:   return "unknown";
    }
}

/* ==================================================================
 * 六、统一传输层：发送 → 等待 → 校验 → 失败重试
 * ------------------------------------------------------------------
 * 这一层与功能码无关，所有功能码都复用它：
 *   1. 清空接收缓冲
 *   2. 打印并发送请求帧
 *   3. 等待响应，超时则以固定间隔重试
 *   4. 识别异常帧（功能码最高位置 1，长度 5）
 *   5. 校验从机地址、功能码、CRC
 * ------------------------------------------------------------------
 * 返回 true 时，*rx_len 为实际收到的字节数（已通过 CRC 校验）。
 * ================================================================== */
/* ------------------------------------------------------------------
 * 总线互斥：调用方必须已持有 s_bus_lock
 * ------------------------------------------------------------------ */
static bool modbus_transact_locked(uint8_t slave, uint8_t func,
                                   const uint8_t *tx, int tx_len,
                                   uint8_t *rx, int rx_cap, int rx_expect, int *rx_len)
{
    s_last_exception = 0;

    for (int attempt = 0; attempt <= MODBUS_RETRY_COUNT; attempt++) {

        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(MODBUS_RETRY_GAP_MS));
        }

        uart_flush_input(MODBUS_UART_PORT);

        modbus_log_hex("TX", tx, tx_len);
        uart_write_bytes(MODBUS_UART_PORT, (const char *)tx, tx_len);

        int len = uart_read_bytes(MODBUS_UART_PORT, rx, rx_cap,
                                  pdMS_TO_TICKS(MODBUS_TIMEOUT_MS));
        if (len <= 0) {
            ESP_LOGW(TAG, "func 0x%02X timeout (attempt %d/%d)",
                     (unsigned)func, attempt + 1, MODBUS_RETRY_COUNT + 1);
            continue;
        }

        modbus_log_hex("RX", rx, len);

        /* ---- 异常应答：功能码最高位置 1，长度固定 5 ---- */
        if (len >= 5 && (rx[1] & 0x80)) {
            uint8_t code = rx[2];
            s_last_exception = code;
            ESP_LOGE(TAG, "slave exception: addr=%u func=0x%02X code=0x%02X (%s)",
                     (unsigned)rx[0], (unsigned)(rx[1] & 0x7F),
                     (unsigned)code, modbus_exception_str(code));
            /* 从机明确拒绝，重试无意义 */
            return false;
        }

        /* ---- 长度不足，多半是帧被截断 ---- */
        if (len < rx_expect) {
            ESP_LOGW(TAG, "func 0x%02X short frame: got %d expect %d (attempt %d)",
                     (unsigned)func, len, rx_expect, attempt + 1);
            continue;
        }

        /* ---- 校验从机地址与功能码 ---- */
        if (rx[0] != slave || rx[1] != func) {
            ESP_LOGW(TAG, "func 0x%02X bad header: addr=0x%02X func=0x%02X (attempt %d)",
                     (unsigned)func, (unsigned)rx[0], (unsigned)rx[1], attempt + 1);
            continue;
        }

        /* ---- 校验 CRC（应答 CRC 同样是低字节在前） ---- */
        uint16_t rx_crc = (uint16_t)rx[len - 2] | ((uint16_t)rx[len - 1] << 8);
        uint16_t calc   = modbus_crc16(rx, (uint16_t)(len - 2));
        if (calc != rx_crc) {
            ESP_LOGW(TAG, "func 0x%02X CRC mismatch: calc=0x%04X recv=0x%04X (attempt %d)",
                     (unsigned)func, calc, rx_crc, attempt + 1);
            continue;
        }

        *rx_len = len;
        return true;
    }

    ESP_LOGE(TAG, "func 0x%02X addr=%u failed after %d attempts",
             (unsigned)func, (unsigned)slave, MODBUS_RETRY_COUNT + 1);
    return false;
}

/* ------------------------------------------------------------------
 * 对外事务接口：加锁 → 执行 → 解锁
 *
 * 用互斥量（不是二值信号量）是为了支持同一任务重入时的优先级继承：
 * 采集任务优先级 5、命令任务优先级 5，谁先拿到谁执行，另一个阻塞等待，
 * 不会出现「忙等重试」把总线塞满的情况。
 * ------------------------------------------------------------------ */
static bool modbus_transact(uint8_t slave, uint8_t func,
                            const uint8_t *tx, int tx_len,
                            uint8_t *rx, int rx_cap, int rx_expect, int *rx_len)
{
    if (s_bus_lock == NULL) {
        /* 还没初始化就去调用属于编程错误，退化为无锁执行并留日志 */
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

/* ==================================================================
 * 七、请求帧构造
 * ================================================================== */

/* 01 / 02 / 03 / 04 共用：地址 + 功能码 + 起始地址 + 数量 + CRC，固定 8 字节 */
static int modbus_build_read_request(uint8_t slave, uint8_t func,
                                     uint16_t addr, uint16_t num, uint8_t *tx)
{
    tx[0] = slave;
    tx[1] = func;
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)(addr & 0xFF);
    tx[4] = (uint8_t)(num >> 8);
    tx[5] = (uint8_t)(num & 0xFF);

    uint16_t crc = modbus_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);      /* CRC 低字节在前 */
    tx[7] = (uint8_t)(crc >> 8);
    return 8;
}

/* ==================================================================
 * 八、读位数据（01 / 02 共用）
 * ================================================================== */
static bool modbus_read_bits(uint8_t slave, uint8_t func,
                             uint16_t addr, uint16_t num, uint8_t *bits)
{
    uint8_t tx[8];
    uint8_t rx[MODBUS_RX_FRAME_MAX];

    if (bits == NULL || num == 0 || num > MODBUS_MAX_BITS_READ) {
        ESP_LOGE(TAG, "invalid param: func=0x%02X num=%u",
                 (unsigned)func, (unsigned)num);
        return false;
    }

    int tx_len     = modbus_build_read_request(slave, func, addr, num, tx);
    int byte_count = (num + 7) / 8;
    int rx_expect  = 5 + byte_count;
    int rx_len     = 0;

    if (!modbus_transact(slave, func, tx, tx_len, rx, sizeof(rx), rx_expect, &rx_len)) {
        return false;
    }

    if (rx[2] != byte_count) {
        ESP_LOGW(TAG, "func 0x%02X byte_count=%u expect %d",
                 (unsigned)func, (unsigned)rx[2], byte_count);
        return false;
    }

    memcpy(bits, &rx[3], (size_t)byte_count);

    ESP_LOGI(TAG, "func 0x%02X ok: addr=%u num=%u crc=0x%04X",
             (unsigned)func, (unsigned)addr, (unsigned)num,
             (unsigned)((uint16_t)rx[rx_len - 2] | ((uint16_t)rx[rx_len - 1] << 8)));

    uint16_t show = (num > MODBUS_LOG_ITEM_LIMIT) ? MODBUS_LOG_ITEM_LIMIT : num;
    for (uint16_t i = 0; i < show; i++) {
        ESP_LOGI(TAG, "  bit[%u] = %d", (unsigned)(addr + i),
                 modbus_bit_get(bits, i) ? 1 : 0);
    }
    if (num > show) {
        ESP_LOGI(TAG, "  ... (%u bits, only first %d shown)",
                 (unsigned)num, MODBUS_LOG_ITEM_LIMIT);
    }

    return true;
}

/* ==================================================================
 * 九、读字数据（03 / 04 共用）
 * ================================================================== */
static bool modbus_read_regs(uint8_t slave, uint8_t func,
                             uint16_t addr, uint16_t num, uint16_t *regs)
{
    uint8_t tx[8];
    uint8_t rx[MODBUS_RX_FRAME_MAX];

    if (regs == NULL || num == 0 || num > MODBUS_MAX_REGS_READ) {
        ESP_LOGE(TAG, "invalid param: func=0x%02X num=%u",
                 (unsigned)func, (unsigned)num);
        return false;
    }

    int tx_len    = modbus_build_read_request(slave, func, addr, num, tx);
    int rx_expect = 5 + num * 2;
    int rx_len    = 0;

    if (!modbus_transact(slave, func, tx, tx_len, rx, sizeof(rx), rx_expect, &rx_len)) {
        return false;
    }

    if (rx[2] != num * 2) {
        ESP_LOGW(TAG, "func 0x%02X byte_count=%u expect %u",
                 (unsigned)func, (unsigned)rx[2], (unsigned)(num * 2));
        return false;
    }

    ESP_LOGI(TAG, "func 0x%02X ok: addr=%u num=%u crc=0x%04X",
             (unsigned)func, (unsigned)addr, (unsigned)num,
             (unsigned)((uint16_t)rx[rx_len - 2] | ((uint16_t)rx[rx_len - 1] << 8)));

    for (uint16_t i = 0; i < num; i++) {
        /* 大端序：高字节在前 */
        regs[i] = (uint16_t)(((uint16_t)rx[3 + i * 2] << 8) | rx[4 + i * 2]);
        ESP_LOGI(TAG, "  reg[0x%04X] = 0x%04X (%u)",
                 (unsigned)(addr + i), (unsigned)regs[i], (unsigned)regs[i]);
    }

    return true;
}

/* ==================================================================
 * 十、对外接口
 * ================================================================== */

bool modbus_read_coils(uint8_t slave, uint16_t addr, uint16_t num, uint8_t *bits)
{
    return modbus_read_bits(slave, MODBUS_FUNC_READ_COILS, addr, num, bits);
}

bool modbus_read_discrete_inputs(uint8_t slave, uint16_t addr, uint16_t num, uint8_t *bits)
{
    return modbus_read_bits(slave, MODBUS_FUNC_READ_DISCRETE_INPUTS, addr, num, bits);
}

bool modbus_read_holding(uint8_t slave, uint16_t addr, uint16_t num, uint16_t *regs)
{
    return modbus_read_regs(slave, MODBUS_FUNC_READ_HOLDING, addr, num, regs);
}

bool modbus_read_input(uint8_t slave, uint16_t addr, uint16_t num, uint16_t *regs)
{
    return modbus_read_regs(slave, MODBUS_FUNC_READ_INPUT, addr, num, regs);
}

/* ------------------------------------------------------------------
 * 06 写单个寄存器
 * 请求：地址 + 0x06 + 寄存器地址 + 值 + CRC（8 字节）
 * 应答：请求帧原样回显（8 字节）
 * ------------------------------------------------------------------ */
bool modbus_write_register(uint8_t slave, uint16_t addr, uint16_t value)
{
    uint8_t tx[8];
    uint8_t rx[MODBUS_RX_FRAME_MAX];

    tx[0] = slave;
    tx[1] = MODBUS_FUNC_WRITE_REGISTER;
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)(addr & 0xFF);
    tx[4] = (uint8_t)(value >> 8);
    tx[5] = (uint8_t)(value & 0xFF);

    uint16_t crc = modbus_crc16(tx, 6);
    tx[6] = (uint8_t)(crc & 0xFF);
    tx[7] = (uint8_t)(crc >> 8);

    int rx_len = 0;
    if (!modbus_transact(slave, MODBUS_FUNC_WRITE_REGISTER,
                         tx, sizeof(tx), rx, sizeof(rx), 8, &rx_len)) {
        return false;
    }

    /* 06 的应答是请求帧回显，前 6 字节必须一模一样 */
    if (memcmp(tx, rx, 6) != 0) {
        ESP_LOGW(TAG, "func 0x06 echo mismatch: tx=%02X %02X %02X %02X %02X %02X, "
                      "rx=%02X %02X %02X %02X %02X %02X",
                 (unsigned)tx[0], (unsigned)tx[1], (unsigned)tx[2],
                 (unsigned)tx[3], (unsigned)tx[4], (unsigned)tx[5],
                 (unsigned)rx[0], (unsigned)rx[1], (unsigned)rx[2],
                 (unsigned)rx[3], (unsigned)rx[4], (unsigned)rx[5]);
        return false;
    }

    ESP_LOGI(TAG, "func 0x06 ok: write reg[0x%04X] = 0x%04X (%u)",
             (unsigned)addr, (unsigned)value, (unsigned)value);
    return true;
}

/* ------------------------------------------------------------------
 * 16 写多个寄存器
 * 请求：地址 + 0x10 + 起始地址 + 数量 + 字节数 + 数据 + CRC
 * 应答：地址 + 0x10 + 起始地址 + 数量 + CRC（8 字节）
 * ------------------------------------------------------------------ */
bool modbus_write_registers(uint8_t slave, uint16_t addr, uint16_t num, const uint16_t *regs)
{
    uint8_t tx[9 + MODBUS_MAX_REGS_WRITE * 2];
    uint8_t rx[MODBUS_RX_FRAME_MAX];

    if (regs == NULL || num == 0 || num > MODBUS_MAX_REGS_WRITE) {
        ESP_LOGE(TAG, "invalid param: func=0x10 num=%u", (unsigned)num);
        return false;
    }

    int idx = 0;
    tx[idx++] = slave;
    tx[idx++] = MODBUS_FUNC_WRITE_REGISTERS;
    tx[idx++] = (uint8_t)(addr >> 8);
    tx[idx++] = (uint8_t)(addr & 0xFF);
    tx[idx++] = (uint8_t)(num >> 8);
    tx[idx++] = (uint8_t)(num & 0xFF);
    tx[idx++] = (uint8_t)(num * 2);          /* 字节数 = 数量 × 2 */

    for (uint16_t i = 0; i < num; i++) {
        tx[idx++] = (uint8_t)(regs[i] >> 8);
        tx[idx++] = (uint8_t)(regs[i] & 0xFF);
    }

    uint16_t crc = modbus_crc16(tx, (uint16_t)idx);
    tx[idx++] = (uint8_t)(crc & 0xFF);
    tx[idx++] = (uint8_t)(crc >> 8);

    int rx_len = 0;
    if (!modbus_transact(slave, MODBUS_FUNC_WRITE_REGISTERS,
                         tx, idx, rx, sizeof(rx), 8, &rx_len)) {
        return false;
    }

    /* 应答里的起始地址与数量应回显一致 */
    uint16_t echo_addr = (uint16_t)(((uint16_t)rx[2] << 8) | rx[3]);
    uint16_t echo_num  = (uint16_t)(((uint16_t)rx[4] << 8) | rx[5]);
    if (echo_addr != addr || echo_num != num) {
        ESP_LOGW(TAG, "func 0x10 echo mismatch: addr=0x%04X num=%u, expect addr=0x%04X num=%u",
                 (unsigned)echo_addr, (unsigned)echo_num,
                 (unsigned)addr, (unsigned)num);
        return false;
    }

    ESP_LOGI(TAG, "func 0x10 ok: write reg[0x%04X..0x%04X] %u 个",
             (unsigned)addr, (unsigned)(addr + num - 1), (unsigned)num);
    return true;
}

/* ==================================================================
 * 十一、兼容旧接口
 * ================================================================== */
bool modbus_read_registers(uint16_t reg_addr, uint16_t reg_num, uint16_t *data)
{
    return modbus_read_holding(MODBUS_DEFAULT_SLAVE, reg_addr, reg_num, data);
}
