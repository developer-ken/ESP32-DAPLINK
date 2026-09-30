#include "dap.h"
#include "swd_jtag.h"
#include "dap_config.h"
#include "status_led.h"
#include "esp_rom_sys.h"
#include <string.h>

// ---------------- CMSIS-DAP 命令编号 ----------------
#define ID_DAP_INFO                 0x00
#define ID_DAP_HOST_STATUS          0x01
#define ID_DAP_CONNECT               0x02
#define ID_DAP_DISCONNECT            0x03
#define ID_DAP_TRANSFER_CONFIGURE    0x04
#define ID_DAP_TRANSFER              0x05
#define ID_DAP_TRANSFER_BLOCK        0x06
#define ID_DAP_TRANSFER_ABORT        0x07
#define ID_DAP_WRITE_ABORT           0x08
#define ID_DAP_DELAY                 0x09
#define ID_DAP_RESET_TARGET          0x0A
#define ID_DAP_SWJ_PINS              0x10
#define ID_DAP_SWJ_CLOCK             0x11
#define ID_DAP_SWJ_SEQUENCE          0x12
#define ID_DAP_SWD_CONFIGURE         0x13
#define ID_DAP_JTAG_SEQUENCE         0x14
#define ID_DAP_JTAG_CONFIGURE        0x15
#define ID_DAP_JTAG_IDCODE           0x16

#define DAP_OK                       0x00
#define DAP_ERROR                    0xFF

#define DAP_TRANSFER_OK              0x01
#define DAP_TRANSFER_WAIT            0x02
#define DAP_TRANSFER_FAULT           0x04
#define DAP_TRANSFER_ERROR           0x08
#define DAP_TRANSFER_MISMATCH        0x10

// SWD request bit fields (host -> target 8bit packet, 不含 start/park/parity 已由 build_swd_request 处理)
#define SWD_REQ_APnDP  (1u << 0)
#define SWD_REQ_RnW    (1u << 1)
#define SWD_REQ_A2     (1u << 2)
#define SWD_REQ_A3     (1u << 3)

// DP RDBUFF 寄存器（APnDP=0,RnW=1,A[3:2]=0b11）：用于冲刷 AP 寄存器读的流水线结果
#define DP_RDBUFF_REQ  (SWD_REQ_A3 | SWD_REQ_A2 | SWD_REQ_RnW)

// JTAG-DP IR 指令
#define JTAG_IR_DPACC  0xA
#define JTAG_IR_APACC  0xB

#define MAX_JTAG_DEVICES 4

static uint8_t s_port = DAP_PORT_DISABLED;
static uint8_t s_transfer_idle_cycles = 0;
static uint16_t s_transfer_wait_retry = 100;
static uint16_t s_transfer_match_retry = 0;

static uint8_t s_jtag_count = 0;
static uint8_t s_jtag_ir_len[MAX_JTAG_DEVICES] = {4, 4, 4, 4};
static uint8_t s_jtag_index = 0;

// ---- 状态灯 ----
// 灯的亮灭由全局 status_led 任务统一驱动（见 status_led.h），协议层只描述期望状态：
//   LED_ID_PURPLE (DAP_LED_PURPLE_PIN) -> DAP 状态：目标断开=熄灭，已连接=常亮，运行中=快闪
//   LED_ID_RED    (DAP_LED_RED_PIN)    -> 连接模式（USB/Wi-Fi），由 main/wifi_bridge 维护，协议层不碰
static bool s_dap_connected = false;   // 目标是否已通过 DAP_CONNECT 连接
static bool s_dap_running   = false;   // 目标是否处于运行态（HOST_STATUS type=1）

// 依据“已连接/运行中”两个状态刷新紫灯
static void dap_led_refresh(void)
{
    if (!s_dap_connected) {
        status_led_set(LED_ID_PURPLE, LED_MODE_OFF);        // 目标断开：熄灭
    } else if (s_dap_running) {
        status_led_set(LED_ID_PURPLE, LED_MODE_FAST_BLINK); // 运行中：快闪
    } else {
        status_led_set(LED_ID_PURPLE, LED_MODE_ON);         // 已连接：常亮
    }
}

void dap_led_set_connected(bool connected)
{
    s_dap_connected = connected;
    if (!connected) s_dap_running = false;
    dap_led_refresh();
}

void dap_led_set_running(bool running)
{
    s_dap_running = running;
    dap_led_refresh();
}

void dap_init(void)
{
    dap_io_init();
    s_port = DAP_PORT_DISABLED;
}

