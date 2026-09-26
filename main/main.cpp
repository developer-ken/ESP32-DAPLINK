// 上电流程：优先尝试作为 USB 复合设备(CMSIS-DAP + 只读U盘)运行；
// 若一段时间内未被电脑枚举成功，则回退到 Wi-Fi 调试桥模式
#include <cstdio>
#include "dap_config.h"
#include "dap.h"
#include "usb_descriptors.h"
#include "msc_disk.h"
#include "wifi_bridge.h"
#include "nvs_flash.h"
#include "tinyusb_default_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// LED_PWR/LED_ACT 均为低电平点亮
static void board_leds_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << BOARD_LED_PWR_PIN) | (1ULL << BOARD_LED_ACT_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    gpio_set_level(BOARD_LED_PWR_PIN, 1); // 先熄灭，初始化完成后再点亮
    gpio_set_level(BOARD_LED_ACT_PIN, 1);
}

extern "C" void app_main(void)
{
    board_leds_init();
    gpio_set_level(BOARD_LED_PWR_PIN, 0); // 程序已开始运行：常亮

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    dap_init();
    msc_disk_init();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &g_usb_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = g_usb_fs_config_descriptor;
    tusb_cfg.descriptor.string = g_usb_string_descriptor;
    tusb_cfg.descriptor.string_count = g_usb_string_descriptor_count;

    esp_err_t usb_err = tinyusb_driver_install(&tusb_cfg);

    bool usb_ok = false;
    if (usb_err == ESP_OK) {
        for (int waited = 0; waited < USB_MOUNT_WAIT_MS; waited += 100) {
            if (tud_mounted()) {
                usb_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else {
        ESP_LOGE(TAG, "USB 驱动安装失败: %s", esp_err_to_name(usb_err));
    }

    if (usb_ok) {
        ESP_LOGI(TAG, "USB 已连接：以 \"%s\" 运行 CMSIS-DAP + 只读U盘", DAP_USB_PRODUCT);
    } else {
        ESP_LOGW(TAG, "USB 未连接，切换到 Wi-Fi 调试桥");
        if (!wifi_bridge_start()) {
            ESP_LOGE(TAG, "Wi-Fi 调试桥启动失败，请检查热点/密码");
        }
    }

    while (1) {
        gpio_set_level(BOARD_LED_ACT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(BOARD_LED_ACT_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
