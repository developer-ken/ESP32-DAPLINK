#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 挂载/首次格式化 1MB storage 分区，并注册 USB MSC（只读）后端
esp_err_t msc_disk_init(void);

#ifdef __cplusplus
}
#endif