// ================= SWD 传输 =================

static uint8_t swd_parity32(uint32_t v)
{
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
}

// 执行一次 SWD 传输，request 为 APnDP/RnW/A2/A3 组合，*data 用于读/写数据
// 返回 3bit ACK（DAP_TRANSFER_OK/WAIT/FAULT），收尾逻辑对齐 ARM 官方 SW_DP.c 参考实现
static uint8_t swd_transfer(uint8_t request, uint32_t *data)
{
    uint8_t parity = ((request & SWD_REQ_APnDP) != 0) + ((request & SWD_REQ_RnW) != 0) +
                     ((request & SWD_REQ_A2) != 0) + ((request & SWD_REQ_A3) != 0);
    parity &= 1;

    uint8_t packet = 0x81; // start=1(bit0), park=1(bit7)
    packet |= (request & 0x0F) << 1;
    packet |= parity << 5;

    dap_io_swd_dio_to_output();
    dap_io_swd_write_bits(packet, 8);

    dap_io_swd_dio_to_input();
    dap_io_swd_turnaround();

    uint32_t ack = dap_io_swd_read_bits(3);

    if (ack == DAP_TRANSFER_OK) {
        if (request & SWD_REQ_RnW) {
            uint32_t value = dap_io_swd_read_bits(32);
            uint32_t par = dap_io_swd_read_bits(1);
            dap_io_swd_turnaround();
            dap_io_swd_dio_to_output();
            if (data) {
                *data = value;
            }
            if (swd_parity32(value) != (par & 1)) {
                ack = DAP_TRANSFER_ERROR;
            }
        } else {
            dap_io_swd_turnaround();
            dap_io_swd_dio_to_output();
            uint32_t value = data ? *data : 0;
            dap_io_swd_write_bits(value, 32);
            dap_io_swd_write_bits(swd_parity32(value), 1);
        }
        // 按主机通过 DAP_TransferConfigure 配置的 idle cycles 补齐（OpenOCD 默认配置为 0）
        if (s_transfer_idle_cycles) {
            dap_io_swd_write_bits(0, s_transfer_idle_cycles);
        }
        dap_io_swd_dio_idle_high();
        return (uint8_t)ack;
    }

    if (ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT) {
        // WAIT/FAULT：无数据相位，但 ACK 阶段是目标在驱动总线，不管读写都必须先转向才能切回输出，
        // 否则会在这一个 SWCLK 周期内跟目标抢总线，导致目标 SW-DP 协议错误、后续访问持续失败
        dap_io_swd_turnaround();
        dap_io_swd_dio_to_output();
        dap_io_swd_dio_idle_high();
        return (uint8_t)ack;
    }

    // 协议错误（ACK 既不是 OK/WAIT/FAULT，例如总线悬空读回的垃圾值）：目标可能仍以为自己
    // 处在 32+1 位数据相位里，多放空这段周期再收回总线，避免只转向 1 拍就抢线
    dap_io_swd_turnaround();
    dap_io_swd_read_bits(33);
    dap_io_swd_dio_to_output();
    dap_io_swd_dio_idle_high();
    return (uint8_t)ack;
}

// ================= JTAG 传输 =================

static void jtag_move(uint8_t tms_bits, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++) {
        dap_io_jtag_clock((tms_bits >> i) & 1, 0);
    }
}

// 复位 TAP 到 Run-Test/Idle
static void jtag_reset_to_idle(void)
{
    jtag_move(0x1F, 5); // 5 个 TMS=1 -> Test-Logic-Reset
    jtag_move(0x00, 1); // TMS=0 -> Run-Test/Idle
}

static void jtag_shift_ir(uint8_t device_index, uint8_t ir_value)
{
    uint32_t before = 0, after = 0;
    for (uint8_t i = 0; i < device_index; i++) before += s_jtag_ir_len[i];
    for (uint8_t i = device_index + 1; i < s_jtag_count; i++) after += s_jtag_ir_len[i];
    uint8_t len = s_jtag_ir_len[device_index];

    dap_io_jtag_clock(1, 0); // idle -> select-dr
    dap_io_jtag_clock(1, 0); // select-dr -> select-ir
    dap_io_jtag_clock(0, 0); // select-ir -> capture-ir
    dap_io_jtag_clock(0, 0); // capture-ir -> shift-ir

    for (uint32_t i = 0; i < before; i++) dap_io_jtag_clock(0, 1);
    for (uint8_t i = 0; i < len; i++) {
        bool last_bit = (i == (uint8_t)(len - 1)) && (after == 0);
        dap_io_jtag_clock(last_bit ? 1 : 0, (ir_value >> i) & 1);
    }
    for (uint32_t i = 0; i < after; i++) {
        dap_io_jtag_clock((i == after - 1) ? 1 : 0, 1);
    }

    dap_io_jtag_clock(1, 0); // exit1-ir -> update-ir
    dap_io_jtag_clock(0, 0); // update-ir -> idle
}

