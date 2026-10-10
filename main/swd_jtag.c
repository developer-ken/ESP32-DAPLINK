#include "swd_jtag.h"
#include "dap_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "hal/gpio_ll.h"
#include "hal/spi_ll.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

// gpio_set_level()/gpio_get_level() 经过驱动层的参数检查和自旋锁，单次调用开销可达数百 ns，
// 在位带时序的热路径里会让实际翻转速率远低于请求频率（示波器实测 2MHz 请求只能跑出 ~300kHz）。
// 改用 HAL 层的无锁寄存器读写，单次调用只有几条指令。
#define FAST_SET_LEVEL(pin, level) gpio_ll_set_level(GPIO_LL_GET_HW(0), (pin), (level))
#define FAST_GET_LEVEL(pin)        gpio_ll_get_level(GPIO_LL_GET_HW(0), (pin))

static uint8_t s_port = DAP_PORT_DISABLED;
static uint32_t s_half_period_ns = 1000; // 默认 ~500kHz
static uint32_t s_half_period_cycles = 0; // 与 s_half_period_ns 配套，只在 dap_io_set_clock() 里算一次
static uint8_t s_turnaround_cycles = 1;
static bool s_fast_clock = false;   // fast 档：不加延时，实际速率由翻转指令开销决定（bit-bang 极限）
static bool s_use_critical = false; // 高频档才加临界区：低频档周期长、被打断无害，且避免长期关中断

// ---------------- SWD SPI 后端 ----------------
// SWD 用 GP-SPI 外设模拟：SPI 的 SCK 充当 SWCLK，MOSI 以 3WIRE（单线双向）+ 半双工充当 SWDIO。
//   半双工（SPI_DEVICE_HALFDUPLEX）：先输出 tx 相位、再输入 rx 相位，方向由硬件无缝切换，
//   正好表达 SWD 的“写请求→转向→读 ACK/数据”分时双向时序。
//   3WIRE（SPI_DEVICE_3WIRE）：MOSI 兼做输出与输入，无需独立的 MISO 引脚，也无需 PCB 短接。
//   LSB-first（SPI_DEVICE_BIT_LSBFIRST）：SWD 是 LSB-first 位序。
//   NO_DUMMY（SPI_DEVICE_NO_DUMMY）：禁用高速读自动插入的 dummy 时钟，SWD 时钟必须精确，
//   建立时间由 SWD 协议自身的转向周期（turnaround）保证。
//   时钟相位：写方向用 mode=3（CPOL=1,CPHA=1），读方向用 mode=2（CPOL=1,CPHA=0）。
//   SWD 协议边沿不对称：主机写数据在下降沿变化、目标在上升沿采样（mode 3 正确）；
//   但目标读数据在**上升沿**驱动，主机必须在**下降沿**采样（mode 2）才不跟目标抢同一沿。
//   因此每笔事务前按“纯写/纯读”翻转 ck_out_edge（CPHA）位，实现两种边沿。
#define SWD_SPI_HOST        SPI2_HOST
static spi_device_handle_t s_spi_dev = NULL;
static bool s_spi_bus_ready = false;    // spi_bus_initialize 已完成
static bool s_spi_active = false;       // SWD 已接管（spi_bus_add_device 完成）
static uint32_t s_spi_clk_hz = 1000000; // 当前 SWD SPI 时钟频率（SWCLK 频率）

// 位带时序不中断安全：WiFi/Tick ISR 在传输中途插入会拉长某个半周期（SWD 表现为 ACK/IDR 读错）。
// 用自旋锁保护“单段连续位序列”（一次 write/read/turnaround），而不是整个命令，
// 这样无关中断仍能在两段位序列之间得到服务。
static portMUX_TYPE s_io_mux = portMUX_INITIALIZER_UNLOCKED;

