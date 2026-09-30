// 从 storage 分区的 wifi.cfg 解析用户 Wi-Fi 配置
#include "wifi_config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "wifi_config";

static wifi_user_config_t s_config;

// 去掉首尾空白（含 \r \n 与空格）
static void trim(char *s)
{
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

// 去掉行内 # 注释
static void strip_comment(char *s)
{
    char *p = strchr(s, '#');
    if (p) {
        *p = '\0';
    }
}

// 转大写（键名不区分大小写）
static void to_upper(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void apply_key(const char *key, const char *value)
{
    if (strcmp(key, WIFI_CONFIG_KEY_SSID) == 0) {
        strncpy(s_config.ssid, value, sizeof(s_config.ssid) - 1);
        s_config.ssid[sizeof(s_config.ssid) - 1] = '\0';
    } else if (strcmp(key, WIFI_CONFIG_KEY_PASSWORD) == 0) {
        strncpy(s_config.password, value, sizeof(s_config.password) - 1);
        s_config.password[sizeof(s_config.password) - 1] = '\0';
    } else if (strcmp(key, WIFI_CONFIG_KEY_IP_ADDR) == 0) {
        strncpy(s_config.ip_addr, value, sizeof(s_config.ip_addr) - 1);
        s_config.ip_addr[sizeof(s_config.ip_addr) - 1] = '\0';
    } else if (strcmp(key, WIFI_CONFIG_KEY_IP_NETMASK) == 0) {
        strncpy(s_config.ip_netmask, value, sizeof(s_config.ip_netmask) - 1);
        s_config.ip_netmask[sizeof(s_config.ip_netmask) - 1] = '\0';
    } else if (strcmp(key, WIFI_CONFIG_KEY_IP_GATEWAY) == 0) {
        strncpy(s_config.ip_gateway, value, sizeof(s_config.ip_gateway) - 1);
        s_config.ip_gateway[sizeof(s_config.ip_gateway) - 1] = '\0';
    } else if (strcmp(key, WIFI_CONFIG_KEY_MDNS) == 0) {
        strncpy(s_config.mdns_hostname, value, sizeof(s_config.mdns_hostname) - 1);
        s_config.mdns_hostname[sizeof(s_config.mdns_hostname) - 1] = '\0';
    }
}

esp_err_t wifi_config_load(void)
{
    memset(&s_config, 0, sizeof(s_config));

    FILE *f = fopen(WIFI_CONFIG_FILE_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "未找到配置文件 %s，使用默认逻辑", WIFI_CONFIG_FILE_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        strip_comment(line);
        trim(line);
        if (line[0] == '\0') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);
        to_upper(key);
        if (key[0] == '\0') {
            continue;
        }
        apply_key(key, value);
    }
    fclose(f);

    // 静态 IP 需三项齐全才生效
    if (s_config.ip_addr[0] || s_config.ip_netmask[0] || s_config.ip_gateway[0]) {
        if (s_config.ip_addr[0] && s_config.ip_netmask[0] && s_config.ip_gateway[0]) {
            s_config.has_ip = true;
        } else {
            ESP_LOGW(TAG, "静态 IP 配置不完整（需同时设置 IP_ADDR/IP_NETMASK/IP_GATEWAY），已忽略");
        }
    }

    ESP_LOGI(TAG, "配置已加载: SSID=%s IP=%s mDNS=%s",
             s_config.ssid[0] ? s_config.ssid : "(默认)",
             s_config.has_ip ? s_config.ip_addr : "(DHCP)",
             s_config.mdns_hostname[0] ? s_config.mdns_hostname : "(默认)");
    return ESP_OK;
}

const wifi_user_config_t *wifi_config_get(void)
{
    return &s_config;
}