// 移入 bit_count 位（LSB 优先，最多 64 位）到 DR，同时捕获输出到 out（可为 NULL）
static void jtag_shift_dr(uint8_t device_index, uint64_t value, uint8_t bit_count, uint64_t *out)
{
    uint32_t before = device_index;                                    // 每个前置器件 bypass 寄存器为 1 位
    uint32_t after = s_jtag_count ? (s_jtag_count - device_index - 1) : 0;
    uint64_t captured = 0;

    dap_io_jtag_clock(1, 0); // idle -> select-dr
    dap_io_jtag_clock(0, 0); // select-dr -> capture-dr
    dap_io_jtag_clock(0, 0); // capture-dr -> shift-dr

    for (uint32_t i = 0; i < before; i++) dap_io_jtag_clock(0, 0);

    for (uint8_t i = 0; i < bit_count; i++) {
        bool last_bit = (i == (uint8_t)(bit_count - 1)) && (after == 0);
        uint8_t tdo = dap_io_jtag_clock(last_bit ? 1 : 0, (value >> i) & 1);
        if (tdo) captured |= ((uint64_t)1 << i);
    }
    for (uint32_t i = 0; i < after; i++) {
        dap_io_jtag_clock((i == after - 1) ? 1 : 0, 0);
    }

    dap_io_jtag_clock(1, 0); // exit1-dr -> update-dr
    dap_io_jtag_clock(0, 0); // update-dr -> idle

    if (out) *out = captured;
}

// JTAG-DP 寄存器访问：注意由于 JTAG-DP 是流水线结构，写操作返回的 ACK 对应“上一次”访问，
// 读操作通过额外一次 RDBUFF 访问把结果“冲刷”出来，因此读操作 ACK/数据是准确的，
// 写操作的即时 ACK 仅为近似值（与多数轻量级 CMSIS-DAP 实现一致的已知折衷）。
static uint8_t jtag_transfer(uint8_t request, uint32_t *data)
{
    uint8_t ir = (request & SWD_REQ_APnDP) ? JTAG_IR_APACC : JTAG_IR_DPACC;
    jtag_shift_ir(s_jtag_index, ir);

    uint64_t addr_rnw = ((request & SWD_REQ_A2) ? 0x2 : 0) | ((request & SWD_REQ_A3) ? 0x4 : 0) |
                        ((request & SWD_REQ_RnW) ? 0x1 : 0);
    uint64_t dr_in = addr_rnw | ((uint64_t)(data ? *data : 0) << 3);
    uint64_t dr_out = 0;
    jtag_shift_dr(s_jtag_index, dr_in, 35, &dr_out);
    uint8_t ack = dr_out & 0x7;

    if (request & SWD_REQ_RnW) {
        // 追加一次 RDBUFF 读，冲刷出真正的读结果
        uint64_t rdbuff_req = 0x4 | 0x1; // A[3:2]=11(RDBUFF), RnW=1
        jtag_shift_dr(s_jtag_index, rdbuff_req, 35, &dr_out);
        ack = dr_out & 0x7;
        if (data) *data = (uint32_t)(dr_out >> 3);
    }
    return ack;
}

static uint8_t do_transfer(uint8_t request, uint32_t *data)
{
    uint8_t ack;
    uint16_t retry = s_transfer_wait_retry ? s_transfer_wait_retry : 1;
    do {
        ack = (s_port == DAP_PORT_JTAG) ? jtag_transfer(request, data) : swd_transfer(request, data);
    } while (ack == DAP_TRANSFER_WAIT && --retry);
    return ack;
}