// 忙等延时。所有档位统一用 CPU 周期计数忙等（含低频），不在半周期里调用 esp_rom_delay_us，
// 避免函数调用/ROM 调用开销影响高频档精度。
// always_inline：把 entry/retw 与多余判断开销从热路径里去掉，否则 slow 档固定开销会高达 ~50 周期，
// 导致“请求 2MHz 实测只有 1.38MHz”且 slow 档最高只能到 ~2.3MHz。
// IRAM_ATTR：时序函数放 SRAM，执行不经过 flash cache，避免 WiFi 占用 cache 时引入抖动。
static inline __attribute__((always_inline)) IRAM_ATTR void half_period_delay(void)
{
    if (s_fast_clock) {
        // fast 档：加少量 nop，把速率稳定在 ~5MHz。不加的话内联后 fast 档会冲到 ~6.5MHz，
        // 读采样点相对 CLK 下降沿过近（GPIO 写/读各有跨时钟域延迟），目标可能来不及建立数据。
        __asm__ __volatile__("nop; nop; nop; nop; nop; nop;");
    } else if (s_half_period_cycles != 0) {
        // slow 档：忙等 cycles 个 CPU 周期（含低频档，统一用周期计数，不调 esp_rom_delay_us）
        uint32_t start = esp_cpu_get_cycle_count();
        while ((esp_cpu_get_cycle_count() - start) < s_half_period_cycles) {
        }
    }
    // cycles == 0 且非 fast 档：无延时（仅初始态/断层区间出现，无害）
}

// ---------------- SWD SPI 后端：初始化 / 释放 / 传输原语 ----------------

// 确保 SPI 总线已初始化（把 SCK/MOSI 经 GPIO matrix 映射到 SWCLK/SWDIO）
static void spi_swd_bus_ensure(void)
{
    if (s_spi_bus_ready) {
        return;
    }
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = DAP_SWD_CLK_PIN,
        .mosi_io_num = DAP_SWD_DIO_PIN,
        .miso_io_num = -1,    // 3WIRE 复用 MOSI，不需要独立 MISO
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8, // 最大一次 37bit 写 / 33bit 读，8 字节足够
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    if (spi_bus_initialize(SWD_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED) != ESP_OK) {
        return; // 失败则保持 s_spi_bus_ready=false，后续调用会重试
    }
    s_spi_bus_ready = true;
}

// 确保 SPI 设备已挂载（接管 SWCLK/SWDIO）
static void spi_swd_device_ensure(void)
{
    spi_swd_bus_ensure();
    if (s_spi_active) {
        return;
    }
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)s_spi_clk_hz,
        .mode = 3,          // CPOL=1, CPHA=1：SWCLK 空闲高、下降沿变化、上升沿采样
        .spics_io_num = -1, // SWD 无片选
        .queue_size = 1,
        .command_bits = 0,
        .address_bits = 0,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE |
                 SPI_DEVICE_BIT_LSBFIRST | SPI_DEVICE_NO_DUMMY,
    };
    if (spi_bus_add_device(SWD_SPI_HOST, &dev_cfg, &s_spi_dev) != ESP_OK) {
        return;
    }
    s_spi_active = true;
}

// 释放 SPI 对 SWCLK/SWDIO 的占用（spi_bus_free 内部会 gpio_reset_pin 归还 GPIO）
static void spi_swd_release(void)
{
    if (s_spi_active) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
        s_spi_active = false;
    }
    if (s_spi_bus_ready) {
        spi_bus_free(SWD_SPI_HOST);
        s_spi_bus_ready = false;
    }
}

// 翻转 SPI 采样边沿（CPHA / ck_out_edge）：
//   纯写（rx_bits==0）：mode 3，下降沿驱动、上升沿采样（目标在上升沿采样主机写数据）。
//   纯读（rx_bits!=0）：mode 2，下降沿采样（目标在上升沿驱动读数据，需晚半周期采样）。
// 直接写寄存器而非重挂设备，避免每 bit/每段事务都走 spi_bus_remove/add_device 的开销。
static inline void spi_swd_set_sample_edge(bool read)
{
    SPI_LL_GET_HW(SWD_SPI_HOST)->user.ck_out_edge = read ? 1 : 0;
}

