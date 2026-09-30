// 从 storage 分区中的配置文件读取用户自定义的 Wi-Fi 配置
// 配置文件格式：每行一条 "KEY=VALUE"，以 # 开头的行为注释（默认全部注释，即使用固件默认逻辑）
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// storage 分区中配置文件的路径（storage 挂载到 APP 端时可访问）
#define WIFI_CONFIG_FILE_PATH   "/storage/wifi.cfg"

// 配置文件中的键名
#define WIFI_CONFIG_KEY_SSID        "WIFI_SSID"
#define WIFI_CONFIG_KEY_PASSWORD    "WIFI_PASSWORD"
#define WIFI_CONFIG_KEY_IP_ADDR     "IP_ADDR"
#define WIFI_CONFIG_KEY_IP_NETMASK  "IP_NETMASK"
#define WIFI_CONFIG_KEY_IP_GATEWAY  "IP_GATEWAY"
#define WIFI_CONFIG_KEY_MDNS        "MDNS_HOSTNAME"

// 默认配置模板（首次格式化 storage 时写入，全部注释）
#define WIFI_CONFIG_FILE_TEMPLATE                                        \
    "# Eggy CMSIS-DAP Wi-Fi 配置\r\n"                                     \
    "# 取消某一行开头的 # 即可启用该配置；未启用的项继续使用固件默认逻辑。\r\n" \
    "# 固件默认：自动扫描 SSID 含 \"DEBUG\" 的 2.4GHz 热点并尝试连接。\r\n"   \
    "\r\n"                                                                \
    "# 要连接的 Wi-Fi 名称（启用后优先连接该热点）\r\n"                     \
    "#WIFI_SSID=你的WiFi名称\r\n"                                         \
    "\r\n"                                                                \
    "# Wi-Fi 密码（开放网络可留空）\r\n"                                   \
    "#WIFI_PASSWORD=你的WiFi密码\r\n"                                     \
    "\r\n"                                                                \
    "# 静态 IP 地址（三项需同时启用，否则仍使用 DHCP）\r\n"                 \
    "#IP_ADDR=192.168.1.100\r\n"                                          \
    "#IP_NETMASK=255.255.255.0\r\n"                                       \
    "#IP_GATEWAY=192.168.1.1\r\n"                                         \
    "\r\n"                                                                \
    "# mDNS 主机名（启用后使用该名称，默认 eggydebugger）\r\n"              \
    "#MDNS_HOSTNAME=mydebugger\r\n"

typedef struct {
    char ssid[33];            // 用户指定的 SSID（空串表示未设置）
    char password[65];        // 用户指定的密码（空串表示开放网络）
    bool has_ip;              // 是否完整设置了静态 IP
    char ip_addr[16];
    char ip_netmask[16];
    char ip_gateway[16];
    char mdns_hostname[64];   // 用户指定的 mDNS 主机名（空串表示未设置）
} wifi_user_config_t;

// 从 WIFI_CONFIG_FILE_PATH 读取并解析配置（需在 storage 挂载到 APP 端时调用）
// 配置文件不存在时返回 ESP_ERR_NOT_FOUND，配置保持为空（即全部使用默认逻辑）
esp_err_t wifi_config_load(void);

// 返回当前已加载的配置（未加载过则为空配置）
const wifi_user_config_t *wifi_config_get(void);

#ifdef __cplusplus
}
#endif