// ================= DAP_Info =================
static uint16_t dap_info(uint8_t id, uint8_t *resp)
{
    switch (id) {
    case 0x01: { const char *s = DAP_USB_MANUFACTURER; uint8_t n = strlen(s) + 1; resp[0] = n; memcpy(&resp[1], s, n); return n + 1; }
    case 0x02: { const char *s = DAP_USB_PRODUCT; uint8_t n = strlen(s) + 1; resp[0] = n; memcpy(&resp[1], s, n); return n + 1; }
    case 0x03: { const char *s = "EGGY0001"; uint8_t n = strlen(s) + 1; resp[0] = n; memcpy(&resp[1], s, n); return n + 1; }
    case 0x04: { const char *s = DAP_FW_VERSION; uint8_t n = strlen(s) + 1; resp[0] = n; memcpy(&resp[1], s, n); return n + 1; }
    case 0xF0: resp[0] = 1; resp[1] = 0x03; return 2; // SWD + JTAG 均支持
    case 0xFE: resp[0] = 1; resp[1] = DAP_PACKET_COUNT; return 2;
    case 0xFF: resp[0] = 2; resp[1] = DAP_PACKET_SIZE & 0xFF; resp[2] = (DAP_PACKET_SIZE >> 8) & 0xFF; return 3;
    default: resp[0] = 0; return 1;
    }
}

