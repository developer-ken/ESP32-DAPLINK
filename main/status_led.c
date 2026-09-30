// 全局状态灯任务：所有 LED 的亮灭时序都集中在这里，业务模块只写状态变量
#include "status_led.h"
#include "dap_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 统一节拍：所有闪烁都以该周期为基准
#define LED_TICK_MS     50
#define LED_SLOW_TICKS  (250 / LED_TICK_MS) // 慢闪：250ms 翻转一次（约 2Hz）
#define LED_FAST_TICKS  (100 / LED_TICK_MS) // 快闪：100ms 翻转一次（约 5Hz）

static const gpio_num_t s_pins[LED_ID_COUNT] = {
    [LED_ID_RED] = DAP_LED_RED_PIN,
    [LED_ID_PURPLE] = DAP_LED_PURPLE_PIN,
};

// 各灯的期望状态（volatile：其它任务/中断可直接写入）
static volatile led_mode_t s_mode[LED_ID_COUNT] = {LED_MODE_OFF, LED_MODE_OFF};

// 低电平点亮
static inline void led_write(led_id_t id, bool on)
{
    gpio_set_level(s_pins[id], on ? 0 : 1);
}

// 单个节拍内某盏灯是否需要点亮
static bool led_level_for(led_mode_t mode, uint32_t tick)
{
    switch (mode) {
    case LED_MODE_ON:
        return true;
    case LED_MODE_SLOW_BLINK:
        return ((tick / LED_SLOW_TICKS) & 1u) == 0;
    case LED_MODE_FAST_BLINK:
        return ((tick / LED_FAST_TICKS) & 1u) == 0;
    case LED_MODE_OFF:
    default:
        return false;
    }
}

static void status_led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    for (;;) {
        for (int i = 0; i < LED_ID_COUNT; i++) {
            led_write((led_id_t)i, led_level_for(s_mode[i], tick));
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

void status_led_init(void)
{
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pin_bit_mask = (1ULL << DAP_LED_RED_PIN) | (1ULL << DAP_LED_PURPLE_PIN);
    gpio_config(&io_conf);

    for (int i = 0; i < LED_ID_COUNT; i++) {
        s_mode[i] = LED_MODE_OFF;
        led_write((led_id_t)i, false);
    }

    xTaskCreate(status_led_task, "status_led", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void status_led_set(led_id_t id, led_mode_t mode)
{
    if ((int)id < 0 || id >= LED_ID_COUNT) return;
    s_mode[id] = mode;
}

led_mode_t status_led_get(led_id_t id)
{
    if ((int)id < 0 || id >= LED_ID_COUNT) return LED_MODE_OFF;
    return s_mode[id];
}
