#include "swd_jtag.h"
#include "dap_config.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

// gpio_set_level()/gpio_get_level() 经过驱动层的参数检查和自旋锁，单次调用开销可达数百 ns。
// 即使 HAL 的 gpio_ll_set_level() 也已带 if(level)/if(gpio<32) 分支、且每次重算 1<<pin。
// 这里直接对 GPIO 寄存器做单条 store：SWD_CLK(36)/SWD_DIO(38)/JTAG_TDI(34) 都落在 GPIO32+
// 的上排 bank，写 out1_w1ts/out1_w1tc 的 .val（整 32 位成员，避免位域写触发读改写）；
// JTAG_TDO(9) 落在下排 bank，读 in。每条翻转最终编译为一次 store/load。
#define SWCLK_BIT   (1u << (DAP_SWD_CLK_PIN - 32))
#define SWDIO_BIT   (1u << (DAP_SWD_DIO_PIN - 32))
#define TDI_BIT     (1u << (DAP_JTAG_TDI_PIN - 32))

#define SWCLK_SET() do { GPIO.out1_w1ts.val = SWCLK_BIT; } while (0)
#define SWCLK_CLR() do { GPIO.out1_w1tc.val = SWCLK_BIT; } while (0)
#define SWDIO_SET() do { GPIO.out1_w1ts.val = SWDIO_BIT; } while (0)
#define SWDIO_CLR() do { GPIO.out1_w1tc.val = SWDIO_BIT; } while (0)
#define TDI_SET()   do { GPIO.out1_w1ts.val = TDI_BIT; } while (0)
#define TDI_CLR()   do { GPIO.out1_w1tc.val = TDI_BIT; } while (0)

#define SWDIO_RD()  ((GPIO.in1.val >> (DAP_SWD_DIO_PIN - 32)) & 1u)
#define TDO_RD()    ((GPIO.in >> DAP_JTAG_TDO_PIN) & 1u)

// JTAG 的 TCK/TMS 与 SWCLK/SWDIO 是同一根物理引脚，直接复用上面的宏
#define TCK_SET()   SWCLK_SET()
#define TCK_CLR()   SWCLK_CLR()
#define TMS_SET()   SWDIO_SET()
#define TMS_CLR()   SWDIO_CLR()

static uint8_t s_port = DAP_PORT_DISABLED;
static uint32_t s_busy_cycles = 0;        // 传给 half_period_delay() 的忙等 cycles（由 set_clock 用校准结果反解）
static uint32_t s_overhead = 0;           // 校准：N=0 时一个完整 SWCLK 周期的固定开销（含 SWDIO 分支+循环开销）
static uint32_t s_slope_x1024 = 1024;     // 校准：完整周期随 N 的斜率，1024 定点（≈1024，即每 N 增 1 周期）
static uint8_t s_turnaround_cycles = 1;
static bool s_use_critical = false;       // 高频档才加临界区：低频档周期长、被打断无害，且避免长期关中断
static const char *TAG = "swd_jtag";

// 位带时序不中断安全：WiFi/Tick ISR 在传输中途插入会拉长某个半周期（SWD 表现为 ACK/IDR 读错）。
// 用自旋锁保护“单段连续位序列”（一次 write/read/turnaround），而不是整个命令，
// 这样无关中断仍能在两段位序列之间得到服务。
static portMUX_TYPE s_io_mux = portMUX_INITIALIZER_UNLOCKED;

// 单条指令读 CCOUNT（Xtensa rsr.ccount）。用内联汇编显式保证不产生函数调用开销，
// 比 esp_cpu_get_cycle_count() 少了依赖编译器内联的一层不确定。
static inline __attribute__((always_inline)) IRAM_ATTR uint32_t read_ccount(void)
{
    uint32_t ccount;
    __asm__ __volatile__("rsr %0, ccount" : "=r"(ccount));
    return ccount;
}

// 忙等延时。用“绝对目标 + 周期计数”统一实现，全频段（含低频）都用 CPU 周期忙等，
// 不在半周期里调用 esp_rom_delay_us，避免函数/ROM 调用开销影响高频精度。
// always_inline：把 entry/retw 与多余判断开销从热路径去掉。
// IRAM_ATTR：时序函数放 SRAM，执行不经过 flash cache，避免 WiFi 占用 cache 时引入抖动。
// 循环体为空 → 每轮仅 rsr + bltu 两条指令，2-cycle 粒度（比带 nop 的 4-cycle 细一倍）。
// 用无符号比较：SWD 半周期最多数千 cycles，远小于 32 位回绕周期，无需有符号处理。
static inline __attribute__((always_inline)) IRAM_ATTR void half_period_delay(void)
{
    uint32_t target = read_ccount() + s_busy_cycles;
    while (read_ccount() < target) {
    }
}

