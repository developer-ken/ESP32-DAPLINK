// OpenOCD "cmsis-dap" 驱动的 TCP 后端服务端实现
//
// 协议（参照 OpenOCD 官方源码 src/jtag/drivers/cmsis_dap_tcp.c，
// 以及参考实现 github.com/bkuschak/cmsis_dap_tcp_esp32）：
//   TCP 是字节流，CMSIS-DAP 命令包长度可变，因此在每个命令/响应前加 8 字节小端包头：
//     [0..3]  uint32 signature = 0x00504144（即字节 0x44 0x41 0x50 0x00，"DAP\0"）
//     [4..5]  uint16 length    = 负载长度（不含包头）
//     [6]     uint8  type      = 0x01 请求（主机->设备）/ 0x02 响应（设备->主机）
//     [7]     uint8  reserved  = 0
//   负载即为原始 CMSIS-DAP 命令/响应字节（本实现直接复用 dap_process_command）。
//   OpenOCD 侧配置：
//     adapter driver cmsis-dap
//     cmsis-dap backend tcp
//     cmsis-dap tcp host <ip>
//     cmsis-dap tcp port 3333
#include "cmsis_dap_tcp.h"
#include "dap.h"
#include "swd_jtag.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cmsis_dap_tcp";

#define DAP_TCP_HEADER_SIZE   8
#define DAP_TCP_SIGNATURE     0x00504144u  // "DAP\0" 小端
#define DAP_TCP_PKT_REQUEST   0x01
#define DAP_TCP_PKT_RESPONSE  0x02
#define DAP_TCP_PAYLOAD_MAX   1024         // 与 OpenOCD 的 CMSIS_DAP_PACKET_SIZE 一致

// 循环读取直到收满 len 字节或出错；返回已读字节数（0=对端关闭，<0=错误）
static int recv_exact(int sock, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(sock, buf + got, len - got, 0);
        if (n <= 0) return n < 0 ? n : got;
        got += n;
    }
    return got;
}

// 循环发送直到发完 len 字节
static int send_exact(int sock, const uint8_t *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n < 0) return -1;
        sent += n;
    }
    return sent;
}

static void handle_client(int sock)
{
    // 单客户端串行处理，静态缓冲避免占用任务栈。
    // out 前 8 字节预留包头、后接响应负载，合并成一次 send，避免 header/payload 拆成两个 TCP 段。
    static uint8_t req[DAP_TCP_PAYLOAD_MAX];
    static uint8_t out[DAP_TCP_HEADER_SIZE + DAP_TCP_PAYLOAD_MAX];

    ESP_LOGI(TAG, "OpenOCD (cmsis-dap backend) 已连接");
    while (1) {
        uint8_t hdr[DAP_TCP_HEADER_SIZE];
        if (recv_exact(sock, hdr, DAP_TCP_HEADER_SIZE) <= 0) break;

        uint32_t signature = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                             ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint16_t length = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
        uint8_t  type   = hdr[6];

        if (signature != DAP_TCP_SIGNATURE || type != DAP_TCP_PKT_REQUEST) {
            ESP_LOGW(TAG, "非法包头: sig=0x%08lX type=0x%02X", (unsigned long)signature, type);
            break;
        }
        if (length == 0 || length > DAP_TCP_PAYLOAD_MAX) {
            ESP_LOGW(TAG, "非法请求长度: %u", length);
            break;
        }
        if (recv_exact(sock, req, length) <= 0) break;

        // 响应负载直接写到 out 的包头之后，包头与负载合并成一次 send
        uint16_t resp_len = dap_process_command(req, length, out + DAP_TCP_HEADER_SIZE);

        out[0] = 0x44; out[1] = 0x41; out[2] = 0x50; out[3] = 0x00;
        out[4] = (uint8_t)(resp_len & 0xFF);
        out[5] = (uint8_t)((resp_len >> 8) & 0xFF);
        out[6] = DAP_TCP_PKT_RESPONSE;
        out[7] = 0x00;

        if (send_exact(sock, out, DAP_TCP_HEADER_SIZE + resp_len) < 0) break;
    }

    // 客户端断开：复位调试口并熄灭紫灯
    dap_io_disconnect();
    dap_led_set_connected(false);
    close(sock);
    ESP_LOGI(TAG, "OpenOCD (cmsis-dap backend) 已断开");
}

static void server_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "创建 socket 失败");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "监听端口 %u 失败", port);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "CMSIS-DAP TCP 服务已启动，端口 %u", port);
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) continue;

        // 关闭 Nagle 以降低延迟（OpenOCD 侧同样设置）
        int one = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // 加大收发缓冲，提升窗口吞吐、减少流控停顿（WiFi 下默认 5~8KB 偏小）
        int sndbuf = 32 * 1024;
        int rcvbuf = 32 * 1024;
        setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        handle_client(client_sock);
    }
}

void cmsis_dap_tcp_start(uint16_t port)
{
    // TCP 传输用大包 + 多缓冲，显著减少 WiFi 往返次数（参考 OpenOCD 后端 CMSIS_DAP_PACKET_SIZE=1024，
    // 以及参考实现 bkuschak/cmsis_dap_tcp_esp32 的 DAP_PACKET_COUNT=8、OpenOCD 上限 4）。
    dap_set_packet_size(DAP_TCP_PAYLOAD_MAX);
    dap_set_packet_count(4);
    xTaskCreate(server_task, "cmsis_dap_tcp", 4096, (void *)(uintptr_t)port, 5, NULL);
}
