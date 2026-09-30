// OpenOCD "cmsis-dap" 驱动的 TCP 后端（cmsis-dap backend tcp）服务端声明
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 启动 CMSIS-DAP over TCP 服务端（内部创建任务），端口通常为 3333
void cmsis_dap_tcp_start(uint16_t port);

#ifdef __cplusplus
}
#endif