// 用与 dap_io_swd_write_bits() 完全一致的结构（含 SWDIO 的 if/else 分支 store、for 循环
// 的 i++/比较/跳转）测一个完整 SWCLK 周期（两个半周期）的平均耗时，确保校准到的
// 固定开销与真实热路径一致。
static uint32_t measure_swclk_cycle(uint32_t busy)
{
    const uint32_t n = 256;
    uint32_t save = s_busy_cycles;
    s_busy_cycles = busy;

    uint32_t start = read_ccount();
    for (uint32_t i = 0; i < n; i++) {
        if (i & 1) SWDIO_SET(); else SWDIO_CLR();
        SWCLK_CLR();
        half_period_delay();
        SWCLK_SET();
        half_period_delay();
    }
    uint32_t elapsed = read_ccount() - start;
    s_busy_cycles = save;
    return elapsed / n; // 每个完整 SWCLK 周期的平均 cycles
}

// 两点线性拟合：周期(N) = s_overhead + (s_slope_x1024/1024) * N。
// 相比单点（只测 N=0）更能捕获忙等循环退出检测带来的斜率偏差，精度远高于写死常量。
// 仅在 dap_io_init()（单线程、中断稳定）调用一次，无需 IRAM。
static void calibrate_timing(void)
{
    const uint32_t N_REF = 300;
    uint32_t c0 = measure_swclk_cycle(0);
    uint32_t c1 = measure_swclk_cycle(N_REF);

    s_overhead = c0;
    uint32_t delta = c1 - c0;            // = 2 * slope * N_REF（一个周期含两次 delay）
    uint32_t slope_x1024 = (uint32_t)((uint64_t)delta * 1024u / (2u * N_REF));
    if (slope_x1024 == 0) {
        slope_x1024 = 1024;              // 防御：理论斜率恒为 1024
    }
    s_slope_x1024 = slope_x1024;

    // 报告校准结果：固定开销（完整周期 cycles）与对应的 bit-bang 最高频率
    uint32_t max_hz = (uint32_t)((uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL / s_overhead);
    ESP_LOGI(TAG, "时序校准完成: 固定开销=%lu cycles (最高约 %lu Hz), 斜率=%lu/1024",
             (unsigned long)s_overhead, (unsigned long)max_hz, (unsigned long)s_slope_x1024);
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

    // 校准位带时序：实测零延时半周期的真实开销（此时 SWCLK 已配置且未断开）
    calibrate_timing();

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
        SWCLK_SET();
        dap_io_swd_dio_to_output();
        SWDIO_SET();
    } else if (port == DAP_PORT_JTAG) {
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TCK_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TMS_PIN);
        gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_JTAG_TDI_PIN);
        TCK_SET();
        TMS_SET();
        TDI_CLR();
    }
}

