// SWO (Serial Wire Output) trace 数据捕获
// 使用 ESP32-S3 硬件 UART 接收目标 TPIU 输出的 UART 编码（异步 NRZ 8N1）trace 流，
// 通过 GPIO 矩阵把 SWO 引脚（复用 TDO）路由到 UART RX。
// Manchester 编码未实现（需要专用定时器/位带解码），当前仅支持 UART 模式。
#include "swo.h"
#include "dap_config.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "swo";

#define SWO_EVENT_QUEUE_SIZE 16
#define SWO_STREAM_CHUNK     512   // 每次从 UART 缓冲读取并送出的最大字节数（匹配 Bulk 传输缓冲）

static QueueHandle_t s_event_queue = NULL;
static uint8_t  s_mode = SWO_MODE_OFF;
static volatile uint8_t s_transport = SWO_TRANSPORT_DATA;
static uint32_t s_baudrate = 1000000;
static volatile bool    s_capture_active = false;
static volatile uint8_t s_error_flags = 0;   // 累积错误位：SWO_STATUS_STREAM_ERROR / OVERRUN
static volatile swo_stream_sink_t s_stream_sink = NULL; // USB Bulk 流式输出回调
static portMUX_TYPE s_error_lock = portMUX_INITIALIZER_UNLOCKED;

static void swo_stream_task(void *arg);

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

    xTaskCreate(swo_stream_task, "swo_stream", 4096, NULL, 10, NULL);
}

// SWO 流式任务：作为 UART 事件队列的唯一消费者，阻塞等待事件到达（零轮询延迟）。
//   - 数据事件(UART_DATA / UART_BUFFER_FULL)到达且启用 WINUSB 流式 + 捕获激活时，
//     立即把 UART 缓冲里的 trace 数据读出并通过 s_stream_sink 送出（USB Bulk IN）。
//   - 溢出/帧错误事件(UART_FIFO_OVF / UART_BUFFER_FULL / UART_FRAME_ERR / PARITY_ERR)
//     累积到错误标志，供 DAP_SWO_Status / DAP_SWO_Data 查询。
static void swo_stream_task(void *arg)
{
    (void)arg;
    uart_event_t ev;
    for (;;) {
        // 阻塞等待下一个 UART 事件（数据到达、缓冲满、溢出、帧错误等）
        if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // 累积错误标志（无论是否 streaming 都要记录）
        switch (ev.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            portENTER_CRITICAL(&s_error_lock);
            s_error_flags |= SWO_STATUS_OVERRUN;
            portEXIT_CRITICAL(&s_error_lock);
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            portENTER_CRITICAL(&s_error_lock);
            s_error_flags |= SWO_STATUS_STREAM_ERROR;
            portEXIT_CRITICAL(&s_error_lock);
            break;
        case UART_BREAK:
        case UART_DATA:
        default:
            break;
        }

        // 流式激活时，把当前缓冲里的数据全部送出（一次事件可能对应多字节/多个块）
        if (s_capture_active && s_transport == SWO_TRANSPORT_WINUSB && s_stream_sink) {
            uint8_t buf[SWO_STREAM_CHUNK];
            for (;;) {
                uint16_t n = swo_read(buf, sizeof(buf));
                if (n == 0) break;
                s_stream_sink(buf, n);
            }
        }
    }
}

bool swo_set_transport(uint8_t transport)
{
    switch (transport) {
    case SWO_TRANSPORT_NONE:
    case SWO_TRANSPORT_DATA:
    case SWO_TRANSPORT_WINUSB:
        s_transport = transport;
        return true;
    default:
        return false;
    }
}

uint8_t swo_get_transport(void)
{
    return s_transport;
}

void swo_set_stream_sink(swo_stream_sink_t sink)
{
    s_stream_sink = sink;
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
    // 无论启动还是停止都清空缓冲、事件队列与错误标志，避免读到陈旧的/悬空的垃圾数据。
    // 事件队列由流式任务独占消费，这里用 xQueueReset 丢弃残留事件而非消费它们。
    uart_flush_input(DAP_SWO_UART_NUM);
    if (s_event_queue != NULL) {
        xQueueReset(s_event_queue);
    }
    portENTER_CRITICAL(&s_error_lock);
    s_error_flags = 0;
    portEXIT_CRITICAL(&s_error_lock);
    s_capture_active = start;
}

uint16_t swo_read(uint8_t *buf, uint16_t max_count)
{
    if (!s_capture_active || max_count == 0) {
        return 0;
    }
    int len = uart_read_bytes(DAP_SWO_UART_NUM, buf, max_count, 0);
    return (len > 0) ? (uint16_t)len : 0;
}

uint32_t swo_get_buffered_count(void)
{
    size_t len = 0;
    uart_get_buffered_data_len(DAP_SWO_UART_NUM, &len);
    return (uint32_t)len;
}

uint8_t swo_get_status(void)
{
    uint8_t status = 0;
    if (s_capture_active) {
        status |= SWO_STATUS_ACTIVE;
    }
    portENTER_CRITICAL(&s_error_lock);
    status |= s_error_flags;
    portEXIT_CRITICAL(&s_error_lock);
    return status;
}
