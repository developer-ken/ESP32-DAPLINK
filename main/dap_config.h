// 引脚与设备参数集中配置：修改这里即可适配不同硬件走线
#pragma once

#include "driver/gpio.h"

// ---------------- SWD 引脚定义 ----------------
#define DAP_SWD_CLK_PIN        GPIO_NUM_36   // SWCLK
#define DAP_SWD_DIO_PIN        GPIO_NUM_38   // SWDIO (双向)

// ---------------- SWD/JTAG 位带时序校准 ----------------
// 半个 SWCLK/TCK 周期内，除忙等延时外“固定开销”（GPIO 写 + 循环分支）消耗的 CPU 周期数。
// 用途：1) 请求频率足够高时进入 fast 档——不再忙等，由指令开销本身决定实际速率（即 bit-bang 极限）；
//      2) slow 档精确分频——忙等周期数 = 目标半周期 - 本开销。
// 该值与 CPU 主频、引脚是否走 GPIO 矩阵、编译优化等级有关，请用示波器实测 SWCLK 频率微调。
// 经验参考：ESP32-S3 @240MHz ≈ 23（cmsis_dap_tcp_esp32 实测值）。实际频率偏高时调大、偏低时调小。
#define DAP_BIT_OVERHEAD_CYCLES   23

// ---------------- JTAG 引脚定义 ----------------
#define DAP_JTAG_TCK_PIN       GPIO_NUM_36   // 与 SWCLK 共用
#define DAP_JTAG_TMS_PIN       GPIO_NUM_38   // 与 SWDIO 共用
#define DAP_JTAG_TDI_PIN       GPIO_NUM_34  // 原 GPIO6 与板载 LED_ACT 冲突，改用此引脚
#define DAP_JTAG_TDO_PIN       GPIO_NUM_9

// ---------------- 公共控制引脚 ----------------
#define DAP_NRESET_PIN         GPIO_NUM_35  // 目标复位 (开漏, 低有效)
#define DAP_LED_CONNECT_PIN    GPIO_NUM_6   // 连接状态指示灯
#define DAP_LED_RUNNING_PIN    GPIO_NUM_8   // 运行状态指示灯

// 是否使用 nTRST（多数 Cortex-M 目标不需要）
#define DAP_JTAG_HAS_TRST      1
#define DAP_JTAG_TRST_PIN      GPIO_NUM_33

// ---------------- 板载状态灯（低电平点亮）----------------
#define BOARD_LED_PWR_PIN      GPIO_NUM_1   // 上电常亮，表示程序已开始运行
#define BOARD_LED_ACT_PIN      GPIO_NUM_2   // 运行时 0.5s 间隔闪烁，表示主循环存活

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
