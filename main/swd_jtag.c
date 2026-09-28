#include "swd_jtag.h"
#include "dap_config.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_cpu.h"
#include "hal/gpio_ll.h"

// gpio_set_level()/gpio_get_level() 经过驱动层的参数检查和自旋锁，单次调用开销可达数百 ns，
// 在位带时序的热路径里会让实际翻转速率远低于请求频率（示波器实测 2MHz 请求只能跑出 ~300kHz）。
// 改用 HAL 层的无锁寄存器读写，单次调用只有几条指令。
#define FAST_SET_LEVEL(pin, level) gpio_ll_set_level(GPIO_LL_GET_HW(0), (pin), (level))
#define FAST_GET_LEVEL(pin)        gpio_ll_get_level(GPIO_LL_GET_HW(0), (pin))

static uint8_t s_port = DAP_PORT_DISABLED;
static uint32_t s_half_period_ns = 1000; // 默认 ~500kHz
static uint32_t s_half_period_cycles = 0; // 与 s_half_period_ns 配套，只在 dap_io_set_clock() 里算一次
static uint8_t s_turnaround_cycles = 1;

// 忙等延时（纳秒级）。之前亚微秒档完全不限速，实际翻转速率只取决于 gpio_set_level
// 调用本身的开销，跟主机请求的时钟频率毫无关系——杜邦线/面包板走线在这种不受控的高速
// 翻转下容易出现建立时间不足、过冲振铃，导致目标采样到错误电平（表现为 ACK/IDR 读取失败）。
// 改用 CPU 周期计数忙等，保证亚微秒档也能按请求频率输出。周期数缓存在 dap_io_set_clock() 里算好，
// 避免每个 bit 都重复调用 esp_rom_get_cpu_ticks_per_us()。
static inline void half_period_delay(void)
{
    if (s_half_period_ns >= 1000) {
        esp_rom_delay_us(s_half_period_ns / 1000);
        return;
    }
    if (s_half_period_cycles == 0) {
        __asm__ __volatile__("nop; nop; nop; nop;");
        return;
    }
    uint32_t start = esp_cpu_get_cycle_count();
    while ((esp_cpu_get_cycle_count() - start) < s_half_period_cycles) {
        // 忙等
    }
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

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_LED_CONNECT_PIN) | (1ULL << DAP_LED_RUNNING_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_LED_CONNECT_PIN, 1);
    gpio_set_level(DAP_LED_RUNNING_PIN, 1);

    // 引脚模式已配置完毕，断开态默认释放总线（只关 output-enable）
    dap_io_disconnect();
}

void dap_io_deinit(void)
{
    dap_io_disconnect();
}

void dap_io_connect(uint8_t port)
{
    dap_io_disconnect();
    s_port = port;

    if (port == DAP_PORT_SWD) {
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_SWD_CLK_PIN);
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 1);
        dap_io_swd_dio_to_output();
        FAST_SET_LEVEL(DAP_SWD_DIO_PIN, 1);
    } else if (port == DAP_PORT_JTAG) {
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TCK_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TMS_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TDI_PIN);
        FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 1);
        FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, 1);
        FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, 0);
    }

    gpio_set_level(DAP_LED_CONNECT_PIN, port == DAP_PORT_DISABLED);
}

void dap_io_disconnect(void)
{
    // 只关 output-enable 把总线释放为高阻，引脚模式/上下拉保持不变（已在 dap_io_init() 配好）
    // DAP_SWD_CLK_PIN==DAP_JTAG_TCK_PIN、DAP_SWD_DIO_PIN==DAP_JTAG_TMS_PIN 是同一根物理引脚
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_CLK_PIN);
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_JTAG_TDI_PIN);
    s_port = DAP_PORT_DISABLED;
    gpio_set_level(DAP_LED_CONNECT_PIN, 1);
}

void dap_io_set_clock(uint32_t clock_hz)
{
    if (clock_hz == 0) {
        clock_hz = 500000;
    }
    // 半周期时间 = 1e9 / (2 * freq)，最小钳位到 0（进入高速空转档）
    uint64_t half_ns = 1000000000ULL / (2ULL * clock_hz);
    s_half_period_ns = (uint32_t)half_ns;
    if (s_half_period_ns < 1000) {
        // 提前算好周期数，避免热路径里每次都重新查 CPU 主频
        uint32_t ticks_per_us = esp_rom_get_cpu_ticks_per_us();
        s_half_period_cycles = (uint32_t)((uint64_t)s_half_period_ns * ticks_per_us / 1000);
    } else {
        s_half_period_cycles = 0;
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

void dap_io_swj_sequence(uint32_t count, const uint8_t *data)
{
    for (uint32_t i = 0; i < count; i++) {
        uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;
        if (s_port == DAP_PORT_JTAG) {
            dap_io_jtag_clock(bit, 0);
        } else {
            FAST_SET_LEVEL(DAP_SWD_DIO_PIN, bit);
            FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 0);
            half_period_delay();
            FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 1);
            half_period_delay();
        }
    }
}

void dap_io_swd_configure(uint8_t turnaround_cycles)
{
    s_turnaround_cycles = turnaround_cycles ? turnaround_cycles : 1;
}

void dap_io_swd_dio_to_output(void)
{
    // 直接操作 output-enable 位，不重新走 gpio_set_direction()/IO_MUX 配置
    gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
}

void dap_io_swd_dio_to_input(void)
{
    // 输入使能在 dap_io_connect() 里已经常驻打开，这里只需关输出，让目标接管驱动总线
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
}

void dap_io_swd_write_bits(uint32_t value, int count)
{
    for (int i = 0; i < count; i++) {
        FAST_SET_LEVEL(DAP_SWD_DIO_PIN, (value >> i) & 1);
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
}

uint32_t dap_io_swd_read_bits(int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; i++) {
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        if (FAST_GET_LEVEL(DAP_SWD_DIO_PIN)) {
            value |= (1u << i);
        }
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
    return value;
}

void dap_io_swd_turnaround(void)
{
    for (uint8_t i = 0; i < s_turnaround_cycles; i++) {
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        FAST_SET_LEVEL(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
}

void dap_io_swd_dio_idle_high(void)
{
    FAST_SET_LEVEL(DAP_SWD_DIO_PIN, 1);
}

uint8_t dap_io_jtag_clock(uint8_t tms, uint8_t tdi)
{
    FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 0);
    half_period_delay();
    uint8_t tdo = FAST_GET_LEVEL(DAP_JTAG_TDO_PIN);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, 1);
    half_period_delay();
    return tdo;
}

void dap_io_jtag_set_pins(uint8_t tck, uint8_t tms, uint8_t tdi)
{
    FAST_SET_LEVEL(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    FAST_SET_LEVEL(DAP_JTAG_TCK_PIN, tck ? 1 : 0);
}

uint8_t dap_io_jtag_get_tdo(void)
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
