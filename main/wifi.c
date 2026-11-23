#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI";

/* ==================================================================
 * 网络参数（由 menuconfig 填写，见 Kconfig.projbuild）
 *   Idf Menuconfig → Inverter Collector → Wi-Fi 参数
 * ------------------------------------------------------------------
 * 自测：电脑热点；验收：公司路由器（SSID / 密码由导师提供）
 * 代码中不硬编码任何真实凭据，sdkconfig 已加入 .gitignore
 * ================================================================== */
#define WIFI_SSID       CONFIG_WIFI_SSID
#define WIFI_PASSWORD   CONFIG_WIFI_PASSWORD

/* 断线重连：不限次数，直到连上为止 */
#define WIFI_RETRY_FOREVER   0

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT   BIT0

static bool s_connected = false;
static char s_ip[16] = "0.0.0.0";
static int  s_retry_cnt = 0;

/* ------------------------------------------------------------------
 * 事件回调：处理 Wi-Fi 事件与 IP 事件
 * ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA start, connecting to \"%s\" ...", WIFI_SSID);
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 断线：立即重连，这是"自动重连"的核心 */
        s_connected = false;
        s_retry_cnt++;
        ESP_LOGW(TAG, "disconnected (reason=%d), reconnect attempt %d ...",
                 ((wifi_event_sta_disconnected_t *)event_data)->reason, s_retry_cnt);
        esp_wifi_connect();

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* 拿到 IP 才算真正可用 */
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        s_retry_cnt = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: %s", s_ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------
 * 初始化 NVS / 网络栈 / Wi-Fi，并启动连接
 * ------------------------------------------------------------------ */
bool wifi_init_sta(void)
{
    /* 1. NVS：Wi-Fi 驱动需要保存校准数据 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 网络接口与事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();

    /* 3. Wi-Fi 驱动初始化 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 4. 注册事件：Wi-Fi 事件 + IP 事件（题目要求用事件循环处理 GOT_IP） */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    /* 5. 配置为 STA 模式并填入 SSID / 密码
     *    认证方式放宽为 WPA/WPA2 兼容，并声明支持 PMF，提高与各类热点/路由器的兼容性
     */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished, SSID=%s", WIFI_SSID);
    return true;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

/* ------------------------------------------------------------------
 * 阻塞等待"已拿到 IP"这个事件
 *   pdFALSE = 不清除事件位（后续再等仍能立刻返回）
 *   pdTRUE  = 等待所有指定位都满足（这里只有一位）
 * ------------------------------------------------------------------ */
bool wifi_wait_connected(uint32_t timeout_ms)
{
    /* 已经连上就不必再等（例如断线重连后很快恢复的情况） */
    if (s_connected) {
        return true;
    }
    if (s_wifi_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

const char *wifi_get_ip(void)
{
    return s_ip;
}
