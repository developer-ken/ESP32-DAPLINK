// 引脚与设备参数集中配置：修改这里即可适配不同硬件走线
#pragma once

#include "driver/gpio.h"

// ---------------- SWD 引脚定义 ----------------
#define DAP_SWD_CLK_PIN        GPIO_NUM_4   // SWCLK
#define DAP_SWD_DIO_PIN        GPIO_NUM_5   // SWDIO (双向)

// ---------------- JTAG 引脚定义 ----------------
#define DAP_JTAG_TCK_PIN       GPIO_NUM_4   // 与 SWCLK 共用
#define DAP_JTAG_TMS_PIN       GPIO_NUM_5   // 与 SWDIO 共用
#define DAP_JTAG_TDI_PIN       GPIO_NUM_6
#define DAP_JTAG_TDO_PIN       GPIO_NUM_7

// ---------------- 公共控制引脚 ----------------
#define DAP_NRESET_PIN         GPIO_NUM_15  // 目标复位 (开漏, 低有效)
#define DAP_LED_CONNECT_PIN    GPIO_NUM_2   // 连接状态指示灯
#define DAP_LED_RUNNING_PIN    GPIO_NUM_1   // 运行状态指示灯

// 是否使用 nTRST（多数 Cortex-M 目标不需要）
#define DAP_JTAG_HAS_TRST      0
#define DAP_JTAG_TRST_PIN      GPIO_NUM_16

// ---------------- USB 设备信息 ----------------
#define DAP_USB_VID            0x303A       // Espressif VID
#define DAP_USB_PID            0x8001
#define DAP_USB_MANUFACTURER   "Eggy"
#define DAP_USB_PRODUCT        "Eggy CMSIS-DAP"
#define DAP_FW_VERSION         "v1.0.0"

// ---------------- Wi-Fi 调试桥配置 ----------------
#define WIFI_DEBUG_SSID_KEYWORD   "DEBUG"        // 仅连接 SSID 中包含该关键字的热点
#define WIFI_DEBUG_FIXED_PASSWORD "1250542735"   // 首选密码，失败后尝试 SSID 本身作为密码
#define WIFI_DEBUG_MDNS_HOSTNAME  "eggydebugger" // -> eggydebugger.local
#define WIFI_DEBUG_MDNS_INSTANCE  "Eggy CMSIS-DAP Wi-Fi Bridge"
#define WIFI_DEBUG_BITBANG_PORT   3335           // openocd remote_bitbang 默认口

// USB 是否成功挂载的判定超时时间 (ms)
#define USB_MOUNT_WAIT_MS         3000

// 1MB 只读存储分区在 partitions.csv 中的名字
#define STORAGE_PARTITION_LABEL   "storage"