// SPI 半双工传输原语：先输出 tx_bits 位，再输入 rx_bits 位，返回读值（右对齐到 bit0）
static uint32_t spi_swd_xfer(uint32_t tx, uint8_t tx_bits, uint8_t rx_bits)
{
    spi_transaction_t t = {0};
    uint32_t rx = 0;
    spi_swd_set_sample_edge(rx_bits != 0);
    t.length = tx_bits;
    t.rxlength = rx_bits;
    t.tx_buffer = tx_bits ? (const uint8_t *)&tx : NULL;
    t.rx_buffer = rx_bits ? (uint8_t *)&rx : NULL;
    spi_device_polling_transmit(s_spi_dev, &t);
    return rx;
}

// 同上，但 tx/rx 用 64bit 缓冲，支持一次 33/34 bit 的读写
static uint64_t spi_swd_xfer64(uint64_t tx, uint8_t tx_bits, uint8_t rx_bits)
{
    spi_transaction_t t = {0};
    uint64_t rx = 0;
    spi_swd_set_sample_edge(rx_bits != 0);
    t.length = tx_bits;
    t.rxlength = rx_bits;
    t.tx_buffer = tx_bits ? (const uint8_t *)&tx : NULL;
    t.rx_buffer = rx_bits ? (uint8_t *)&rx : NULL;
    spi_device_polling_transmit(s_spi_dev, &t);
    return rx;
}

// 参照 ARM 官方 CMSIS-DAP 参考实现（PORT_SWD_SETUP / PIN_SWDIO_OUT_ENABLE|DISABLE）：
// SWD/JTAG 信号引脚的 IO_MUX 功能选择、上下拉只在这里一次性配置，之后连接/断开、
// 转向都只翻转 output-enable 这一个寄存器位，绝不再调用 gpio_set_direction()/gpio_config()
// ——那两个函数会重新走 IO_MUX 配置，耗时不确定，放在转向这个时序最敏感的临界点上
// 会引入抖动，实测表现为 SWD ACK 读到垃圾值（JUNK）。
void dap_io_init(void)
{
    gpio_config_t io_conf = {0};

    // nRESET / nTRST：开漏，默认释放（高电平代表未激活）
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << DAP_NRESET_PIN)
#if DAP_JTAG_HAS_TRST
                         | (1ULL << DAP_JTAG_TRST_PIN)
#endif
                         ;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_NRESET_PIN, 1);
#if DAP_JTAG_HAS_TRST
    gpio_set_level(DAP_JTAG_TRST_PIN, 1);
#endif

    // SWCLK/TCK（同一引脚）：宿主始终驱动，不需要转向
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_SWD_CLK_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_SWD_CLK_PIN, 1);

    // SWDIO/TMS（同一引脚）：需要转向，常驻 INPUT_OUTPUT + 上拉
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_SWD_DIO_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_SWD_DIO_PIN, 1);

    // JTAG_TDI：只做输出
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_JTAG_TDI_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_JTAG_TDI_PIN, 0);

    // JTAG_TDO：只做输入
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_JTAG_TDO_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // LED 引脚不在这里配置：统一由 status_led_init() 管理

    // 引脚模式已配置完毕，断开态默认释放总线（只关 output-enable）
    dap_io_disconnect();
}

void dap_io_deinit(void)
{
    dap_io_disconnect();
}

// 重新把 SWCLK/TCK、SWDIO/TMS 配置回普通 GPIO（JTAG 用）。
// SPI 释放后 gpio_reset_pin 已把这两根引脚的 IO_MUX 复位为默认态，JTAG 需重建其 GPIO 模式。
static void swclk_swdio_gpio_restore(void)
{
    gpio_config_t io_conf = {0};
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_SWD_CLK_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_SWD_CLK_PIN, 1);

    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_SWD_DIO_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_SWD_DIO_PIN, 1);
}

