#include "sdkconfig.h"

/* 非 MQTT 通道时不编译本文件内容，避免引用未定义的 CONFIG_MQTT_* 宏 */
#if CONFIG_REPORT_PROTO_MQTT

#include "mqtt_cmd.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "modbus.h"
#include "mqtt_report.h"
#include "telemetry_codec.h"

static const char *TAG = "MQTT_CMD";

/* ==================================================================
 * 边界与容量：所有上限集中在此，代码里不出现魔法数字
 * ------------------------------------------------------------------
 * 报文长度上限直接用编解码层的 CODEC_MAX_PAYLOAD，
 * 两处各写一个 512 迟早会改漏一个。
 * ================================================================== */
#define CMD_MAX_PAYLOAD     CODEC_MAX_PAYLOAD
#define CMD_MAX_REQ_ID      CODEC_MAX_REQ_ID
#define CMD_MAX_READ_NUM    CODEC_MAX_READ_NUM
#define CMD_QUEUE_LEN       8     /* 待执行请求队列深度 */
#define CMD_TASK_STACK      4096  /* Modbus 调用 + protobuf 编码，4096 足够 */
#define CMD_TASK_PRIO       5

/* 模块内的错误码，直接填进 Resp.msg 字段 */
#define MSG_INVALID_PARAM   "invalid_param"
#define MSG_BUSY            "request_busy"
#define MSG_INTERNAL        "internal_error"

/* ------------------------------------------------------------------
 * 队列元素：解码后、还没执行的请求
 *
 * 不用 codec_read_req_t / codec_write_req_t 直接入队，因为队列必须
 * 装得下两种请求（同一个 xQueueReceive 出队），统一成一种结构更简单。
 * ------------------------------------------------------------------ */
typedef struct {
    bool     is_write;              /* true=write(06)，false=read(03) */
    uint8_t  slave;
    uint16_t addr;
    uint16_t num;                   /* 仅 read 有效 */
    uint16_t value;                 /* 仅 write 有效 */
    char     req_id[CMD_MAX_REQ_ID];
} cmd_req_t;

static QueueHandle_t s_queue = NULL;

/* ------------------------------------------------------------------
 * 组包并发布应答
 *
 * 二进制载荷不能用 strlen 量长度：Resp 里 values=[3500,500,1750]
 * 编码后含 0x00（3500=0x0DAC 变长编码为 AC 1B，但 500=0x01F4 → F4 03，
 * 而 raw=0 时会编出 00），交给 strlen 会截断成半条报文，
 * 对端 unpack 直接失败。所以统一走 len 参数。
 * ------------------------------------------------------------------ */
static bool publish_resp(uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return false;
    }

    bool ok = mqtt_report_publish(MQTT_TOPIC_RESP, (const char *)buf, (int)len);
    codec_free(buf);        /* 无论成败，缓冲都由这里释放 */

    if (!ok) {
        ESP_LOGE(TAG, "publish resp to \"%s\" failed", MQTT_TOPIC_RESP);
    }
    return ok;
}

/* Resp{req_id, ok=false, msg} */
static void send_resp_error(const char *req_id, const char *msg)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    if (!codec_encode_resp_error(req_id, msg, &buf, &len)) {
        ESP_LOGE(TAG, "encode error resp failed (req_id=%s)", req_id);
        return;
    }

    ESP_LOGW(TAG, "resp error: req_id=%s msg=%s", req_id, msg);
    publish_resp(buf, len);
}

/* Resp{req_id, ok=true} */
static void send_resp_ok(const char *req_id)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    if (!codec_encode_resp_ok(req_id, &buf, &len)) {
        ESP_LOGE(TAG, "encode ok resp failed (req_id=%s)", req_id);
        return;
    }

    ESP_LOGI(TAG, "resp ok: req_id=%s", req_id);
    publish_resp(buf, len);
}

/* Resp{req_id, ok=true, values=[...]} */
static void send_resp_ok_values(const char *req_id, const uint16_t *vals, int num)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    if (!codec_encode_resp_values(req_id, vals, num, &buf, &len)) {
        ESP_LOGE(TAG, "encode values resp failed (req_id=%s)", req_id);
        send_resp_error(req_id, MSG_INTERNAL);
        return;
    }

    ESP_LOGI(TAG, "resp ok: req_id=%s, %d value(s)", req_id, num);
    publish_resp(buf, len);
}

/* 把 Modbus 的失败原因翻译成对端能看懂的措辞 */
static const char *modbus_err_msg(void)
{
    uint8_t exc = modbus_last_exception();

    if (exc == 0) {
        return "modbus_timeout";        /* 从机无响应：没接、站号错、波特率错 */
    }
    switch (exc) {
    case 0x01:
        return "modbus_illegal_function";
    case 0x02:
        return "modbus_illegal_address";
    case 0x03:
        return "modbus_illegal_value";
    case 0x04:
        return "modbus_slave_failure";
    default:
        return "modbus_exception";
    }
}

/* ------------------------------------------------------------------
 * 执行任务：从队列取请求 → 真正读写 Modbus → 回应答
 * ------------------------------------------------------------------ */
