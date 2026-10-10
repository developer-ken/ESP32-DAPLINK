// SWD / JTAG 底层 GPIO 位带（bit-bang）驱动
// 只负责电平与时序，协议层（CMSIS-DAP 命令、SWD 传输帧、JTAG TAP 状态机）在 dap.c 中实现
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAP_PORT_DISABLED  0
#define DAP_PORT_SWD       1
#define DAP_PORT_JTAG      2

void dap_io_init(void);
void dap_io_deinit(void);

// 连接/断开指定调试口，会配置引脚方向与上下拉
void dap_io_connect(uint8_t port);
void dap_io_disconnect(void);

// 设置 SWCLK/TCK 频率（Hz），内部换算为每半周期的延时
void dap_io_set_clock(uint32_t clock_hz);

// DAP_SWJ_Pins：读取/写入原始引脚电平
// bit0=SWCLK/TCK bit1=SWDIO/TMS bit2=TDI bit3=TDO bit5=nTRST bit7=nRESET
uint8_t dap_io_get_swj_pins(void);
void dap_io_set_swj_pins(uint8_t value, uint8_t select);

// 在 SWDIO/TMS 线上以 SWJ 方式移出 count 个位（用于行复位/切换序列），line 由当前 port 决定
void dap_io_swj_sequence(uint32_t count, const uint8_t *data);

// ---------------- SWD ----------------
void dap_io_swd_configure(uint8_t turnaround_cycles);

// ---------------- SWD 帧传输（SPI 后端，仅 DAP_PORT_SWD 有效）----------------
// 用 SPI 半双工把一次 SWD 帧的“写请求→转向→读ACK→读写数据”压缩为 2~3 次事务，
// 避免逐 bit 的 write_bits/read_bits 带来的事务启动开销。内部已处理转向与 idle 收尾，
// 协议层无需再显式调用 dio_to_* / turnaround。
uint8_t  dap_io_swd_req_ack(uint32_t request_packet);    // 写 8bit 请求 + 转向 + 读 3bit ACK，返回 3bit ACK
uint64_t dap_io_swd_read_phase(void);                     // 读 33bit(32data+1parity) + 转向（读→写），返回 (parity<<32)|data
void     dap_io_swd_write_phase(uint32_t data, uint32_t parity); // 转向（读→写）+ 写 32data + 1parity
void     dap_io_swd_idle(uint8_t idle_cycles);            // idle cycles 输出 0 + idle 高收尾
void     dap_io_swd_finish(void);                         // WAIT/FAULT 收尾：转向输出 + idle 高
void     dap_io_swd_drain(void);                          // 协议错误收尾：读 33bit 吸收 + 转向输出 + idle 高

// ---------------- JTAG ----------------
// 单个 TCK 周期：先给出 tms/tdi，再采样 tdo，返回 tdo 电平(0/1)
uint8_t dap_io_jtag_clock(uint8_t tms, uint8_t tdi);

// 直接设置 TCK/TMS/TDI 电平，不自带时钟脉冲（供 remote_bitbang 这类逐比特协议使用）
void dap_io_jtag_set_pins(uint8_t tck, uint8_t tms, uint8_t tdi);
uint8_t dap_io_jtag_get_tdo(void);

// ---------------- 复位控制 ----------------
void dap_io_set_nreset(bool asserted); // true = 拉低复位目标
void dap_io_set_ntrst(bool asserted);

#ifdef __cplusplus
}
#endif