void dap_io_connect(uint8_t port)
{
    dap_io_disconnect();
    s_port = port;

    if (port == DAP_PORT_SWD) {
        // SWD：SPI 外设接管 SWCLK/SWDIO（SCK/MOSI 经 GPIO matrix 映射）
        spi_swd_device_ensure();
    } else if (port == DAP_PORT_JTAG) {
        // JTAG：SPI 已在 disconnect 里释放并 gpio_reset_pin 了 SWCLK/SWDIO，
        // 需重新把它们配回普通 GPIO 再输出使能
        swclk_swdio_gpio_restore();
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TCK_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TMS_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TDI_PIN);
        FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 1);
        FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, 1);
        FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, 0);
    }
}

void dap_io_disconnect(void)
{
    // SWD：释放 SPI 对 SWCLK/SWDIO 的占用（spi_bus_free 内部 gpio_reset_pin 归还 GPIO）
    spi_swd_release();
    // JTAG/GPIO：只关 output-enable 释放总线（引脚模式在 connect 时已配好）
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_CLK_PIN);
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_JTAG_TDI_PIN);
    s_port = DAP_PORT_DISABLED;
}

void dap_io_set_clock(uint32_t clock_hz)
{
    if (clock_hz == 0) {
        clock_hz = 500000;
    }
    // 半周期时间 = 1e9 / (2 * freq)
    uint64_t half_ns = 1000000000ULL / (2ULL * clock_hz);
    s_half_period_ns = (uint32_t)half_ns;

    // 把半周期换算成 CPU 周期数。
    // 用编译期常量 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ，而不是 esp_rom_get_cpu_ticks_per_us()：
    // 后者是 ROM 的 ets_get_cpu_frequency()，其缓存在某些启动路径下可能停留在 ROM 默认值(80)，
    // 导致 half_cycles 偏小、中低频被误判进 fast 档（实测“请求 2MHz 却输出 4.7MHz”的根因）。
    uint32_t ticks_per_us = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t half_cycles = (uint32_t)((uint64_t)s_half_period_ns * ticks_per_us / 1000);

    if (half_cycles <= DAP_FAST_OVERHEAD_CYCLES) {
        // 目标半周期已小于翻转固定开销 → fast 档：不再忙等，输出即 bit-bang 极限速率
        s_fast_clock = true;
        s_half_period_cycles = 0;
    } else if (half_cycles > DAP_SLOW_OVERHEAD_CYCLES) {
        // slow 档：忙等“目标半周期 - 固定开销”，精确匹配请求频率
        s_fast_clock = false;
        s_half_period_cycles = half_cycles - DAP_SLOW_OVERHEAD_CYCLES;
    } else {
        // 介于 fast 阈值与 slow 极限之间：slow 档忙等 0，输出 slow 档固定开销速率（尽量快）
        s_fast_clock = false;
        s_half_period_cycles = 0;
    }

    // 临界区：仅半周期较短（>500kHz）时加，防 ISR 拉长半周期；
    // 低频档周期长、被打断无害，且避免在临界区里长时间关中断。
    s_use_critical = (s_half_period_ns < 1000);

    // SWD SPI 后端：SWCLK 频率即 SPI 时钟频率。若 SPI 已接管则按新频率重挂设备。
    s_spi_clk_hz = clock_hz;
    if (s_spi_active) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
        s_spi_active = false;
        spi_swd_device_ensure();
    }
}

