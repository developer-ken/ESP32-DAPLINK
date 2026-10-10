// USB 设备描述符与 CMSIS-DAP vendor bulk 收发回调
#include "usb_descriptors.h"
#include "dap_config.h"
#include "dap.h"
#include "swo.h"
#include <string.h>

enum {
    ITF_NUM_HID = 0,
    ITF_NUM_VENDOR,   // CMSIS-DAP v2 (WinUSB Bulk)
    ITF_NUM_MSC,
    ITF_NUM_TOTAL
};

#define EPNUM_HID_OUT     0x01
#define EPNUM_HID_IN      0x81
#define EPNUM_VENDOR_OUT  0x02
#define EPNUM_VENDOR_IN   0x82
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83

#define HID_EP_SIZE       DAP_PACKET_SIZE
#define VENDOR_EP_SIZE    64   // 全速 Bulk 端点最大包长
#define MSC_EP_SIZE       64

// ---------------- 设备描述符 ----------------
tusb_desc_device_t const g_usb_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210, // USB 2.1：需支持 BOS 描述符（供 WinUSB / MS OS 2.0 使用）
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

// ---------------- HID Report Descriptor ----------------
static uint8_t const s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, HID_EP_SIZE,
    0x09, 0x01,
    0x81, 0x02,
    0x95, HID_EP_SIZE,
    0x09, 0x01,
    0x91, 0x02,
    0xC0
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// ---------------- 配置描述符（CMSIS-DAP HID v1 + WinUSB Bulk v2 + MSC） ----------------
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const g_usb_fs_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                              sizeof(s_hid_report_descriptor),
                              EPNUM_HID_OUT, EPNUM_HID_IN, HID_EP_SIZE, 1),

    // CMSIS-DAP v2：WinUSB 厂商自定义接口（Bulk），由 MS OS 2.0 描述符绑定 WinUSB 驱动
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 6, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, VENDOR_EP_SIZE),

    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE),
};

// ---------------- BOS + Microsoft OS 2.0 描述符（WinUSB 绑定） ----------------
#define VENDOR_REQUEST_MICROSOFT  1

#define BOS_TOTAL_LEN       (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN   0xB2

uint8_t const s_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_descriptor;
}

// MS OS 2.0 描述符集：把 WinUSB 兼容 ID 绑定到 Vendor(Bulk) 接口，并注册设备接口 GUID
uint8_t const s_ms_os_20_descriptor[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, config total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset header: length, type, first interface, reserved, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // Compatible ID descriptor: length, type, compatible ID ("WINUSB"), sub compatible ID
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property descriptor: length, type
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType(REG_MULTI_SZ), wPropertyNameLength, "DeviceInterfaceGUIDs\0"
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
    'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    // bPropertyData: "{3D0A7A5E-2A41-4B6F-9E6C-1F2D3A4B5C6D}\0\0" (UTF-16, REG_MULTI_SZ)
    '{', 0x00, '3', 0x00, 'D', 0x00, '0', 0x00, 'A', 0x00, '7', 0x00, 'A', 0x00, '5', 0x00, 'E', 0x00, '-', 0x00,
    '2', 0x00, 'A', 0x00, '4', 0x00, '1', 0x00, '-', 0x00, '4', 0x00, 'B', 0x00, '6', 0x00, 'F', 0x00, '-', 0x00,
    '9', 0x00, 'E', 0x00, '6', 0x00, 'C', 0x00, '-', 0x00, '1', 0x00, 'F', 0x00, '2', 0x00, 'D', 0x00, '3', 0x00,
    'A', 0x00, '4', 0x00, 'B', 0x00, '5', 0x00, 'C', 0x00, '6', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_descriptor) == MS_OS_20_DESC_LEN, "Incorrect MS OS 2.0 descriptor size");

// 处理厂商类控制请求（此处用于应答 MS OS 2.0 描述符请求）
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {
        uint16_t total_len;
        memcpy(&total_len, s_ms_os_20_descriptor + 8, 2);
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_ms_os_20_descriptor, total_len);
    }

    return false; // 不支持的请求 -> STALL
}

// ---------------- 字符串描述符 ----------------
char const *g_usb_string_descriptor[] = {
    (const char[]){0x09, 0x04}, // 0: 语言 ID (English, 0x0409)
    DAP_USB_MANUFACTURER,       // 1
    DAP_USB_PRODUCT,            // 2
    "EGGY0001",                 // 3: 序列号
    "Eggy CMSIS-DAP HID",       // 4
    "Eggy Storage",             // 5
    "Eggy CMSIS-DAP v2",        // 6: Bulk (WinUSB) 接口
};
const size_t g_usb_string_descriptor_count =
    sizeof(g_usb_string_descriptor) / sizeof(g_usb_string_descriptor[0]);

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    (void)report_type;
    if (instance != 0 || bufsize == 0 || bufsize > DAP_PACKET_SIZE) {
        return;
    }

    uint8_t response[DAP_PACKET_SIZE] = {0};
    dap_process_command(buffer, bufsize, response);
    tud_hid_report(0, response, sizeof(response));
}

// ---------------- CMSIS-DAP v2 Bulk 命令处理 ----------------
// 缓冲模式（CFG_TUD_VENDOR_RX_BUFSIZE > 0）下，收到一条完整 Bulk OUT 传输时回调一次，
// 命令数据在 RX FIFO 中，用 tud_vendor_read 读出；处理后写回 TX FIFO 并 flush 发出响应。
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (itf != 0) {
        return;
    }

    uint32_t available = tud_vendor_available();
    if (available == 0) {
        return;
    }

    uint8_t req[DAP_PACKET_SIZE] = {0};
    uint32_t req_len = tud_vendor_read(req, sizeof(req));
    if (req_len == 0) {
        return;
    }

    uint8_t response[DAP_PACKET_SIZE] = {0};
    uint16_t resp_len = dap_process_command(req, req_len, response);
    if (resp_len == 0) {
        return;
    }

    tud_vendor_write(response, resp_len);
    tud_vendor_write_flush();
}

// ---------------- CMSIS-DAP v2 SWO 流式输出 ----------------
// 由 swo.c 的流式任务回调，把捕获到的 trace 原始字节持续写入 Bulk IN 端点。
static void usb_dap_swo_stream_sink(const uint8_t *data, uint16_t len)
{
    if (len == 0 || !tud_vendor_mounted()) {
        return;
    }
    tud_vendor_write(data, len);
    tud_vendor_write_flush();
}

// 初始化 Bulk 相关的回调绑定（在 USB 枚举前调用一次即可）
void usb_dap_init(void)
{
    swo_set_stream_sink(usb_dap_swo_stream_sink);
}

