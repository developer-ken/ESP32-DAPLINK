// USB 设备/配置/HID 描述符 + HID 类回调（CMSIS-DAP 走 HID Vendor collection，64 字节 IN/OUT）
#include "usb_descriptors.h"
#include "dap_config.h"
#include "dap.h"
#include <string.h>

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_HID_OUT   0x01
#define EPNUM_HID_IN    0x81
#define EPNUM_MSC_OUT   0x02
#define EPNUM_MSC_IN    0x82

#define HID_EP_SIZE     DAP_PACKET_SIZE
#define MSC_EP_SIZE     64

// ---------------- 设备描述符 ----------------
tusb_desc_device_t const g_usb_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = DAP_USB_VID,
    .idProduct          = DAP_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// ---------------- HID 报告描述符（CMSIS-DAP 厂商自定义） ----------------
static uint8_t const s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,       // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,             // Usage (0x01)
    0xA1, 0x01,             // Collection (Application)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, HID_EP_SIZE,      //   Report Count
    0x09, 0x01,             //   Usage (0x01)
    0x81, 0x02,             //   Input (Data,Var,Abs)
    0x95, HID_EP_SIZE,      //   Report Count
    0x09, 0x01,             //   Usage (0x01)
    0x91, 0x02,             //   Output (Data,Var,Abs)
    0xC0                    // End Collection
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf)
{
    (void)itf;
    return s_hid_report_descriptor;
}

// ---------------- 配置描述符（HID + MSC 复合设备） ----------------
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const g_usb_fs_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                              sizeof(s_hid_report_descriptor),
                              EPNUM_HID_OUT, EPNUM_HID_IN, HID_EP_SIZE, 1),

    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE),
};

// ---------------- 字符串描述符 ----------------
char const *g_usb_string_descriptor[] = {
    (const char[]){0x09, 0x04}, // 0: 语言 ID (English, 0x0409)
    DAP_USB_MANUFACTURER,       // 1
    DAP_USB_PRODUCT,            // 2
    "EGGY0001",                 // 3: 序列号
    "Eggy CMSIS-DAP HID",       // 4
    "Eggy Storage",             // 5
};
const size_t g_usb_string_descriptor_count =
    sizeof(g_usb_string_descriptor) / sizeof(g_usb_string_descriptor[0]);

// ---------------- HID 类回调：所有 CMSIS-DAP 命令通过 OUT 报告进入，处理后经 IN 报告返回 ----------------
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    (void)itf; (void)report_id; (void)report_type;
    uint8_t resp[DAP_PACKET_SIZE];
    uint16_t len = dap_process_command(buffer, bufsize, resp);
    tud_hid_report(0, resp, len);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)itf; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
