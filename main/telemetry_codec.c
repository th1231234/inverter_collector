#include "telemetry_codec.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "telemetry.pb-c.h"

static const char *TAG = "CODEC";

/* ------------------------------------------------------------------
 * Modbus 协议规定的取值范围（与 mqtt_cmd 一致，这里只做协议层校验）
 * ------------------------------------------------------------------ */
#define MODBUS_SLAVE_MIN   1
#define MODBUS_SLAVE_MAX   247
#define MODBUS_ADDR_MAX    0xFFFFU

static volatile bool s_time_synced = false;
static bool s_sntp_started = false;

/* ==================================================================
 * 时间基准
 * ================================================================== */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP synced, real time available");
}

void codec_time_start(void)
{
    /* 幂等：重复调用直接返回，避免第二次 esp_sntp_init 覆盖掉已注册的回调 */
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    /* POLL 模式：一次性请求后停止，不需要持续回调 */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    /* 国内优先阿里云 NTP，失败自动换下一个 */
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
}

bool codec_time_is_synced(void)
{
    return s_time_synced;
}

int64_t codec_now_ms(void)
{
    if (s_time_synced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
    /* 未对时：退回开机毫秒。上位机看到 ts_ms 很小即可判断「这是相对时间」 */
    return esp_timer_get_time() / 1000;
}

/* ==================================================================
 * 通用：打包到新缓冲
 * ------------------------------------------------------------------
 * 三步走：get_packed_size → malloc → pack。
 * 不写死缓冲区大小，因为 metrics 数量可变。
 * ================================================================== */
static bool pack_message(const ProtobufCMessage *msg, uint8_t **out, size_t *out_len)
{
    if (msg == NULL || out == NULL || out_len == NULL) {
        return false;
    }

    size_t len = protobuf_c_message_get_packed_size(msg);
    if (len == 0 || len > CODEC_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "packed size out of range: %u", (unsigned)len);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc %u bytes failed", (unsigned)len);
        return false;
    }

    size_t written = protobuf_c_message_pack(msg, buf);
    if (written != len) {
        /* 理论不会发生：算出来的长度和实际写入不一致，说明描述符有问题 */
        ESP_LOGE(TAG, "pack size mismatch: expect %u got %u",
                 (unsigned)len, (unsigned)written);
        free(buf);
        return false;
    }

    *out     = buf;
    *out_len = written;
    return true;
}

void codec_free(uint8_t *buf)
{
    if (buf != NULL) {
        free(buf);
    }
}

/* ==================================================================
 * 编码：遥测帧
 * ================================================================== */
bool codec_encode_telemetry(uint32_t seq, int32_t state,
                            const codec_metric_t *pts, int n_metrics,
                            uint8_t **out, size_t *out_len)
{
    if (pts == NULL || n_metrics <= 0 || n_metrics > CODEC_MAX_METRICS) {
        ESP_LOGE(TAG, "invalid metrics arg: n=%d", n_metrics);
        return false;
    }

    /* ---- 测点：每个 Metric 单独分配，再用指针数组串起来 ----
     * protobuf-c 生成的字段是 Metric **metrics + size_t n_metrics，
     * 也就是「指向指针的指针」，所以这里要两层分配。
     * 全部用 calloc，保证未显式赋值的字段是 0，符合 proto3 默认值语义。 */
    Metric *metrics = (Metric *)calloc((size_t)n_metrics, sizeof(Metric));
    Metric **ptrs   = (Metric **)calloc((size_t)n_metrics, sizeof(Metric *));
    if (metrics == NULL || ptrs == NULL) {
        ESP_LOGE(TAG, "alloc metrics failed");
        free(metrics);
        free(ptrs);
        return false;
    }

    for (int i = 0; i < n_metrics; i++) {
        metric__init(&metrics[i]);
        metrics[i].slave = pts[i].slave;
        metrics[i].addr  = pts[i].addr;
        metrics[i].raw   = pts[i].raw;
        metrics[i].value = pts[i].value;
        ptrs[i] = &metrics[i];
    }

    Telemetry t = TELEMETRY__INIT;
    t.device_id = (char *)CONFIG_MQTT_DEV_ID;   /* 与 Client ID 一致 */
    t.seq       = seq;
    t.ts_ms     = codec_now_ms();
    t.state     = state;
    t.n_metrics = (size_t)n_metrics;
    t.metrics   = ptrs;

    bool ok = pack_message(&t.base, out, out_len);

    /* pack 完成后 t 不再被引用，两层缓冲都可以释放 */
    free(ptrs);
    free(metrics);
    return ok;
}

/* ==================================================================
 * 解码：共用校验
 * ================================================================== */

/* req_id 白名单校验：与 mqtt_cmd 保持同一套规则 */
static bool req_id_is_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= CODEC_MAX_REQ_ID) {
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

/* 长度预检：必须在 unpack 之前，避免解析超长或空输入（P-06） */
static codec_result_t precheck(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return CODEC_ERR_UNPACK;
    }
    if (len > CODEC_MAX_PAYLOAD) {
        return CODEC_ERR_TOO_LONG;
    }
    return CODEC_OK;
}

