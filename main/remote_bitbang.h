#pragma once

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// 启动 openocd remote_bitbang 协议的 TCP 服务端（阻塞运行，内部创建任务）
void remote_bitbang_start(uint16_t port);

#ifdef __cplusplus
}
#endif
