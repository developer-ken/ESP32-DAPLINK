#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 扫描包含 WIFI_DEBUG_SSID_KEYWORD 的热点并尝试连接；
// 连接成功后启动 mDNS(eggydebugger.local) 与 remote_bitbang TCP 调试服务。
// 返回 true 表示已成功连接并完成调试服务初始化。
bool wifi_bridge_start(void);

#ifdef __cplusplus
}
#endif
