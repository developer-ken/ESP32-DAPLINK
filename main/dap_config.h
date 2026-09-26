// 引脚与设备参数集中配置：修改这里即可适配不同硬件走线
#pragma once

#include "driver/gpio.h"

// ---------------- SWD 引脚定义 ----------------
#define DAP_SWD_CLK_PIN        GPIO_NUM_36   // SWCLK
#define DAP_SWD_DIO_PIN        GPIO_NUM_38   // SWDIO (双向)

// ---------------- JTAG 引脚定义 ----------------
#define DAP_JTAG_TCK_PIN       DAP_SWD_CLK_PIN   // 与 SWCLK 共用
#define DAP_JTAG_TMS_PIN       DAP_SWD_DIO_PIN   // 与 SWDIO 共用
#define DAP_JTAG_TDI_PIN       GPIO_NUM_34
#define DAP_JTAG_TDO_PIN       GPIO_NUM_29

// ---------------- 公共控制引脚 ----------------
#define DAP_NRESET_PIN         GPIO_NUM_35  // 目标复位 (开漏, 低有效)
#define DAP_LED_CONNECT_PIN    GPIO_NUM_8   // 连接状态指示灯
#define DAP_LED_RUNNING_PIN    GPIO_NUM_6   // 运行状态指示灯

// 是否使用 nTRST（多数 Cortex-M 目标不需要）
#define DAP_JTAG_HAS_TRST      1
#define DAP_JTAG_TRST_PIN      GPIO_NUM_33

// ---------------- 板载状态灯（低电平点亮）----------------
// 映射到未使用引脚
#define BOARD_LED_PWR_PIN      GPIO_NUM_42   // 上电常亮，表示程序已开始运行
#define BOARD_LED_ACT_PIN      GPIO_NUM_41   // 运行时 0.5s 间隔闪烁，表示主循环存活

// ---------------- USB 设备信息 ----------------
#define DAP_USB_VID            0x303A       // Espressif VID
#define DAP_USB_PID            0x8001
#define DAP_USB_MANUFACTURER   "AnEgg"
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
