#include "modbus.h"
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "MODBUS";

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

#define MODBUS_SLAVE_ADDR      1        /* 从机地址（截图 ID = 1） */
#define MODBUS_FUNC_READ       0x03     /* 功能码 03：读保持寄存器 */

#define MODBUS_TIMEOUT_MS      200      /* 单次等待超时 200ms */
#define MODBUS_RETRY_COUNT     2        /* 失败后重试 2 次 */
#define MODBUS_RETRY_GAP_MS    20       /* 两次尝试之间的间隔，给总线留恢复时间 */

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
 * 五、读取保持寄存器（功能码 03）
 * ================================================================== */
bool modbus_read_registers(uint16_t reg_addr, uint16_t reg_num, uint16_t *data)
{
    uint8_t tx_buf[8]  = {0};
    uint8_t rx_buf[256] = {0};

    if (reg_num == 0 || reg_num > 125 || data == NULL) {
        ESP_LOGE(TAG, "invalid param: reg_num=%u", reg_num);
        return false;
    }

    /* ---- 1. 组装请求帧：地址 + 功能码 + 起始地址 + 数量 + CRC ---- */
    tx_buf[0] = MODBUS_SLAVE_ADDR;
    tx_buf[1] = MODBUS_FUNC_READ;
    tx_buf[2] = (uint8_t)(reg_addr >> 8);      /* 大端序：高字节在前 */
    tx_buf[3] = (uint8_t)(reg_addr & 0xFF);
    tx_buf[4] = (uint8_t)(reg_num >> 8);
    tx_buf[5] = (uint8_t)(reg_num & 0xFF);

    uint16_t crc = modbus_crc16(tx_buf, 6);
    tx_buf[6] = (uint8_t)(crc & 0xFF);         /* CRC 低字节在前 */
    tx_buf[7] = (uint8_t)(crc >> 8);

    /* 期望应答长度：地址1 + 功能码1 + 字节数1 + 数据2N + CRC2 */
    const int expect_len = 5 + reg_num * 2;

    /* ---- 2. 发送 + 接收，失败重试 ---- */
    for (int attempt = 0; attempt <= MODBUS_RETRY_COUNT; attempt++) {

        /* 非首次尝试：先等一小段时间，让总线与从机恢复，避免连续丢帧 */
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(MODBUS_RETRY_GAP_MS));
        }

        uart_flush_input(MODBUS_UART_PORT);     /* 清掉残留数据，避免错帧 */

        /* 每次尝试都重新打印 TX，日志与实际发送次数保持一致 */
        modbus_log_hex("TX", tx_buf, sizeof(tx_buf));

        uart_write_bytes(MODBUS_UART_PORT, (const char *)tx_buf, sizeof(tx_buf));

        int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, expect_len,
                                  pdMS_TO_TICKS(MODBUS_TIMEOUT_MS));

        if (len <= 0) {
            ESP_LOGW(TAG, "timeout (attempt %d/%d)", attempt + 1, MODBUS_RETRY_COUNT + 1);
            continue;
        }

        /* 打印收到的应答帧原始字节 */
        modbus_log_hex("RX", rx_buf, len);

        /* ---- 3. 从机异常应答：功能码最高位置 1，长度固定 5 ---- */
        if (len >= 5 && (rx_buf[1] & 0x80)) {
            ESP_LOGE(TAG, "slave exception, code=0x%02X (addr=%d, func=0x%02X)",
                     rx_buf[2], rx_buf[0], rx_buf[1] & 0x7F);
            return false;
        }

        /* ---- 4. 长度不足，多半是帧被截断 ---- */
        if (len < expect_len) {
            ESP_LOGW(TAG, "short frame: got %d, expect %d (attempt %d)",
                     len, expect_len, attempt + 1);
            continue;
        }

        /* ---- 5. 校验从机地址与功能码 ---- */
        if (rx_buf[0] != MODBUS_SLAVE_ADDR || rx_buf[1] != MODBUS_FUNC_READ) {
            ESP_LOGW(TAG, "bad header: addr=0x%02X func=0x%02X (attempt %d)",
                     rx_buf[0], rx_buf[1], attempt + 1);
            continue;
        }

        /* ---- 6. 校验 CRC（应答 CRC 同样是低字节在前） ---- */
        uint16_t rx_crc = (uint16_t)rx_buf[len - 2] | ((uint16_t)rx_buf[len - 1] << 8);
        if (modbus_crc16(rx_buf, (uint16_t)(len - 2)) != rx_crc) {
            ESP_LOGW(TAG, "CRC mismatch: calc=0x%04X recv=0x%04X (attempt %d)",
                     modbus_crc16(rx_buf, (uint16_t)(len - 2)), rx_crc, attempt + 1);
            continue;
        }

        /* ---- 7. 打印帧结构，便于逐字段核对 ---- */
        ESP_LOGI(TAG, "resp: addr=%d func=0x%02X bytes=%u crc=0x%04X (ok)",
                 rx_buf[0], rx_buf[1], rx_buf[2], rx_crc);

        /* ---- 8. 解析数据：大端序转主机 uint16，并逐个打印 ---- */
        for (int i = 0; i < reg_num; i++) {
            data[i] = (uint16_t)((uint16_t)rx_buf[3 + i * 2] << 8) | rx_buf[4 + i * 2];
            ESP_LOGI(TAG, "  reg[0x%04X] = 0x%04X (%u)",
                     (unsigned)(reg_addr + i), data[i], data[i]);
        }

        return true;
    }

    ESP_LOGE(TAG, "read reg[0x%04X] x%u failed after %d attempts",
             reg_addr, reg_num, MODBUS_RETRY_COUNT + 1);
    return false;
}
