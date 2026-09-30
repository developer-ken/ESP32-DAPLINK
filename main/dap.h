// CMSIS-DAP 命令处理：解析 USB HID / 网络远程调试收到的命令包，驱动 swd_jtag 完成实际时序
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAP_PACKET_SIZE   64
#define DAP_PACKET_COUNT  1

void dap_init(void);

// DAP 状态灯（紫灯）：目标连接/运行状态，供非 CMSIS-DAP 命令路径（如 Wi-Fi remote_bitbang）复用
void dap_led_set_connected(bool connected);
void dap_led_set_running(bool running);

// 处理一个 CMSIS-DAP 命令包：req 输入，resp 输出，返回 resp 有效长度
uint16_t dap_process_command(const uint8_t *req, uint16_t req_len, uint8_t *resp);

// 调整 DAP_Info(0xFF/0xFE) 上报的包大小与包缓冲数量。
// USB HID 保持默认 64/1；TCP 传输可调大（如 1024/4）以减少网络往返次数、提升吞吐。
void dap_set_packet_size(uint16_t packet_size);
void dap_set_packet_count(uint8_t packet_count);

#ifdef __cplusplus
}
#endif
