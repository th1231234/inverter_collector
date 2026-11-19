#include "udp.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "UDP";

/* ==================================================================
 * 上报目标（由 menuconfig 填写，见 Kconfig.projbuild）
 *   Idf Menuconfig → Inverter Collector → 上报通道
 * ------------------------------------------------------------------
 * 自测（电脑热点）：目标 IP 填热点网关，通常为 192.168.137.1
 * 验收（公司网络）：填 192.168.1.200
 * ================================================================== */
#define UDP_TARGET_IP       CONFIG_REPORT_TARGET_IP
#define UDP_TARGET_PORT     CONFIG_REPORT_TARGET_PORT

static int s_sock = -1;
static struct sockaddr_in s_dest;

bool udp_client_init(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "create socket failed, errno=%d", errno);
        return false;
    }

    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family      = AF_INET;
    s_dest.sin_port        = htons(UDP_TARGET_PORT);
    s_dest.sin_addr.s_addr = inet_addr(UDP_TARGET_IP);

    ESP_LOGI(TAG, "UDP target %s:%d", UDP_TARGET_IP, UDP_TARGET_PORT);
    return true;
}

bool udp_send_json(float voltage, float current, float power)
{
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket not ready");
        return false;
    }

    char payload[128];
    int len = snprintf(payload, sizeof(payload),
                       "{\"voltage\":%.1f,\"current\":%.2f,\"power\":%.0f}",
                       voltage, current, power);
    if (len <= 0 || len >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "json format failed");
        return false;
    }

    int sent = sendto(s_sock, payload, len, 0,
                      (struct sockaddr *)&s_dest, sizeof(s_dest));
    if (sent < 0) {
        ESP_LOGE(TAG, "send to %s:%d failed, errno=%d", UDP_TARGET_IP, UDP_TARGET_PORT, errno);
        return false;
    }

    ESP_LOGI(TAG, "Send to %s:%d success: %s", UDP_TARGET_IP, UDP_TARGET_PORT, payload);
    return true;
}