uint8_t dap_io_get_swj_pins(void)
{
    uint8_t v = 0;
    if (s_port == DAP_PORT_JTAG) {
        v |= gpio_get_level(DAP_JTAG_TCK_PIN) ? 0x01 : 0;
        v |= gpio_get_level(DAP_JTAG_TMS_PIN) ? 0x02 : 0;
        v |= gpio_get_level(DAP_JTAG_TDI_PIN) ? 0x04 : 0;
        v |= gpio_get_level(DAP_JTAG_TDO_PIN) ? 0x08 : 0;
    } else {
        v |= gpio_get_level(DAP_SWD_CLK_PIN) ? 0x01 : 0;
        v |= gpio_get_level(DAP_SWD_DIO_PIN) ? 0x02 : 0;
    }
#if DAP_JTAG_HAS_TRST
    v |= gpio_get_level(DAP_JTAG_TRST_PIN) ? 0x20 : 0;
#else
    v |= 0x20;
#endif
    v |= gpio_get_level(DAP_NRESET_PIN) ? 0x80 : 0;
    return v;
}

void dap_io_set_swj_pins(uint8_t value, uint8_t select)
{
    if (select & 0x01) {
        gpio_set_level(s_port == DAP_PORT_JTAG ? DAP_JTAG_TCK_PIN : DAP_SWD_CLK_PIN, (value & 0x01) ? 1 : 0);
    }
    if (select & 0x02) {
        gpio_set_level(s_port == DAP_PORT_JTAG ? DAP_JTAG_TMS_PIN : DAP_SWD_DIO_PIN, (value & 0x02) ? 1 : 0);
    }
    if ((select & 0x04) && s_port == DAP_PORT_JTAG) {
        gpio_set_level(DAP_JTAG_TDI_PIN, (value & 0x04) ? 1 : 0);
    }
#if DAP_JTAG_HAS_TRST
    if (select & 0x20) {
        dap_io_set_ntrst((value & 0x20) == 0);
    }
#endif
    if (select & 0x80) {
        dap_io_set_nreset((value & 0x80) == 0);
    }
}

void IRAM_ATTR dap_io_swj_sequence(uint32_t count, const uint8_t *data)
{
    if (s_port == DAP_PORT_SWD) {
        // SWD：SPI 后端逐段移出（LSB first），每次最多 32bit，避免逐 bit 的 bit-bang
        uint32_t bit_idx = 0;
        while (bit_idx < count) {
            uint32_t n = count - bit_idx;
            if (n > 32) {
                n = 32;
            }
            uint32_t val = 0;
            for (uint32_t b = 0; b < n; b++) {
                uint32_t bi = bit_idx + b;
                val |= (uint32_t)((data[bi >> 3] >> (bi & 7)) & 1) << b;
            }
            spi_swd_xfer(val, (uint8_t)n, 0);
            bit_idx += n;
        }
        return;
    }
    // JTAG：保留 bit-bang
    for (uint32_t i = 0; i < count; i++) {
        uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;
        dap_io_jtag_clock(bit, 0);
    }
}

void dap_io_swd_configure(uint8_t turnaround_cycles)
{
    s_turnaround_cycles = turnaround_cycles ? turnaround_cycles : 1;
}

// ================= SWD 帧传输（SPI 后端） =================
// 这些原语仅用于 DAP_PORT_SWD + SPI 后端，把一次 SWD 帧压缩为最少次 SPI 半双工事务。
// 半双工事务内部“先 tx 后 rx”天然表达“写请求→转向→读 ACK”的方向切换，无需 bit-bang。

uint8_t dap_io_swd_req_ack(uint32_t request_packet)
{
    // 拆成独立的“纯写”与“纯读”两个事务：3WIRE 半双工下，“一次事务 length+rxlength 先写后读”
    // 的 MOSI 方向切换不可靠（读相位 MOSI 未及时高阻，读到残留输出 park=1 → ACK JUNK）。
    // 纯写/纯读在事务边界由驱动明确切换 MOSI 方向。
    spi_swd_xfer(request_packet, 8, 0);                            // 纯写 8bit 请求
    uint32_t rx = spi_swd_xfer(0, 0, (uint8_t)(s_turnaround_cycles + 3)); // 纯读 转向 + 3bit ACK
    // 低 s_turnaround_cycles 位是转向周期（丢弃），其后 3 位是 ACK
    return (uint8_t)((rx >> s_turnaround_cycles) & 0x7);
}

