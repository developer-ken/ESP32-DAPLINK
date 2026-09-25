// openocd "remote_bitbang" 接口的 TCP 服务端实现
// 协议（逐字节）：
//   '0'-'7' : 设置 TCK(bit2)/TMS(bit1)/TDI(bit0) 电平（不隐含时钟脉冲，电平变化即完成一次采样窗口）
//   'R'     : 采样 TDO，返回 ASCII '0'/'1'
//   'Q'     : 断开连接
//   'B'/'b' : blink on/off（本实现忽略）
//   'r'/'s'/'t'/'u' : 复位控制，映射为 (trst,srst) = (0,0)/(0,1)/(1,0)/(1,1)
//                     该映射沿用主流开源实现的约定，如与具体 OpenOCD 版本行为不完全一致，可按需调整
#include "remote_bitbang.h"
#include "swd_jtag.h"
#include "dap_config.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "remote_bitbang";

static void handle_client(int sock)
{
    dap_io_connect(DAP_PORT_JTAG);
    dap_io_set_clock(1000000);

    uint8_t buf[128];
    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len <= 0) break;
        for (int i = 0; i < len; i++) {
            uint8_t c = buf[i];
            if (c >= '0' && c <= '7') {
                uint8_t v = c - '0';
                dap_io_jtag_set_pins((v >> 2) & 1, (v >> 1) & 1, v & 1);
            } else if (c == 'R') {
                uint8_t reply = dap_io_jtag_get_tdo() ? '1' : '0';
                send(sock, &reply, 1, 0);
            } else if (c == 'Q') {
                dap_io_disconnect();
                close(sock);
                return;
            } else if (c == 'r' || c == 's' || c == 't' || c == 'u') {
                uint8_t idx = c - 'r';
                bool trst = idx & 0x02;
                bool srst = idx & 0x01;
                dap_io_set_nreset(!srst);
                dap_io_set_ntrst(!trst);
            }
            // 'B'/'b' 等未识别字符：忽略
        }
    }
    dap_io_disconnect();
    close(sock);
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

    ESP_LOGI(TAG, "remote_bitbang 服务已启动，端口 %u", port);
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) continue;
        ESP_LOGI(TAG, "OpenOCD 已连接");
        handle_client(client_sock);
        ESP_LOGI(TAG, "OpenOCD 已断开");
    }
}

void remote_bitbang_start(uint16_t port)
{
    xTaskCreate(server_task, "remote_bitbang", 4096, (void *)(uintptr_t)port, 5, NULL);
}
