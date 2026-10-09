// SWO (Serial Wire Output) trace 数据捕获
// 使用 ESP32-S3 硬件 UART 接收目标 TPIU 输出的 UART 编码（异步 NRZ 8N1）trace 流，
// 通过 GPIO 矩阵把 SWO 引脚（复用 TDO）路由到 UART RX。
// Manchester 编码未实现（需要专用定时器/位带解码），当前仅支持 UART 模式。
#include "swo.h"
#include "dap_config.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "swo";

#define SWO_EVENT_QUEUE_SIZE 16

static QueueHandle_t s_event_queue = NULL;
static uint8_t  s_mode = SWO_MODE_OFF;
static uint32_t s_baudrate = 1000000;
static volatile bool    s_capture_active = false;
static volatile uint8_t s_error_flags = 0;   // 累积错误位：SWO_STATUS_STREAM_ERROR / OVERRUN

void swo_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = (int)s_baudrate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_driver_install(DAP_SWO_UART_NUM, SWO_BUFFER_SIZE, 0,
                                        SWO_EVENT_QUEUE_SIZE, &s_event_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART 驱动安装失败: %s", esp_err_to_name(err));
        s_event_queue = NULL;
        return;
    }
    uart_param_config(DAP_SWO_UART_NUM, &cfg);
    // 只路由 RX 到 SWO 引脚，TX/RTS/CTS 不占用引脚
    uart_set_pin(DAP_SWO_UART_NUM, UART_PIN_NO_CHANGE, DAP_SWO_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// 消费 UART 事件，把溢出/帧错误累积到错误标志
static void swo_poll_events(void)
{
    if (s_event_queue == NULL) return;
    uart_event_t ev;
    while (xQueueReceive(s_event_queue, &ev, 0) == pdTRUE) {
        switch (ev.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            s_error_flags |= SWO_STATUS_OVERRUN;
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            s_error_flags |= SWO_STATUS_STREAM_ERROR;
            break;
        case UART_BREAK:
        case UART_DATA:
        default:
            break;
        }
    }
}

bool swo_set_transport(uint8_t transport)
{
    // 本实现只有 DAP_SWO_Data 轮询这一种通道；None 视为合法（关闭）
    return (transport == SWO_TRANSPORT_DATA || transport == SWO_TRANSPORT_NONE);
}

bool swo_set_mode(uint8_t mode)
{
    switch (mode) {
    case SWO_MODE_OFF:
        swo_control(false);
        s_mode = SWO_MODE_OFF;
        return true;
    case SWO_MODE_UART:
        s_mode = SWO_MODE_UART;
        return true;
    case SWO_MODE_MANCHESTER:
    default:
        return false;   // Manchester 未实现
    }
}

uint32_t swo_set_baudrate(uint32_t baudrate)
{
    if (baudrate == 0) {
        return 0;
    }
    if (uart_set_baudrate(DAP_SWO_UART_NUM, (int)baudrate) != ESP_OK) {
        return 0;
    }
    // 回读硬件实际配置的波特率（80MHz APB 分频后可能与请求值有微小偏差）。
    // DAP_SWO_Baudrate 协议要求返回"实际波特率"，主机（OpenOCD）据此计算分频，回读越准越稳。
    uint32_t actual = 0;
    if (uart_get_baudrate(DAP_SWO_UART_NUM, &actual) == ESP_OK && actual != 0) {
        s_baudrate = actual;
        return actual;
    }
    s_baudrate = baudrate;
    return baudrate;
}

void swo_control(bool start)
{
    // 无论启动还是停止都清空缓冲与错误标志，避免读到陈旧的/悬空的垃圾数据
    uart_flush_input(DAP_SWO_UART_NUM);
    swo_poll_events();
    s_error_flags = 0;
    s_capture_active = start;
}

uint16_t swo_read(uint8_t *buf, uint16_t max_count)
{
    swo_poll_events();
    if (!s_capture_active || max_count == 0) {
        return 0;
    }
    int len = uart_read_bytes(DAP_SWO_UART_NUM, buf, max_count, 0);
    return (len > 0) ? (uint16_t)len : 0;
}

uint32_t swo_get_buffered_count(void)
{
    swo_poll_events();
    size_t len = 0;
    uart_get_buffered_data_len(DAP_SWO_UART_NUM, &len);
    return (uint32_t)len;
}

uint8_t swo_get_status(void)
{
    swo_poll_events();
    uint8_t status = 0;
    if (s_capture_active) {
        status |= SWO_STATUS_ACTIVE;
    }
    status |= s_error_flags;
    return status;
}