uint64_t dap_io_swd_read_phase(void)
{
    // 读 33bit（32 data + 1 parity），LSB first：低 32 位为数据、bit32 为校验
    uint64_t rd = spi_swd_xfer64(0, 0, 33);
    // 转向（读→写）：写 1bit=1（host 接管，SWDIO 回到 park 高）
    spi_swd_xfer(1, 1, 0);
    return rd;
}

void dap_io_swd_write_phase(uint32_t data, uint32_t parity)
{
    // 转向（读→写）：纯读 s_turnaround_cycles bit，MOSI 保持高阻（target 释放总线），
    // 不能像请求那样输出 park=1 —— 否则 target 会把这个 1 当作 data 相位第一个 bit 采样，
    // 导致 data 整体错位 1 位、parity 校验失败（ACK=FAULT）。
    spi_swd_xfer(0, 0, (uint8_t)s_turnaround_cycles);
    // 写 32bit 数据 + 1bit 校验，一次纯写事务
    uint64_t tx = ((uint64_t)parity << 32) | (uint64_t)data;
    spi_swd_xfer64(tx, 33, 0);
}

void dap_io_swd_idle(uint8_t idle_cycles)
{
    // 参照 ARM 官方 CMSIS-DAP 参考实现（SWD_Transfer）：只有当 idle_cycles != 0 时才输出
    // idle 位。idle_cycles == 0（OpenOCD 默认）时什么都不做——否则多出的这个 park=1 位会被
    // 目标当成下一帧的 start bit，使后续请求整体错位 1 拍、读回 JUNK/FAULT。
    if (idle_cycles == 0) {
        return;
    }
    // idle cycles：输出 0（低电平），之后 1bit=1 收尾回到 park 高
    for (uint8_t left = idle_cycles; left > 0;) {
        uint8_t n = left > 32 ? 32 : left;
        spi_swd_xfer(0, n, 0);
        left = (uint8_t)(left - n);
    }
    spi_swd_xfer(1, 1, 0);
}

void dap_io_swd_finish(void)
{
    // WAIT/FAULT 收尾：ACK 后 SWDIO 为输入态，写 1bit=1 完成转向（读→写）+ idle 高
    spi_swd_xfer(1, 1, 0);
}

void dap_io_swd_drain(void)
{
    // 协议错误收尾：目标可能仍处在 32+1 位数据相位，读 33bit 吸收后再收回总线
    spi_swd_xfer64(0, 0, 33);
    spi_swd_xfer(1, 1, 0);
}

uint8_t IRAM_ATTR dap_io_jtag_clock(uint8_t tms, uint8_t tdi)
{
    if (s_use_critical) {
        portENTER_CRITICAL(&s_io_mux);
    }
    FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 0);
    half_period_delay();
    uint8_t tdo = FAST_GET_LEVEL(DAP_JTAG_TDO_PIN);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 1);
    half_period_delay();
    if (s_use_critical) {
        portEXIT_CRITICAL(&s_io_mux);
    }
    return tdo;
}

void IRAM_ATTR dap_io_jtag_set_pins(uint8_t tck, uint8_t tms, uint8_t tdi)
{
    FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, tck ? 1 : 0);
}

uint8_t IRAM_ATTR dap_io_jtag_get_tdo(void)
{
    return FAST_GET_LEVEL(DAP_JTAG_TDO_PIN);
}

void dap_io_set_nreset(bool asserted)
{
    gpio_set_level(DAP_NRESET_PIN, asserted ? 0 : 1);
}

void dap_io_set_ntrst(bool asserted)
{
#if DAP_JTAG_HAS_TRST
    gpio_set_level(DAP_JTAG_TRST_PIN, asserted ? 0 : 1);
#else
    (void)asserted;
#endif
}