const char *codec_result_name(codec_result_t r)
{
    switch (r) {
    case CODEC_OK:            return "ok";
    case CODEC_ERR_TOO_LONG:  return "payload_too_long";
    case CODEC_ERR_UNPACK:    return "unpack_failed";
    case CODEC_ERR_REQ_ID:    return "bad_req_id";
    case CODEC_ERR_RANGE:     return "out_of_range";
    default:                  return "unknown";
    }
}

codec_result_t codec_decode_read(const uint8_t *data, size_t len, codec_read_req_t *out)
{
    if (out == NULL) {
        return CODEC_ERR_RANGE;
    }

    codec_result_t pre = precheck(data, len);
    if (pre != CODEC_OK) {
        return pre;
    }

    ReadRequest *req = read_request__unpack(NULL, len, data);
    if (req == NULL) {
        /* 数据非法或被截断：unpack 返回 NULL，绝不能继续解引用 */
        return CODEC_ERR_UNPACK;
    }

    codec_result_t result = CODEC_OK;
    const char *id = (req->req_id != NULL) ? req->req_id : "";

    if (!req_id_is_valid(id)) {
        result = CODEC_ERR_REQ_ID;
        goto done;
    }

    /* unpack 出来的 req_id 指向结构体内部，free_unpacked 之后就失效，
     * 所以必须拷贝到调用方的缓冲区里。
     * 先拷贝再校验取值范围：这样即使是 CODEC_ERR_RANGE，
     * 调用方也能拿到 req_id 去回一条带 req_id 的错误应答，
     * 而不是像坏报文那样只能静默丢弃。 */
    memcpy(out->req_id, id, strlen(id) + 1);

    if (req->slave < MODBUS_SLAVE_MIN || req->slave > MODBUS_SLAVE_MAX ||
        req->addr > MODBUS_ADDR_MAX ||
        req->num == 0 || req->num > CODEC_MAX_READ_NUM ||
        (unsigned)req->addr + req->num - 1U > MODBUS_ADDR_MAX) {
        result = CODEC_ERR_RANGE;
        goto done;
    }

    out->slave = (uint8_t)req->slave;
    out->addr  = (uint16_t)req->addr;
    out->num   = (uint16_t)req->num;

done:
    read_request__free_unpacked(req, NULL);      /* 任何路径都必须释放 */
    return result;
}

codec_result_t codec_decode_write(const uint8_t *data, size_t len, codec_write_req_t *out)
{
    if (out == NULL) {
        return CODEC_ERR_RANGE;
    }

    codec_result_t pre = precheck(data, len);
    if (pre != CODEC_OK) {
        return pre;
    }

    WriteRequest *req = write_request__unpack(NULL, len, data);
    if (req == NULL) {
        return CODEC_ERR_UNPACK;
    }

    codec_result_t result = CODEC_OK;
    const char *id = (req->req_id != NULL) ? req->req_id : "";

    if (!req_id_is_valid(id)) {
        result = CODEC_ERR_REQ_ID;
        goto done;
    }

    /* 同上：先拷 req_id，越界时调用方还能回一条带 req_id 的错误应答 */
    memcpy(out->req_id, id, strlen(id) + 1);

    if (req->slave < MODBUS_SLAVE_MIN || req->slave > MODBUS_SLAVE_MAX ||
        req->addr > MODBUS_ADDR_MAX || req->value > MODBUS_ADDR_MAX) {
        result = CODEC_ERR_RANGE;
        goto done;
    }

    out->slave = (uint8_t)req->slave;
    out->addr  = (uint16_t)req->addr;
    out->value = (uint16_t)req->value;

done:
    write_request__free_unpacked(req, NULL);
    return result;
}

/* ==================================================================
 * 编码：应答
 * ================================================================== */
bool codec_encode_resp_ok(const char *req_id, uint8_t **out, size_t *out_len)
{
    if (req_id == NULL) {
        return false;
    }
    Resp r = RESP__INIT;
    r.req_id = (char *)req_id;
    r.ok     = true;
    r.msg    = (char *)"";      /* proto3 中空串等同默认值，但显式赋值更清晰 */
    return pack_message(&r.base, out, out_len);
}

bool codec_encode_resp_values(const char *req_id,
                              const uint16_t *vals, int n,
                              uint8_t **out, size_t *out_len)
{
    if (req_id == NULL || vals == NULL || n < 0 || n > CODEC_MAX_READ_NUM) {
        return false;
    }

    uint32_t *values = NULL;
    if (n > 0) {
        values = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
        if (values == NULL) {
            ESP_LOGE(TAG, "alloc values failed");
            return false;
        }
        for (int i = 0; i < n; i++) {
            values[i] = vals[i];
        }
    }

    Resp r = RESP__INIT;
    r.req_id    = (char *)req_id;
    r.ok        = true;
    r.msg       = (char *)"";
    r.n_values  = (size_t)n;
    r.values    = values;

    bool ok = pack_message(&r.base, out, out_len);
    free(values);
    return ok;
}

bool codec_encode_resp_error(const char *req_id, const char *msg,
                             uint8_t **out, size_t *out_len)
{
    if (req_id == NULL) {
        return false;
    }
    Resp r = RESP__INIT;
    r.req_id = (char *)req_id;
    r.ok     = false;
    r.msg    = (char *)((msg != NULL) ? msg : "unknown_error");
    return pack_message(&r.base, out, out_len);
}
