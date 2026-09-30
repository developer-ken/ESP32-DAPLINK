// Wi-Fi 调试桥：扫描 SSID 中带有关键字的热点，尝试固定密码/SSID 自身作为密码连接，
// 成功后启动 mDNS 与 openocd remote_bitbang TCP 服务
#include "wifi_bridge.h"
#include "wifi_config.h"
#include "remote_bitbang.h"
#include "cmsis_dap_tcp.h"
#include "dap_config.h"
#include "status_led.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_bridge";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1
#define CONNECT_TIMEOUT_MS  8000

static EventGroupHandle_t s_event_group;

// 红灯反映 Wi-Fi 连接进程：搜索中快闪、连接过程中慢闪、拿到 IP 常亮
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        status_led_set(LED_ID_RED, LED_MODE_SLOW_BLINK); // 掉线/连接未成功：慢闪重试
        xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        status_led_set(LED_ID_RED, LED_MODE_ON);         // 已拿到 IP：常亮
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

// 尝试用给定密码连接一次，成功返回 true
static bool try_connect(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    status_led_set(LED_ID_RED, LED_MODE_SLOW_BLINK); // 正在连接目标热点
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_bridge_start(void)
{
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    // 用户配置：优先使用配置文件中的静态 IP / SSID / mDNS 主机名
    const wifi_user_config_t *cfg = wifi_config_get();
    if (cfg->has_ip) {
        esp_netif_ip_info_t ip_info = {0};
        ip_info.ip.addr = esp_ip4addr_aton(cfg->ip_addr);
        ip_info.netmask.addr = esp_ip4addr_aton(cfg->ip_netmask);
        ip_info.gw.addr = esp_ip4addr_aton(cfg->ip_gateway);
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(sta_netif, &ip_info));
        ESP_LOGI(TAG, "使用静态 IP: %s / %s / %s", cfg->ip_addr, cfg->ip_netmask, cfg->ip_gateway);
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    bool connected = false;

    // 优先尝试连接配置文件指定的热点
    if (cfg->ssid[0] != '\0') {
        ESP_LOGI(TAG, "尝试连接配置文件指定的热点: %s", cfg->ssid);
        connected = try_connect(cfg->ssid, cfg->password);
        if (!connected) {
            ESP_LOGW(TAG, "配置文件热点 %s 连接失败，回退到默认扫描方案", cfg->ssid);
        }
    }

    // 回退：扫描含关键字的调试热点（原有方案）
    if (!connected) {
        wifi_scan_config_t scan_cfg = {0};
        status_led_set(LED_ID_RED, LED_MODE_FAST_BLINK); // 正在搜索热点
        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_cfg, true));

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count == 0) {
            ESP_LOGW(TAG, "未扫描到任何 Wi-Fi 热点");
            return false;
        }

        wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (!records) return false;
        esp_wifi_scan_get_ap_records(&ap_count, records);

        for (uint16_t i = 0; i < ap_count && !connected; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (strstr(ssid, WIFI_DEBUG_SSID_KEYWORD) == NULL) continue;

            ESP_LOGI(TAG, "尝试连接调试热点: %s", ssid);
            if (try_connect(ssid, WIFI_DEBUG_FIXED_PASSWORD)) {
                connected = true;
                break;
            }
            if (try_connect(ssid, ssid)) {
                connected = true;
                break;
            }
            ESP_LOGW(TAG, "连接 %s 失败，尝试下一个热点", ssid);
        }
        free(records);
    }

    if (!connected) {
        ESP_LOGW(TAG, "没有可用的调试热点连接成功");
        return false;
    }

    ESP_LOGI(TAG, "Wi-Fi 已连接");

    const char *mdns_host = (cfg->mdns_hostname[0] != '\0') ? cfg->mdns_hostname : WIFI_DEBUG_MDNS_HOSTNAME;
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(mdns_host);
    mdns_instance_name_set(WIFI_DEBUG_MDNS_INSTANCE);
    mdns_service_add(NULL, "_openocd", "_tcp", WIFI_DEBUG_BITBANG_PORT, NULL, 0);
    mdns_service_add(NULL, "_cmsis-dap", "_tcp", WIFI_DEBUG_CMSIS_DAP_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS 就绪: %s.local", mdns_host);

    remote_bitbang_start(WIFI_DEBUG_BITBANG_PORT);
    cmsis_dap_tcp_start(WIFI_DEBUG_CMSIS_DAP_PORT);
    return true;
}
