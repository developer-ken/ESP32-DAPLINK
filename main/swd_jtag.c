#include "swd_jtag.h"
#include "dap_config.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static uint8_t s_port = DAP_PORT_DISABLED;
static uint32_t s_half_period_ns = 1000; // 默认 ~500kHz
static uint8_t s_turnaround_cycles = 1;

// 忙等延时（纳秒级），ESP32-S3 上没有专门的纳秒延时 API，退化为微秒延时 + 空指令
static inline void half_period_delay(void)
{
    if (s_half_period_ns >= 1000) {
        esp_rom_delay_us(s_half_period_ns / 1000);
    } else {
        // 高速档：省略延时，翻转开销本身即提供最短半周期
        __asm__ __volatile__("nop; nop; nop; nop;");
    }
}

void dap_io_init(void)
{
    gpio_config_t io_conf = {0};

    // 输出型控制引脚：默认释放（开漏思路，高电平代表未激活）
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
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

    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << DAP_LED_CONNECT_PIN) | (1ULL << DAP_LED_RUNNING_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DAP_LED_CONNECT_PIN, 1);
    gpio_set_level(DAP_LED_RUNNING_PIN, 1);
}

void dap_io_deinit(void)
{
    dap_io_disconnect();
}

void dap_io_connect(uint8_t port)
{
    gpio_config_t io_conf = {0};

    dap_io_disconnect();
    s_port = port;

    if (port == DAP_PORT_SWD) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << DAP_SWD_CLK_PIN);
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(DAP_SWD_CLK_PIN, 1);

        dap_io_swd_dio_to_output();
        gpio_set_level(DAP_SWD_DIO_PIN, 1);
    } else if (port == DAP_PORT_JTAG) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << DAP_JTAG_TCK_PIN) | (1ULL << DAP_JTAG_TMS_PIN) | (1ULL << DAP_JTAG_TDI_PIN);
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        gpio_set_level(DAP_JTAG_TCK_PIN, 1);
        gpio_set_level(DAP_JTAG_TMS_PIN, 1);
        gpio_set_level(DAP_JTAG_TDI_PIN, 0);

        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << DAP_JTAG_TDO_PIN);
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
    }

    gpio_set_level(DAP_LED_CONNECT_PIN, port == DAP_PORT_DISABLED);
}

void dap_io_disconnect(void)
{
    if (s_port == DAP_PORT_SWD) {
        gpio_set_direction(DAP_SWD_DIO_PIN, GPIO_MODE_INPUT);
        gpio_set_direction(DAP_SWD_CLK_PIN, GPIO_MODE_INPUT);
    } else if (s_port == DAP_PORT_JTAG) {
        gpio_set_direction(DAP_JTAG_TCK_PIN, GPIO_MODE_INPUT);
        gpio_set_direction(DAP_JTAG_TMS_PIN, GPIO_MODE_INPUT);
        gpio_set_direction(DAP_JTAG_TDI_PIN, GPIO_MODE_INPUT);
    }
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
            gpio_set_level(DAP_SWD_DIO_PIN, bit);
            gpio_set_level(DAP_SWD_CLK_PIN, 0);
            half_period_delay();
            gpio_set_level(DAP_SWD_CLK_PIN, 1);
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
    gpio_set_direction(DAP_SWD_DIO_PIN, GPIO_MODE_OUTPUT);
}

void dap_io_swd_dio_to_input(void)
{
    gpio_set_direction(DAP_SWD_DIO_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(DAP_SWD_DIO_PIN, GPIO_PULLUP_ONLY);
}

void dap_io_swd_write_bits(uint32_t value, int count)
{
    for (int i = 0; i < count; i++) {
        gpio_set_level(DAP_SWD_DIO_PIN, (value >> i) & 1);
        gpio_set_level(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        gpio_set_level(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
}

uint32_t dap_io_swd_read_bits(int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; i++) {
        gpio_set_level(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        if (gpio_get_level(DAP_SWD_DIO_PIN)) {
            value |= (1u << i);
        }
        gpio_set_level(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
    return value;
}

void dap_io_swd_turnaround(void)
{
    for (uint8_t i = 0; i < s_turnaround_cycles; i++) {
        gpio_set_level(DAP_SWD_CLK_PIN, 0);
        half_period_delay();
        gpio_set_level(DAP_SWD_CLK_PIN, 1);
        half_period_delay();
    }
}

uint8_t dap_io_jtag_clock(uint8_t tms, uint8_t tdi)
{
    gpio_set_level(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    gpio_set_level(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    gpio_set_level(DAP_JTAG_TCK_PIN, 0);
    half_period_delay();
    uint8_t tdo = gpio_get_level(DAP_JTAG_TDO_PIN);
    gpio_set_level(DAP_JTAG_TCK_PIN, 1);
    half_period_delay();
    return tdo;
}

void dap_io_jtag_set_pins(uint8_t tck, uint8_t tms, uint8_t tdi)
{
    gpio_set_level(DAP_JTAG_TMS_PIN, tms ? 1 : 0);
    gpio_set_level(DAP_JTAG_TDI_PIN, tdi ? 1 : 0);
    gpio_set_level(DAP_JTAG_TCK_PIN, tck ? 1 : 0);
}

uint8_t dap_io_jtag_get_tdo(void)
{
    return gpio_get_level(DAP_JTAG_TDO_PIN);
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