void dap_io_disconnect(void)
{
    // 只关 output-enable 把总线释放为高阻，引脚模式/上下拉保持不变（已在 dap_io_init() 配好）
    // DAP_SWD_CLK_PIN==DAP_JTAG_TCK_PIN、DAP_SWD_DIO_PIN==DAP_JTAG_TMS_PIN 是同一根物理引脚
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

    // 半周期换算成 CPU 周期数：half = CPU_MHz * 1e6 / (2 * freq)
    // 用编译期常量 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ，而不是 esp_rom_get_cpu_ticks_per_us()：
    // 后者是 ROM 的 ets_get_cpu_frequency()，其缓存在某些启动路径下可能停留在 ROM 默认值(80)，
    // 导致 half_cycles 偏小、中低频被误判进 fast 档（实测“请求 2MHz 却输出 4.7MHz”的根因）。
    uint32_t half_cycles = (uint32_t)((uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000ULL / (2ULL * clock_hz));

    // 读时序安全下限：半周期再短目标可能来不及在下降沿后建立数据（SWD 读错）。
    if (half_cycles < DAP_MIN_HALF_PERIOD_CYCLES) {
        half_cycles = DAP_MIN_HALF_PERIOD_CYCLES;
    }

    // 目标完整周期 = 2 * half_cycles。用校准出的 offset/slope 反解忙等 cycles：
    //   周期(N) = s_overhead + (s_slope_x1024/1024)*N   =>   N = (周期 - s_overhead) * 1024 / s_slope_x1024
    uint32_t full = half_cycles * 2u;
    if (full <= s_overhead) {
        s_busy_cycles = 0;   // 已到 bit-bang 极限，输出即校准出的最高速率
    } else {
        s_busy_cycles = (uint32_t)((uint64_t)(full - s_overhead) * 1024u / s_slope_x1024);
    }

    // 临界区：仅半周期较短（>500kHz）时加，防 ISR 拉长半周期；
    // 低频档周期长、被打断无害，且避免长期关中断。
    s_use_critical = (clock_hz > 500000);
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
    for (uint32_t i = 0; i < count; i++) {
        uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;
        if (s_port == DAP_PORT_JTAG) {
            dap_io_jtag_clock(bit, 0);
        } else {
            if (bit) SWDIO_SET(); else SWDIO_CLR();
            SWCLK_CLR();
            half_period_delay();
            SWCLK_SET();
            half_period_delay();
        }
    }
}

void dap_io_swd_configure(uint8_t turnaround_cycles)
{
    s_turnaround_cycles = turnaround_cycles ? turnaround_cycles : 1;
}

void IRAM_ATTR dap_io_swd_dio_to_output(void)
{
    // 直接操作 output-enable 位，不重新走 gpio_set_direction()/IO_MUX 配置
    gpio_ll_output_enable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
}

void IRAM_ATTR dap_io_swd_dio_to_input(void)
{
    // 输入使能在 dap_io_connect() 里已经常驻打开，这里只需关输出，让目标接管驱动总线
    gpio_ll_output_disable(GPIO_LL_GET_HW(0), DAP_SWD_DIO_PIN);
}

void IRAM_ATTR dap_io_swd_write_bits(uint32_t value, int count)
{
    if (s_use_critical) {
        portENTER_CRITICAL(&s_io_mux);
    }
    for (int i = 0; i < count; i++) {
        if (value & (1u << i)) SWDIO_SET(); else SWDIO_CLR();
        SWCLK_CLR();
        half_period_delay();
        SWCLK_SET();
        half_period_delay();
    }
    if (s_use_critical) {
        portEXIT_CRITICAL(&s_io_mux);
    }
}

uint32_t IRAM_ATTR dap_io_swd_read_bits(int count)
{
    uint32_t value = 0;
    if (s_use_critical) {
        portENTER_CRITICAL(&s_io_mux);
    }
    for (int i = 0; i < count; i++) {
        SWCLK_CLR();
        half_period_delay();
        if (SWDIO_RD()) {
            value |= (1u << i);
        }
        SWCLK_SET();
        half_period_delay();
    }
    if (s_use_critical) {
        portEXIT_CRITICAL(&s_io_mux);
    }
    return value;
}

void IRAM_ATTR dap_io_swd_turnaround(void)
{
    if (s_use_critical) {
        portENTER_CRITICAL(&s_io_mux);
    }
    for (uint8_t i = 0; i < s_turnaround_cycles; i++) {
        SWCLK_CLR();
        half_period_delay();
        SWCLK_SET();
        half_period_delay();
    }
    if (s_use_critical) {
        portEXIT_CRITICAL(&s_io_mux);
    }
}

void IRAM_ATTR dap_io_swd_dio_idle_high(void)
{
    SWDIO_SET();
}

uint8_t IRAM_ATTR dap_io_jtag_clock(uint8_t tms, uint8_t tdi)
{
    if (s_use_critical) {
        portENTER_CRITICAL(&s_io_mux);
    }
    if (tms) TMS_SET(); else TMS_CLR();
    if (tdi) TDI_SET(); else TDI_CLR();
    TCK_CLR();
    half_period_delay();
    uint8_t tdo = TDO_RD();
    TCK_SET();
    half_period_delay();
    if (s_use_critical) {
        portEXIT_CRITICAL(&s_io_mux);
    }
    return tdo;
}

void IRAM_ATTR dap_io_jtag_set_pins(uint8_t tck, uint8_t tms, uint8_t tdi)
{
    if (tms) TMS_SET(); else TMS_CLR();
    if (tdi) TDI_SET(); else TDI_CLR();
    if (tck) TCK_SET(); else TCK_CLR();
}

uint8_t IRAM_ATTR dap_io_jtag_get_tdo(void)
{
    return TDO_RD();
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
