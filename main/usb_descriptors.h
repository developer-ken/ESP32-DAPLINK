#pragma once
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB 设备/配置/字符串描述符，供 tinyusb_config_t.descriptor 引用
extern tusb_desc_device_t const g_usb_device_descriptor;
extern uint8_t const g_usb_fs_config_descriptor[];
extern char const *g_usb_string_descriptor[];
extern const size_t g_usb_string_descriptor_count;

#ifdef __cplusplus
}
#endif