uint16_t dap_process_command(const uint8_t *req, uint16_t req_len, uint8_t *resp)
{
    if (req_len == 0) { resp[0] = ID_DAP_INFO; resp[1] = 0; return 2; }
    uint8_t cmd = req[0];
    uint16_t ri = 1, wi = 0;
    resp[wi++] = cmd;

    switch (cmd) {
    case ID_DAP_INFO: {
        uint8_t id = req[ri++];
        wi += dap_info(id, &resp[wi]);
        break;
    }
    case ID_DAP_HOST_STATUS: {
        uint8_t type = req[ri++];
        uint8_t status = req[ri++];
        if (type == 0) {
            // type=0 为主机侧调试会话状态，不影响紫灯（紫灯只反映目标连接/运行）
        } else {
            dap_led_set_running(status != 0); // 目标运行中 -> 紫灯快闪，停止 -> 回常亮/熄灭
        }
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_CONNECT: {
        uint8_t port = req[ri++];
        if (port == 0) port = DAP_PORT_SWD; // Default -> 优先 SWD
        dap_io_connect(port);
        s_port = port;
        dap_led_set_connected(true); // 目标已连接：紫灯常亮
        resp[wi++] = port;
        break;
    }
    case ID_DAP_DISCONNECT: {
        dap_io_disconnect();
        s_port = DAP_PORT_DISABLED;
        dap_led_set_connected(false); // 目标断开：紫灯熄灭
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_TRANSFER_CONFIGURE: {
        s_transfer_idle_cycles = req[ri++];
        s_transfer_wait_retry = req[ri] | (req[ri + 1] << 8); ri += 2;
        s_transfer_match_retry = req[ri] | (req[ri + 1] << 8); ri += 2;
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_TRANSFER: {
        uint8_t dap_index = req[ri++];
        if (s_port == DAP_PORT_JTAG) s_jtag_index = dap_index;
        uint8_t count = req[ri++];
        uint8_t done = 0;
        uint8_t last_ack = DAP_TRANSFER_OK;
        uint16_t wi_count_pos = wi++;
        uint16_t wi_ack_pos = wi++; // ack 必须紧跟在 count 后面、data 之前（CMSIS-DAP 规范）
        // SWD 下 AP 寄存器读是流水线的：发出请求那一刻返回的数据是“上一次”访问的结果，
        // 必须靠再访问一次（下一次 AP 读，或用 DP RDBUFF）才能把真正的值冲出来——
        // 否则读到的永远是上一次/初始的陈旧值（表现为 CPUID 之类的寄存器读回 0）。
        // JTAG 下 jtag_transfer() 每次内部已经自带一次 RDBUFF 冲刷，不需要这里再处理。
        bool post_read = false;
        for (; done < count; done++) {
            uint8_t xreq = req[ri++];
            uint32_t data = 0;
            bool is_read = xreq & SWD_REQ_RnW;

            if (s_port != DAP_PORT_SWD) {
                if (!is_read) {
                    memcpy(&data, &req[ri], 4);
                    ri += 4;
                }
                last_ack = do_transfer(xreq & 0x0F, &data);
                if (last_ack == DAP_TRANSFER_OK && is_read) {
                    memcpy(&resp[wi], &data, 4);
                    wi += 4;
                }
                if (last_ack != DAP_TRANSFER_OK) { done++; break; }
                for (uint8_t k = 0; k < s_transfer_idle_cycles; k++) {
                    if (s_port == DAP_PORT_JTAG) dap_io_jtag_clock(0, 0);
                }
                continue;
            }

            if (is_read) {
                if (post_read) {
                    // 上一次挂起的 AP 读还没取回，这次先把它冲出来
                    if (xreq & SWD_REQ_APnDP) {
                        // 连续 AP 读：这次访问顺带取回上一次的数据，同时又给自己挂起新的一次
                        last_ack = do_transfer(xreq & 0x0F, &data);
                    } else {
                        // 换成了 DP 读，必须单独用 RDBUFF 冲刷
                        last_ack = do_transfer(DP_RDBUFF_REQ, &data);
                        post_read = false;
                    }
                    if (last_ack != DAP_TRANSFER_OK) { done++; break; }
                    memcpy(&resp[wi], &data, 4);
                    wi += 4;
                    if (xreq & SWD_REQ_APnDP) {
                        for (uint8_t k = 0; k < s_transfer_idle_cycles; k++) {
                            if (s_port == DAP_PORT_JTAG) dap_io_jtag_clock(0, 0);
                        }
                        continue; // 上面那次访问已经顺带完成了这次 AP 读的请求
                    }
                }

                if (xreq & SWD_REQ_APnDP) {
                    // AP 读是流水线的：立即返回的数据不可用，挂起等下一次访问再取
                    last_ack = do_transfer(xreq & 0x0F, NULL);
                    if (last_ack != DAP_TRANSFER_OK) { done++; break; }
                    post_read = true;
                } else {
                    // DP 读没有流水线延迟，直接拿结果
                    last_ack = do_transfer(xreq & 0x0F, &data);
                    if (last_ack != DAP_TRANSFER_OK) { done++; break; }
                    memcpy(&resp[wi], &data, 4);
                    wi += 4;
                }
            } else {
                if (post_read) {
                    // 写之前先用 RDBUFF 冲掉挂起的 AP 读
                    last_ack = do_transfer(DP_RDBUFF_REQ, &data);
                    post_read = false;
                    if (last_ack != DAP_TRANSFER_OK) { done++; break; }
                    memcpy(&resp[wi], &data, 4);
                    wi += 4;
                }
                memcpy(&data, &req[ri], 4);
                ri += 4;
                last_ack = do_transfer(xreq & 0x0F, &data);
                if (last_ack != DAP_TRANSFER_OK) { done++; break; }
            }

            for (uint8_t k = 0; k < s_transfer_idle_cycles; k++) {
                if (s_port == DAP_PORT_JTAG) dap_io_jtag_clock(0, 0);
            }
        }
        if (last_ack == DAP_TRANSFER_OK && post_read) {
            // 批次结束时还有一次挂起的 AP 读，补一次 RDBUFF 冲出最终结果
            uint32_t data = 0;
            last_ack = do_transfer(DP_RDBUFF_REQ, &data);
            if (last_ack == DAP_TRANSFER_OK) {
                memcpy(&resp[wi], &data, 4);
                wi += 4;
            }
        }
        resp[wi_count_pos] = done;
        resp[wi_ack_pos] = last_ack;
        break;
    }
    case ID_DAP_TRANSFER_BLOCK: {
        uint8_t dap_index = req[ri++];
        if (s_port == DAP_PORT_JTAG) s_jtag_index = dap_index;
        uint16_t count = req[ri] | (req[ri + 1] << 8); ri += 2;
        uint8_t xreq = req[ri++];
        bool is_read = xreq & SWD_REQ_RnW;
        bool is_ap = xreq & SWD_REQ_APnDP;
        uint8_t ack = DAP_TRANSFER_OK;
        uint16_t done = 0;
        uint16_t wi_count_pos = wi; wi += 2;
        uint16_t wi_ack_pos = wi++; // ack 必须紧跟在 count 后面、data 之前
        if (is_read && is_ap && s_port == DAP_PORT_SWD) {
            // AP 块读同样是流水线的：先占位读一次把管线填上（结果丢弃），
            // 最后一次改读 DP RDBUFF 把最后挂起的数据冲出来
            ack = do_transfer(xreq & 0x0F, NULL);
        }
        if (ack == DAP_TRANSFER_OK) {
            for (; done < count; done++) {
                uint32_t data = 0;
                if (!is_read) {
                    memcpy(&data, &req[ri], 4);
                    ri += 4;
                    ack = do_transfer(xreq & 0x0F, &data);
                    if (ack != DAP_TRANSFER_OK) { done++; break; }
                    continue;
                }
                uint8_t this_req = xreq & 0x0F;
                if (is_ap && s_port == DAP_PORT_SWD && done == (uint16_t)(count - 1)) {
                    this_req = DP_RDBUFF_REQ;
                }
                ack = do_transfer(this_req, &data);
                if (ack != DAP_TRANSFER_OK) { done++; break; }
                memcpy(&resp[wi], &data, 4);
                wi += 4;
            }
        }
        resp[wi_count_pos] = done & 0xFF;
        resp[wi_count_pos + 1] = (done >> 8) & 0xFF;
        resp[wi_ack_pos] = ack;
        break;
    }
    case ID_DAP_TRANSFER_ABORT:
        // 位带实现下传输是同步阻塞完成的，无排队命令可中止
        resp[wi++] = DAP_OK;
        break;
    case ID_DAP_WRITE_ABORT: {
        ri++; // DAP Index，忽略
        uint32_t value;
        memcpy(&value, &req[ri], 4); ri += 4;
        do_transfer(0, &value); // 写 DP ABORT 寄存器 (APnDP=0,RnW=0,A=0x0)
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_DELAY: {
        uint16_t us = req[ri] | (req[ri + 1] << 8); ri += 2;
        esp_rom_delay_us(us);
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_RESET_TARGET: {
        dap_io_set_nreset(true);
        esp_rom_delay_us(10000);
        dap_io_set_nreset(false);
        resp[wi++] = DAP_OK;
        resp[wi++] = 0; // 未执行设备专属复位序列
        break;
    }
    case ID_DAP_SWJ_PINS: {
        uint8_t value = req[ri++];
        uint8_t select = req[ri++];
        uint32_t wait_us;
        memcpy(&wait_us, &req[ri], 4); ri += 4;
        dap_io_set_swj_pins(value, select);
        if (wait_us) esp_rom_delay_us(wait_us > 100000 ? 100000 : wait_us);
        resp[wi++] = dap_io_get_swj_pins();
        break;
    }
    case ID_DAP_SWJ_CLOCK: {
        uint32_t clk;
        memcpy(&clk, &req[ri], 4); ri += 4;
        dap_io_set_clock(clk);
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_SWJ_SEQUENCE: {
        uint8_t count = req[ri++];
        uint32_t bits = count == 0 ? 256 : count;
        dap_io_swj_sequence(bits, &req[ri]);
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_SWD_CONFIGURE: {
        uint8_t cfg = req[ri++];
        dap_io_swd_configure((cfg & 0x03) + 1);
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_JTAG_SEQUENCE: {
        uint8_t seq_count = req[ri++];
        resp[wi++] = DAP_OK;
        for (uint8_t s = 0; s < seq_count; s++) {
            uint8_t info = req[ri++];
            uint8_t tck_count = info & 0x3F; if (tck_count == 0) tck_count = 64;
            uint8_t tms = (info >> 6) & 1;
            bool capture = (info >> 7) & 1;
            uint8_t nbytes = (tck_count + 7) / 8;
            uint8_t tdo_bytes[8] = {0};
            for (uint8_t i = 0; i < tck_count; i++) {
                uint8_t tdi = (req[ri + (i >> 3)] >> (i & 7)) & 1;
                uint8_t tdo = dap_io_jtag_clock(tms, tdi);
                if (tdo) tdo_bytes[i >> 3] |= (1u << (i & 7));
            }
            ri += nbytes;
            if (capture) { memcpy(&resp[wi], tdo_bytes, nbytes); wi += nbytes; }
        }
        break;
    }
    case ID_DAP_JTAG_CONFIGURE: {
        uint8_t count = req[ri++];
        if (count > MAX_JTAG_DEVICES) count = MAX_JTAG_DEVICES;
        s_jtag_count = count;
        for (uint8_t i = 0; i < count; i++) s_jtag_ir_len[i] = req[ri++];
        resp[wi++] = DAP_OK;
        break;
    }
    case ID_DAP_JTAG_IDCODE: {
        uint8_t index = req[ri++];
        // JTAG 复位后每个器件默认选中 IDCODE 指令，无需显式写 IR
        jtag_reset_to_idle();
        uint64_t idcode64 = 0;
        jtag_shift_dr(index, 0, 32, &idcode64);
        uint32_t idcode = (uint32_t)idcode64;
        resp[wi++] = DAP_OK;
        memcpy(&resp[wi], &idcode, 4); wi += 4;
        break;
    }
    default:
        // 未知命令：按 CMSIS-DAP 规范返回 0xFF
        resp[0] = 0xFF;
        return 1;
    }
    return wi;
}
