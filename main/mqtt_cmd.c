#include "sdkconfig.h"

/* 非 MQTT 通道时不编译本文件内容，避免引用未定义的 CONFIG_MQTT_* 宏 */
#if CONFIG_REPORT_PROTO_MQTT

#include "mqtt_cmd.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "modbus.h"
#include "mqtt_report.h"

static const char *TAG = "MQTT_CMD";

/* ==================================================================
 * 边界与容量：所有上限集中在此，代码里不出现魔法数字
 * ================================================================== */
#define CMD_MAX_PAYLOAD     512   /* 载荷上限，超过直接丢弃（M-08 的 1KB 超长 JSON） */
#define CMD_MAX_REQ_ID      32    /* req_id 最长 31 字符 + '\0' */
#define CMD_MAX_READ_NUM    16    /* 单次最多读 16 个寄存器 */
#define CMD_QUEUE_LEN       8     /* 待执行请求队列深度 */
#define CMD_TASK_STACK      4096  /* cJSON + Modbus 调用，4096 足够 */
#define CMD_TASK_PRIO       5

/* Modbus 协议规定的取值范围 */
#define MODBUS_SLAVE_MIN    1
#define MODBUS_SLAVE_MAX    247
#define MODBUS_ADDR_MAX     0xFFFF

/* 模块内的错误码，直接对应 resp 里的 msg 字段 */
#define MSG_INVALID_PARAM   "invalid_param"
#define MSG_BUSY            "request_busy"
#define MSG_INTERNAL        "internal_error"
#define MSG_PUBLISH_FAIL    "publish_failed"

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
 * req_id 白名单校验
 *
 * 只允许字母数字下划线连字符。这样既堵住了向 JSON 里注入引号的可能，
 * 也避免为了转义去动态分配内存（边界安全加分项）。
 * ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------
 * 取一个无符号整数并校验范围
 * 非数字类型（字符串 / bool / null）一律拒绝，避免 "1" 这种被误用
 * ------------------------------------------------------------------ */
static bool json_get_uint(const cJSON *item, unsigned min, unsigned max, unsigned *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }

    double d = item->valuedouble;
    if (d < (double)min || d > (double)max) {
        return false;
    }

    *out = (unsigned)d;
    return true;
}

/* ------------------------------------------------------------------
 * 组包并发布应答
 * ------------------------------------------------------------------ */
static bool publish_resp(cJSON *root)
{
    if (root == NULL) {
        return false;
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        ESP_LOGE(TAG, "build resp json failed");
        return false;
    }

    int len = (int)strlen(text);
    bool ok = mqtt_report_publish(MQTT_TOPIC_RESP, text, len);
    cJSON_free(text);

    if (!ok) {
        ESP_LOGE(TAG, "publish resp to \"%s\" failed", MQTT_TOPIC_RESP);
    }
    return ok;
}

/* {"req_id":"7","result":"error","msg":"modbus_timeout"} */
static void send_resp_error(const char *req_id, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "create resp object failed");
        return;
    }

    cJSON_AddStringToObject(root, "req_id", req_id);
    cJSON_AddStringToObject(root, "result", "error");
    cJSON_AddStringToObject(root, "msg", msg);

    ESP_LOGW(TAG, "resp error: req_id=%s msg=%s", req_id, msg);
    publish_resp(root);
}

/* {"req_id":"2","result":"ok"} */
static void send_resp_ok(const char *req_id)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "create resp object failed");
        return;
    }

    cJSON_AddStringToObject(root, "req_id", req_id);
    cJSON_AddStringToObject(root, "result", "ok");

    ESP_LOGI(TAG, "resp ok: req_id=%s", req_id);
    publish_resp(root);
}

