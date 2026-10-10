// 引脚与设备参数集中配置：修改这里即可适配不同硬件走线
#pragma once

#include "driver/gpio.h"

// ---------------- SWD 引脚定义 ----------------
#define DAP_SWD_CLK_PIN        GPIO_NUM_36   // SWCLK
#define DAP_SWD_DIO_PIN        GPIO_NUM_38   // SWDIO (双向)

// ---------------- SWD/JTAG 位带时序 ----------------
// 半周期实际开销不再写死：dap_io_init() 会运行时校准出“零延时半周期”的真实 cycles，
// 之后 dap_io_set_clock() 用该值精确补偿，保证请求频率准确（不再依赖手工估的常量）。
//
// DAP_MIN_HALF_PERIOD_CYCLES：半周期的最短下限（即最高频率的硬上限）。
// 必须给足 SWD 读的建立时间：目标在 SWCLK 下降沿驱动 SWDIO，主机在上升沿前采样，
// 半周期太短会导致 ACK/IDR 读成 JUNK。240MHz 下：
//   12 cycles ≈ 50ns 半周期 ≈ 10MHz（激进，需示波器确认目标能跟上）
//   24 cycles ≈ 100ns 半周期 ≈ 5MHz（保守，稳定）
#define DAP_MIN_HALF_PERIOD_CYCLES  12

// ---------------- JTAG 引脚定义 ----------------
#define DAP_JTAG_TCK_PIN       GPIO_NUM_36   // 与 SWCLK 共用
#define DAP_JTAG_TMS_PIN       GPIO_NUM_38   // 与 SWDIO 共用
#define DAP_JTAG_TDI_PIN       GPIO_NUM_34  // 原 GPIO6 与板载 LED_ACT 冲突，改用此引脚
#define DAP_JTAG_TDO_PIN       GPIO_NUM_9

// ---------------- SWO 引脚定义 ----------------
// SWO（Serial Wire Output）复用 TDO 引脚：SWO 属于 SWD 的 trace 输出，JTAG 的 TDO 不会同时使用
#define DAP_SWO_PIN            GPIO_NUM_9    // 与 TDO 共用同一根物理引脚
#define DAP_SWO_UART_NUM       1             // 接收 SWO 数据的 UART 外设（UART1）

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
#define WIFI_DEBUG_CMSIS_DAP_PORT 3333           // openocd cmsis-dap backend tcp 端口

// USB 是否成功挂载的判定超时时间 (ms)
#define USB_MOUNT_WAIT_MS         3000

// 2.75MB 只读存储分区在 partitions.csv 中的名字
#define STORAGE_PARTITION_LABEL   "storage"
