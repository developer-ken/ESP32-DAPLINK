// 状态指示灯统一管理：把“灯怎么亮”从各业务模块里抽出来。
// 业务层只负责用 status_led_set() 描述期望状态（熄灭/慢闪/快闪/常亮），
// 由唯一的全局任务 status_led_task 按统一节拍驱动所有受管 LED（低电平点亮）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 受管理的指示灯（引脚见 dap_config.h 的 DAP_LED_*_PIN）
typedef enum {
    LED_ID_RED = 0,   // 红色：连接模式 —— USB 常亮；Wi-Fi 搜索=快闪、连接中=慢闪、拿到 IP=常亮
    LED_ID_PURPLE,    // 紫色：DAP 状态 —— 目标断开=熄灭，已连接=常亮，运行中=快闪
    LED_ID_COUNT
} led_id_t;

// 指示灯状态
typedef enum {
    LED_MODE_OFF = 0,    // 熄灭
    LED_MODE_ON,         // 常亮
    LED_MODE_SLOW_BLINK, // 慢闪（约 2Hz）
    LED_MODE_FAST_BLINK  // 快闪（约 5Hz）
} led_mode_t;

// 配置 LED 引脚并启动全局状态灯任务（在 app_main 中调用一次即可）
void status_led_init(void);

// 设置某盏灯的期望状态，可在任意任务/上下文调用，下一个节拍生效
void status_led_set(led_id_t id, led_mode_t mode);

// 读取某盏灯当前设置的状态
led_mode_t status_led_get(led_id_t id);

#ifdef __cplusplus
}
#endif
