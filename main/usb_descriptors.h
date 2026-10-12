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

// 初始化 CMSIS-DAP v2 Bulk 相关回调绑定（如 SWO 流式输出），需在 USB 枚举前调用一次
void usb_dap_init(void);

// 根据芯片 eFuse 出厂 MAC 生成唯一 USB 序列号，需在 tinyusb_driver_install 之前调用
void usb_descriptors_init(void);

// 返回生成的序列号字符串（"EGGY" + 12 位十六进制 MAC），供 USB 描述符与 DAP_Info 复用
const char *usb_serial_number(void);

#ifdef __cplusplus
}
#endif