static void cmd_task(void *arg)
{
    (void)arg;
    cmd_req_t req;

    while (1) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (req.is_write) {
            bool ok = modbus_write_register(req.slave, req.addr, req.value);
            if (ok) {
                ESP_LOGI(TAG, "write ok: req_id=%s slave=%u addr=0x%04X value=%u",
                         req.req_id, (unsigned)req.slave,
                         (unsigned)req.addr, (unsigned)req.value);
                send_resp_ok(req.req_id);
            } else {
                const char *msg = modbus_err_msg();
                ESP_LOGE(TAG, "write failed: req_id=%s addr=0x%04X msg=%s",
                         req.req_id, (unsigned)req.addr, msg);
                send_resp_error(req.req_id, msg);
            }
            continue;
        }

        /* read：req.num 已在解码时限制在 1~CMD_MAX_READ_NUM，定长缓冲安全 */
        uint16_t regs[CMD_MAX_READ_NUM] = {0};
        bool ok = modbus_read_holding(req.slave, req.addr, req.num, regs);
        if (ok) {
            ESP_LOGI(TAG, "read ok: req_id=%s slave=%u addr=0x%04X num=%u",
                     req.req_id, (unsigned)req.slave,
                     (unsigned)req.addr, (unsigned)req.num);
            send_resp_ok_values(req.req_id, regs, (int)req.num);
        } else {
            const char *msg = modbus_err_msg();
            ESP_LOGE(TAG, "read failed: req_id=%s addr=0x%04X msg=%s",
                     req.req_id, (unsigned)req.addr, msg);
            send_resp_error(req.req_id, msg);
        }
    }
}

/* ------------------------------------------------------------------
 * 入队
 * ------------------------------------------------------------------ */
static void enqueue(const cmd_req_t *req)
{
    /* 入队失败说明上一批请求还没处理完，明确回 busy 而不是静默丢弃 */
    if (s_queue == NULL || xQueueSend(s_queue, req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, req_id=%s rejected", req->req_id);
        send_resp_error(req->req_id, MSG_BUSY);
    }
}

/* ------------------------------------------------------------------
 * 解析 + 校验 + 入队
 * ------------------------------------------------------------------ */
static void handle_read(const uint8_t *data, size_t len)
{
    codec_read_req_t r = {0};
    codec_result_t rc = codec_decode_read(data, len, &r);

    if (rc != CODEC_OK) {
        /* 越界（比如 num=0 / num=99）能拿到 req_id，回一条错误应答；
         * 而 unpack 失败 / req_id 非法时取不到合法的 req_id，
         * 硬编一个假的会让客户端误配对，只能记日志后丢弃。
         * 对应 P-06「发截断报文，设备记录日志后继续工作」。 */
        if (rc == CODEC_ERR_RANGE) {
            ESP_LOGW(TAG, "read param out of range (req_id=%s), reply error", r.req_id);
            send_resp_error(r.req_id, MSG_INVALID_PARAM);
        } else {
            ESP_LOGE(TAG, "bad ReadRequest (%s), len=%u, dropped",
                     codec_result_name(rc), (unsigned)len);
        }
        return;
    }

    cmd_req_t req = {
        .is_write = false,
        .slave    = r.slave,
        .addr     = r.addr,
        .num      = r.num,
    };
    memcpy(req.req_id, r.req_id, sizeof(req.req_id));
    enqueue(&req);
}

static void handle_write(const uint8_t *data, size_t len)
{
    codec_write_req_t w = {0};
    codec_result_t rc = codec_decode_write(data, len, &w);

    if (rc != CODEC_OK) {
        if (rc == CODEC_ERR_RANGE) {
            ESP_LOGW(TAG, "write param out of range (req_id=%s), reply error", w.req_id);
            send_resp_error(w.req_id, MSG_INVALID_PARAM);
        } else {
            ESP_LOGE(TAG, "bad WriteRequest (%s), len=%u, dropped",
                     codec_result_name(rc), (unsigned)len);
        }
        return;
    }

    cmd_req_t req = {
        .is_write = true,
        .slave    = w.slave,
        .addr     = w.addr,
        .value    = w.value,
    };
    memcpy(req.req_id, w.req_id, sizeof(req.req_id));
    enqueue(&req);
}

/* ------------------------------------------------------------------
 * 主题比对（topic 不以 '\0' 结尾，必须按长度比较）
 * ------------------------------------------------------------------ */
static bool topic_is(const char *topic, int topic_len, const char *expect)
{
    size_t n = strlen(expect);
    return topic_len == (int)n && memcmp(topic, expect, n) == 0;
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */
void mqtt_cmd_handle_message(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    if (topic == NULL || data == NULL || topic_len <= 0 || data_len <= 0) {
        ESP_LOGW(TAG, "empty message ignored");
        return;
    }

    /* 长度上限在 codec 的 precheck 里还会再查一次，这里先挡一道，
     * 避免超长载荷进入后续流程（P-06 边界安全）。 */
    if (data_len > CMD_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "payload too long (%d > %d), dropped", data_len, CMD_MAX_PAYLOAD);
        return;
    }

    if (topic_is(topic, topic_len, MQTT_TOPIC_WRITE)) {
        handle_write((const uint8_t *)data, (size_t)data_len);
    } else if (topic_is(topic, topic_len, MQTT_TOPIC_READ)) {
        handle_read((const uint8_t *)data, (size_t)data_len);
    } else {
        ESP_LOGW(TAG, "unexpected topic (len=%d), ignored", topic_len);
    }
}

bool mqtt_cmd_init(void)
{
    if (s_queue != NULL) {
        return true;
    }

    s_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_req_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "create request queue failed");
        return false;
    }

    BaseType_t ret = xTaskCreate(cmd_task, "mqtt_cmd", CMD_TASK_STACK, NULL, CMD_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create cmd task failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }

    ESP_LOGI(TAG, "cmd ready: sub \"%s\" & \"%s\", pub \"%s\" (protobuf)",
             MQTT_TOPIC_READ, MQTT_TOPIC_WRITE, MQTT_TOPIC_RESP);
    return true;
}

#endif /* CONFIG_REPORT_PROTO_MQTT */
