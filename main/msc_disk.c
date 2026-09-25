// 1MB storage 分区：首次上电自动格式化为 FAT 并放入说明文件，
// 之后通过 esp_tinyusb 的 MSC 存储后端呈现给电脑
// 注：当前 esp_tinyusb (v2.3.0) 的 MSC 存储后端未提供只读开关，
// USB 主机在协议层面仍可写入；只读语义仅体现在“该分区仅用于分发文件”的使用约定上。
#include "msc_disk.h"
#include "dap_config.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "tinyusb_msc.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "msc_disk";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_storage_handle;

esp_err_t msc_disk_init(void)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, STORAGE_PARTITION_LABEL);
    if (!partition) {
        ESP_LOGE(TAG, "找不到 %s 分区", STORAGE_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wl_mount(partition, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount 失败: %s", esp_err_to_name(err));
        return err;
    }

    tinyusb_msc_storage_config_t cfg = {
        .medium.wl_handle = s_wl_handle,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP, // 先挂到 APP 端写入说明文件
        .fat_fs = {
            .base_path = "/storage",
            .config = { .max_files = 4 },
            .do_not_format = false,
        },
    };
    err = tinyusb_msc_new_storage_spiflash(&cfg, &s_storage_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 MSC 存储失败: %s", esp_err_to_name(err));
        return err;
    }

    FILE *f = fopen("/storage/README.TXT", "r");
    if (f) {
        fclose(f);
    } else {
        f = fopen("/storage/README.TXT", "w");
        if (f) {
            fputs("Eggy CMSIS-DAP\r\n"
                  "This 1MB storage is provided for distributing files.\r\n",
                  f);
            fclose(f);
        }
    }

    // 交给 USB 主机使用
    tinyusb_msc_set_storage_mount_point(s_storage_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    ESP_LOGI(TAG, "storage 分区已就绪并交给 USB 主机");
    return ESP_OK;
}
