#include "sdkconfig.h"

/* 非 MQTT 通道时不编译本文件内容，避免引用未定义的 CONFIG_MQTT_* 宏 */
#if CONFIG_REPORT_PROTO_MQTT

#include "mqtt_report.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"

#include "mqtt_cmd.h"

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;

/* client_id 必须全局唯一，否则同一 Broker 上后连的会把先连的踢掉。
 * 指导书 2.4 要求用自己工号，工号即 CONFIG_MQTT_DEV_ID。 */
#define MQTT_CLIENT_ID  CONFIG_MQTT_DEV_ID

/* ------------------------------------------------------------------
 * 连上后立即订阅两个下行主题
 *
 * 必须放在 CONNECTED 回调里：默认 clean session 不保留会话，
 * 断线重连后订阅关系会丢，只订一次的话重连后就再也收不到命令。
 * ------------------------------------------------------------------ */
static void subscribe_cmd_topics(void)
{
    int id_read  = esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_READ, 1);
    int id_write = esp_mqtt_client_subscribe(s_client, MQTT_TOPIC_WRITE, 1);

    ESP_LOGI(TAG, "subscribed \"%s\" (msg_id=%d), \"%s\" (msg_id=%d)",
             MQTT_TOPIC_READ, id_read, MQTT_TOPIC_WRITE, id_write);
}

/* ------------------------------------------------------------------
 * 事件回调：连上 → 订阅；断开 → 记日志；收到数据 → 交给命令模块
 * ------------------------------------------------------------------ */
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "broker connected: %s", CONFIG_MQTT_BROKER_URI);
        ESP_LOGI(TAG, "client_id=%s, pub topic=%s", MQTT_CLIENT_ID, MQTT_TOPIC_DATA);
        subscribe_cmd_topics();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "broker disconnected, waiting auto reconnect");
        break;

    case MQTT_EVENT_DATA:
        /* 超过接收缓冲的载荷会被 esp-mqtt 拆成多片，每片单独回调一次，
         * 此时 data_len 只是分片长度、total_data_len 才是整条长度。
         *
         * 必须整条丢弃：protobuf 是变长编码，残片有一定概率被解出一条
         * 字段错位的报文（长度字段恰好落在合法范围），从而执行一条本来
         * 该被拒绝的命令。宁可丢一条也不能执行错的。 */
        if (event->total_data_len != event->data_len) {
            ESP_LOGW(TAG, "fragmented payload (%d/%d bytes), dropped",
                     event->data_len, event->total_data_len);
            break;
        }
        /* 回调里只做校验与入队，Modbus 读写交给 mqtt_cmd 自己的任务 */
        mqtt_cmd_handle_message(event->topic, event->topic_len,
                                event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        s_connected = false;
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error: sock_errno=%d, tls_last_err=0x%x",
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->esp_tls_last_esp_err);
        } else {
            ESP_LOGE(TAG, "mqtt error, type=%d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------ */
bool mqtt_report_init(void)
{
    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri            = CONFIG_MQTT_BROKER_URI;
    cfg.credentials.client_id         = MQTT_CLIENT_ID;
    cfg.session.keepalive             = 60;
    cfg.network.reconnect_timeout_ms  = 5000;
    /* 收 / 发缓冲区：要装得下最长 512 字节的命令载荷，留一倍余量。
     * （ESP-IDF 5.5 的 buffer 结构只有 size 和 out_size，没有 in_size） */
    cfg.buffer.size                   = 1024;
    cfg.buffer.out_size               = 1024;

    /* 留空则为匿名连接，公共测试服务器支持匿名 */
    if (strlen(CONFIG_MQTT_USERNAME) > 0) {
        cfg.credentials.username = CONFIG_MQTT_USERNAME;
    }
    if (strlen(CONFIG_MQTT_PASSWORD) > 0) {
        cfg.credentials.authentication.password = CONFIG_MQTT_PASSWORD;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return false;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register event failed: %s", esp_err_to_name(err));
        return false;
    }

    /* 命令队列与执行任务必须在客户端启动前就绪，避免首条命令到达时无处可放 */
    if (!mqtt_cmd_init()) {
        ESP_LOGE(TAG, "mqtt_cmd_init failed");
        return false;
    }

    /* Telemetry.ts_ms 要真实 Unix 毫秒，先起 SNTP 对时。
     * 放在这里是因为调用方已等到 Wi-Fi 拿到 IP；对时是异步的，
     * 没对上之前 ts_ms 退化为开机毫秒，不阻塞上报。 */
    codec_time_start();

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "mqtt client started, waiting for broker ...");
    return true;
}

bool mqtt_report_is_connected(void)
{
    return s_connected;
}

/* ------------------------------------------------------------------
 * 发布
 * ------------------------------------------------------------------ */
bool mqtt_report_publish(const char *topic, const char *payload, int len)
{
    if (s_client == NULL || topic == NULL || payload == NULL || len <= 0) {
        return false;
    }
    if (!s_connected) {
        ESP_LOGW(TAG, "broker not connected yet, drop publish to \"%s\"", topic);
        return false;
    }

    /* QoS 1：至少送达一次。len 用真实字节数，二进制载荷也不会被 0x00 截断 */
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, len, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish to \"%s\" failed (msg_id=%d)", topic, msg_id);
        return false;
    }
    return true;
}

bool mqtt_report_send_telemetry(uint32_t seq, int32_t state,
                                const codec_metric_t *pts, int n)
{
    uint8_t *buf  = NULL;
    size_t   len  = 0;

    if (!codec_encode_telemetry(seq, state, pts, n, &buf, &len)) {
        ESP_LOGE(TAG, "encode telemetry failed (seq=%u)", (unsigned)seq);
        return false;
    }

    /* 关键：传 len 而不是 strlen((char *)buf)。
     * 报文里 raw=0 / addr=0 都会产生 0x00 字节，按字符串量长度会截断，
     * 对端 unpack 直接失败 —— 这就是 P-05「二进制完整性」要验的点。 */
    bool ok = mqtt_report_publish(MQTT_TOPIC_DATA, (const char *)buf, (int)len);
    codec_free(buf);

    if (!ok) {
        return false;
    }

    ESP_LOGI(TAG, "data -> \"%s\": seq=%u state=%d %d metric(s), %u bytes",
             MQTT_TOPIC_DATA, (unsigned)seq, (int)state, n, (unsigned)len);
    return true;
}

#endif /* CONFIG_REPORT_PROTO_MQTT */
