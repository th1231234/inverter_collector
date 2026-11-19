#include "tcp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "TCP";

/* ==================================================================
 * 上报目标（由 menuconfig 填写，见 Kconfig.projbuild）
 *   Idf Menuconfig → Inverter Collector → 上报通道
 * ================================================================== */
#define TCP_TARGET_IP           CONFIG_REPORT_TARGET_IP
#define TCP_TARGET_PORT         CONFIG_REPORT_TARGET_PORT
#define TCP_CONNECT_TIMEOUT_S   3
#define TCP_SEND_TIMEOUT_S      3

static int s_sock = -1;
static struct sockaddr_in s_dest;

/* ------------------------------------------------------------------
 * 建立连接
 * 用「非阻塞 connect + select 限时」而不是直接阻塞 connect：
 * 目标不可达时 lwIP 会重传 SYN 十几秒，那会把采集任务整个卡死。
 * ------------------------------------------------------------------ */
static bool tcp_do_connect(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "create socket failed, errno=%d", errno);
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, (struct sockaddr *)&s_dest, sizeof(s_dest));
    if (ret < 0 && errno != EINPROGRESS) {
        ESP_LOGE(TAG, "connect %s:%d failed, errno=%d",
                 TCP_TARGET_IP, TCP_TARGET_PORT, errno);
        close(sock);
        return false;
    }

    if (ret < 0) {
        fd_set wfds;
        struct timeval tv = { .tv_sec = TCP_CONNECT_TIMEOUT_S, .tv_usec = 0 };
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        ret = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret == 0) {
            ESP_LOGE(TAG, "connect %s:%d timeout (%d s)",
                     TCP_TARGET_IP, TCP_TARGET_PORT, TCP_CONNECT_TIMEOUT_S);
            close(sock);
            return false;
        }
        if (ret < 0) {
            ESP_LOGE(TAG, "select failed, errno=%d", errno);
            close(sock);
            return false;
        }

        int so_err = 0;
        socklen_t opt_len = sizeof(so_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &opt_len);
        if (so_err != 0) {
            ESP_LOGE(TAG, "connect %s:%d refused, so_error=%d",
                     TCP_TARGET_IP, TCP_TARGET_PORT, so_err);
            close(sock);
            return false;
        }
    }

    /* 恢复阻塞模式，并限制单次 send 的阻塞时间 */
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    struct timeval snd_tv = { .tv_sec = TCP_SEND_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

    s_sock = sock;
    ESP_LOGI(TAG, "connected to %s:%d", TCP_TARGET_IP, TCP_TARGET_PORT);
    return true;
}

bool tcp_client_init(void)
{
    memset(&s_dest, 0, sizeof(s_dest));
    s_dest.sin_family      = AF_INET;
    s_dest.sin_port        = htons(TCP_TARGET_PORT);
    s_dest.sin_addr.s_addr = inet_addr(TCP_TARGET_IP);

    ESP_LOGI(TAG, "TCP target %s:%d (connect on first send)",
             TCP_TARGET_IP, TCP_TARGET_PORT);
    return true;
}

bool tcp_client_is_connected(void)
{
    return s_sock >= 0;
}

void tcp_client_close(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

bool tcp_client_reconnect(void)
{
    tcp_client_close();
    return tcp_do_connect();
}

/* ------------------------------------------------------------------
 * 发送一条 JSON
 * 注意：TCP 是字节流，没有消息边界，因此每条末尾追加 '\n' 作为分隔符，
 *       接收端需按行切分（对比 UDP：一包即一条消息，无需分隔符）。
 * ------------------------------------------------------------------ */
bool tcp_send_json(float voltage, float current, float power)
{
    if (s_sock < 0 && !tcp_do_connect()) {
        return false;
    }

    char payload[128];
    int len = snprintf(payload, sizeof(payload),
                       "{\"voltage\":%.1f,\"current\":%.2f,\"power\":%.0f}\n",
                       voltage, current, power);
    if (len <= 0 || len >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "json format failed");
        return false;
    }

    int sent = send(s_sock, payload, len, 0);
    if (sent != len) {
        ESP_LOGE(TAG, "send failed (%d/%d), close and rebuild next cycle", sent, len);
        tcp_client_close();
        return false;
    }

    ESP_LOGI(TAG, "Send to %s:%d success: %.*s",
             TCP_TARGET_IP, TCP_TARGET_PORT, len - 1, payload);
    return true;
}
