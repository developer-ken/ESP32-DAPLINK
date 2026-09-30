// 上电流程：优先尝试作为 USB 复合设备(CMSIS-DAP + 只读U盘)运行；
// 若一段时间内未被电脑枚举成功，则回退到 Wi-Fi 调试桥模式
#include <cstdio>
#include "dap_config.h"
#include "dap.h"
#include "status_led.h"
#include "usb_descriptors.h"
#include "msc_disk.h"
#include "wifi_bridge.h"
#include "nvs_flash.h"
#include "tinyusb_default_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 指示灯由全局 status_led 任务统一驱动（低电平点亮）：
//   红灯：连接模式 —— USB 常亮；Wi-Fi 搜索中快闪、连接中慢闪、拿到 IP 常亮
//   紫灯：DAP 状态 —— 目标断开熄灭、已连接常亮、运行中快闪（由 dap.c / remote_bitbang.c 维护）
extern "C" void app_main(void)
{
    status_led_init();
    status_led_set(LED_ID_RED, LED_MODE_OFF); // 尝试建立 USB 连接中

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
        status_led_set(LED_ID_RED, LED_MODE_ON); // USB 模式：红灯常亮
    } else {
        ESP_LOGW(TAG, "USB 未连接，切换到 Wi-Fi 调试桥");
        while (!wifi_bridge_start()){
            ESP_LOGW(TAG, "WI-FI连接失败，1s后重试...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        };
    }

    while (1) {
        // 指示灯由 status_led 任务维护，主任务只需保持存活
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
