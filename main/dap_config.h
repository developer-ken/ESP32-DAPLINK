// 引脚与设备参数集中配置：修改这里即可适配不同硬件走线
#pragma once

#include "driver/gpio.h"

// ---------------- SWD 引脚定义 ----------------
#define DAP_SWD_CLK_PIN        GPIO_NUM_36   // SWCLK
#define DAP_SWD_DIO_PIN        GPIO_NUM_38   // SWDIO (双向)

// ---------------- SWD/JTAG 位带时序校准 ----------------
// half_period_delay() 已强制内联。半周期耗时 = 翻转/循环开销 + 忙等 cycles。
// 两个常量分别用于 fast 档判定与 slow 档补偿，需按 CPU 主频实测微调（示波器抓 SWCLK）。
//   DAP_FAST_OVERHEAD_CYCLES：fast 档（6 个 nop，无忙等）半周期的固定开销。
//   DAP_SLOW_OVERHEAD_CYCLES：slow 档（有忙等）半周期中“非忙等部分”的固定开销。
//                            slow 档忙等 cycles = 目标半周期 - 该值。
// 校准法：先测 fast 档实际频率 f_fast，则 FAST ≈ CPU_MHz/(2*f_fast)；
//         再测任一 slow 档请求（如 2MHz），若实测偏低则增大 SLOW、偏高则减小 SLOW。
#define DAP_FAST_OVERHEAD_CYCLES   24
#define DAP_SLOW_OVERHEAD_CYCLES   24

// ---------------- JTAG 引脚定义 ----------------
#define DAP_JTAG_TCK_PIN       GPIO_NUM_36   // 与 SWCLK 共用
#define DAP_JTAG_TMS_PIN       GPIO_NUM_38   // 与 SWDIO 共用
#define DAP_JTAG_TDI_PIN       GPIO_NUM_34  // 原 GPIO6 与板载 LED_ACT 冲突，改用此引脚
#define DAP_JTAG_TDO_PIN       GPIO_NUM_9

// ---------------- 公共控制引脚 ----------------
#define DAP_NRESET_PIN         GPIO_NUM_35  // 目标复位 (开漏, 低有效)
// ---------------- 状态指示灯（低电平点亮，由 status_led 全局任务统一驱动）----------------
// 红灯：连接模式 —— USB 模式常亮；Wi-Fi 模式搜索中快闪、连接过程中慢闪、拿到 IP 后常亮
// 紫灯：DAP 状态 —— 目标断开熄灭、已连接常亮、运行中快闪
#define DAP_LED_RED_PIN       GPIO_NUM_6   // 连接模式指示灯
#define DAP_LED_PURPLE_PIN    GPIO_NUM_8   // DAP 状态指示灯

// 是否使用 nTRST（多数 Cortex-M 目标不需要）
#define DAP_JTAG_HAS_TRST      1
#define DAP_JTAG_TRST_PIN      GPIO_NUM_33

// ---------------- 板载状态灯（低电平点亮）----------------
//#define BOARD_LED_PWR_PIN      GPIO_NUM_1   // 上电常亮，表示程序已开始运行
//#define BOARD_LED_ACT_PIN      GPIO_NUM_2   // 运行时 0.5s 间隔闪烁，表示主循环存活

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
