// SWO (Serial Wire Output) trace 数据捕获接口
// 使用 ESP32-S3 硬件 UART 接收目标 TPIU 输出的 UART 编码（异步 NRZ 8N1）trace 流，
// 通过 GPIO 矩阵把 SWO 引脚（复用 TDO）路由到 UART RX。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// SWO 传输通道（DAP_SWO_Transport 0x17）
#define SWO_TRANSPORT_NONE    0
#define SWO_TRANSPORT_DATA    1   // 经 DAP_SWO_Data 命令轮询
#define SWO_TRANSPORT_WINUSB  2   // USB Bulk 流式（WinUSB 端点，无头原始字节流）

// SWO 捕获模式（DAP_SWO_Mode 0x18）
#define SWO_MODE_OFF          0
#define SWO_MODE_UART         1
#define SWO_MODE_MANCHESTER   2   // 未实现

// trace 状态位（DAP_SWO_Status / DAP_SWO_Data 返回）
#define SWO_STATUS_ACTIVE       (1u << 0)
#define SWO_STATUS_STREAM_ERROR (1u << 6)
#define SWO_STATUS_OVERRUN      (1u << 7)

// SWO 接收环形缓冲大小（同时通过 DAP_Info 0xFD 上报给主机）
#define SWO_BUFFER_SIZE         8192

void swo_init(void);

bool swo_set_transport(uint8_t transport);          // 返回是否支持该通道
uint8_t swo_get_transport(void);                    // 当前传输通道
bool swo_set_mode(uint8_t mode);                    // 返回是否支持该模式
uint32_t swo_set_baudrate(uint32_t baudrate);       // 返回实际波特率（0=失败）
void swo_control(bool start);                       // 启动/停止捕获
uint16_t swo_read(uint8_t *buf, uint16_t max_count);// 读 trace 数据，返回实际字节数
uint32_t swo_get_buffered_count(void);              // 缓冲中未读字节数
uint8_t swo_get_status(void);                       // trace 状态字节

// SWO 流式输出回调（SWO_TRANSPORT_WINUSB 时，捕获数据持续经此回调送出）。
// 由 USB Bulk 层注册，回调内把 trace 数据写入 Bulk IN 端点。
typedef void (*swo_stream_sink_t)(const uint8_t *data, uint16_t len);
void swo_set_stream_sink(swo_stream_sink_t sink);

#ifdef __cplusplus
}
#endif