/* {"req_id":"1","result":"ok","values":[3500,500,1750]} */
static void send_resp_ok_values(const char *req_id, const uint16_t *vals, int num)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "create resp object failed");
        return;
    }

    cJSON_AddStringToObject(root, "req_id", req_id);
    cJSON_AddStringToObject(root, "result", "ok");

    cJSON *arr = cJSON_AddArrayToObject(root, "values");
    if (arr == NULL) {
        cJSON_Delete(root);
        send_resp_error(req_id, MSG_INTERNAL);
        return;
    }

    for (int i = 0; i < num; i++) {
        cJSON *item = cJSON_CreateNumber((double)vals[i]);
        if (item == NULL || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            send_resp_error(req_id, MSG_INTERNAL);
            return;
        }
    }

    ESP_LOGI(TAG, "resp ok: req_id=%s, %d value(s)", req_id, num);
    publish_resp(root);
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

        /* read：req.num 已在入队前限制在 1~CMD_MAX_READ_NUM，缓冲区定长安全 */
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
 * 解析 + 校验 + 入队
 * ------------------------------------------------------------------ */
static void handle_cmd(bool is_write, const char *payload)
{
    const char *topic = is_write ? MQTT_TOPIC_WRITE : MQTT_TOPIC_READ;

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        /* 坏 JSON 里取不到 req_id，无法组成合法应答，只能记日志后丢弃。
         * 指导书 M-07 要求的正是「打印 bad json，进程不重启」。 */
        ESP_LOGE(TAG, "bad json on %s, dropped", topic);
        return;
    }

    cmd_req_t req = { .is_write = is_write };
    unsigned u = 0;
    const char *err = NULL;

    /* ---- req_id：必须存在且字符合法 ---- */
    cJSON *j_id = cJSON_GetObjectItemCaseSensitive(root, "req_id");
    if (!cJSON_IsString(j_id) || j_id->valuestring == NULL ||
        !req_id_is_valid(j_id->valuestring)) {
        ESP_LOGE(TAG, "missing or illegal req_id on %s, dropped", topic);
        cJSON_Delete(root);
        return;
    }
    size_t id_len = strlen(j_id->valuestring);          /* 已校验 < CMD_MAX_REQ_ID */
    memcpy(req.req_id, j_id->valuestring, id_len + 1);

    /* ---- slave：1~247 ---- */
    cJSON *j_slave = cJSON_GetObjectItemCaseSensitive(root, "slave");
    if (!json_get_uint(j_slave, MODBUS_SLAVE_MIN, MODBUS_SLAVE_MAX, &u)) {
        err = MSG_INVALID_PARAM;
        goto done;
    }
    req.slave = (uint8_t)u;

    /* ---- addr：0~65535（70000 会被这里拦下）---- */
    cJSON *j_addr = cJSON_GetObjectItemCaseSensitive(root, "addr");
    if (!json_get_uint(j_addr, 0, MODBUS_ADDR_MAX, &u)) {
        err = MSG_INVALID_PARAM;
        goto done;
    }
    req.addr = (uint16_t)u;

    if (is_write) {
        /* ---- value：0~65535 ---- */
        cJSON *j_value = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (!json_get_uint(j_value, 0, MODBUS_ADDR_MAX, &u)) {
            err = MSG_INVALID_PARAM;
            goto done;
        }
        req.value = (uint16_t)u;
    } else {
        /* ---- num：1~16 且不越出地址空间顶端 ---- */
        cJSON *j_num = cJSON_GetObjectItemCaseSensitive(root, "num");
        if (!json_get_uint(j_num, 1, CMD_MAX_READ_NUM, &u)) {
            err = MSG_INVALID_PARAM;
            goto done;
        }
        if ((unsigned)req.addr + u - 1U > MODBUS_ADDR_MAX) {
            err = MSG_INVALID_PARAM;
            goto done;
        }
        req.num = (uint16_t)u;
    }

done:
    cJSON_Delete(root);

    if (err != NULL) {
        ESP_LOGW(TAG, "invalid param on %s (req_id=%s), reply error", topic, req.req_id);
        send_resp_error(req.req_id, err);
        return;
    }

    /* 入队失败说明上一批请求还没处理完，明确回 busy 而不是静默丢弃 */
    if (s_queue == NULL || xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, req_id=%s rejected", req.req_id);
        send_resp_error(req.req_id, MSG_BUSY);
    }
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

    ESP_LOGI(TAG, "cmd ready: sub \"%s\" & \"%s\", pub \"%s\"",
             MQTT_TOPIC_READ, MQTT_TOPIC_WRITE, MQTT_TOPIC_RESP);
    return true;
}

#endif /* CONFIG_REPORT_PROTO_MQTT */
