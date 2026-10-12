# 架构说明

固件目标：ESP32-S3（4MB Flash，240MHz），ESP-IDF 6.1。入口 `main/main.cpp::app_main`。

## 1. 启动流程

```
app_main
 ├─ status_led_init()            红灯 OFF（启动/故障）
 ├─ nvs_flash_init()
 ├─ dap_init()                   dap_io_init() + swo_init()（UART1 + swo_stream 任务）
 ├─ msc_disk_init()              挂载 storage(FAT) → 生成 README.TXT/wifi.cfg → wifi_config_load() → 交给 USB 主机
 ├─ usb_dap_init()               注册 SWO 流式 sink（Vendor Bulk IN）
 ├─ tinyusb_driver_install()     使用 usb_descriptors.c 里的自定义描述符
 ├─ 轮询 tud_mounted() 最多 USB_MOUNT_WAIT_MS(1000ms)
 ├─ 成功 → 红灯常亮，USB 模式（HID/Bulk 回调驱动 DAP）
 └─ 失败 → while(!wifi_bridge_start()) 每秒重试    → Wi-Fi 模式
```

## 2. 三条命令入口（共用 `dap_process_command`）

```
USB HID  (v1)  tud_hid_set_report_cb ──┐
USB Bulk (v2)  tud_vendor_rx_cb     ───┼─→ dap_process_command(req,len,resp) ─→ dap_io_* (swd_jtag.c) ─→ GPIO
TCP 3333       cmsis_dap_tcp.c      ───┘
TCP 3335       remote_bitbang.c  ──────────→ dap_io_jtag_set_pins / get_tdo（绕过 dap.c，直接逐位 JTAG）
```

- HID 报告 64 字节，单包；Bulk 端点 64 字节（全速）。`DAP_Info` 的 0xFF/0xFE 上报包大小/包数量，默认 64/1。
- TCP 后端：每个报文 8 字节小端包头 `44 41 50 00 | len(u16) | type(1=req,2=resp) | 0`，负载为原始 CMSIS-DAP 字节，最大 1024；`cmsis_dap_tcp_start` 会把全局包参数改为 1024/4。单客户端串行，断开时 `dap_io_disconnect()` + 紫灯灭。
- remote_bitbang：`'0'-'7'` 设 TCK/TMS/TDI（bit2/1/0），`'R'` 读 TDO 回 `'0'/'1'`，`'Q'` 退出，`'r' 's' 't' 'u'` 复位控制，`'B' 'b'` 忽略。连接时固定 JTAG @ 1MHz。

### dap.c 支持的命令
Info、HostStatus、Connect/Disconnect、TransferConfigure、Transfer、TransferBlock、TransferAbort、WriteABORT、Delay、ResetTarget、SWJ_Pins/Clock/Sequence、SWD_Configure、JTAG_Sequence/Configure/IDCODE、SWO_Transport/Mode/Baudrate/Control/Status/Data/ExtendedStatus。
- `DAP_Info 0xF0`（能力）= `0x07`：SWD + JTAG + SWO UART。`0xFD` 上报 SWO 缓冲 8192。
- JTAG 最多 4 个设备（`MAX_JTAG_DEVICES`），DPACC=0xA / APACC=0xB；SWD 读后 WAIT 重试由 `s_transfer_wait_retry` 控制。

## 3. USB 复合设备（`usb_descriptors.c`）

| 接口 | 类型 | 端点 | 说明 |
|---|---|---|---|
| 0 | HID In/Out | 0x01 / 0x81 | CMSIS-DAP v1，自定义 vendor report，64B |
| 1 | Vendor Bulk | 0x02 / 0x82 | CMSIS-DAP v2，WinUSB（BOS + MS OS 2.0 描述符，GUID `{3D0A7A5E-2A41-4B6F-9E6C-1F2D3A4B5C6D}`） |
| 2 | MSC | 0x03 / 0x83 | storage 分区 |

VID/PID = `0x303A/0x8001`，`bcdUSB=0x0210`（BOS 需要）。字符串：`Eggy` / `Eggy CMSIS-DAP` / 序列号 `EGGY0001` / `Eggy CMSIS-DAP HID` / `Eggy Storage` / `Eggy CMSIS-DAP v2`。SWO 流式数据通过同一个 Bulk IN 端点发送（无头原始字节流）。

## 4. SWD / JTAG 位带（`swd_jtag.c`）

- 引脚配置一次性在 `dap_io_init` 完成；connect/turnaround 仅翻转 output-enable，保证时序确定。
- SWCLK 与 TCK 同脚、SWDIO 与 TMS 同脚、SWO 与 TDO 同脚。
- 时钟档位（`dap_io_set_clock`，以 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` 计）：
  - 半周期 ≤ `DAP_FAST_OVERHEAD_CYCLES`(24)：**fast 档**，6 个 nop，约 4.7–5MHz，不随请求频率变化；
  - 半周期 > `DAP_SLOW_OVERHEAD_CYCLES`(24)：**slow 档**，忙等 `half_cycles - 24`；
  - 中间区间：忙等 0，输出 slow 档固有最快速率。
- >500kHz 时使用 `s_io_mux` 自旋锁临界区保护单段位序列（防 Wi-Fi/Tick ISR 拉长半周期）。

## 5. SWO（`swo.c`）

UART1 通过 GPIO 矩阵把 `DAP_SWO_PIN`(GPIO9) 路由到 RX，8N1，默认 1Mbaud，驱动缓冲 8192。`swo_stream` 任务（优先级 10）为 UART 事件队列唯一消费者：
- 数据事件 + `SWO_TRANSPORT_WINUSB` + 捕获激活 → 以 ≤512 字节块送到 `s_stream_sink`（USB Bulk IN）；
- FIFO 溢出/帧错误 → 累积为 `SWO_STATUS_OVERRUN / STREAM_ERROR`，由 `DAP_SWO_Status/Data` 返回。
- `SWO_TRANSPORT_DATA` 走 `DAP_SWO_Data` 轮询 `swo_read`。Manchester 未实现。

## 6. Wi-Fi 桥（`wifi_bridge.c`）

1. 读取 `wifi_config_get()`：有静态 IP 则关 DHCP 并设置 IP。
2. 配置了 `WIFI_SSID` → 先连该热点（超时 8s）。
3. 失败/未配置 → 扫描，对 SSID 含 `DEBUG` 的热点依次尝试：密码 `WIFI_DEBUG_FIXED_PASSWORD`，再试 SSID 本身作密码。
4. 连接成功 → `mdns_init`，主机名取配置 `MDNS_HOSTNAME`，默认 `eggydebugger`；注册 `_openocd._tcp:3335`、`_cmsis-dap._tcp:3333`；启动 `remote_bitbang_start(3335)` 与 `cmsis_dap_tcp_start(3333)`。
5. 返回 false 则 `app_main` 每秒重试。netif / 事件循环 / `esp_wifi_init` / 静态 IP / `esp_wifi_start` 只在首次调用时执行（`wifi_bridge_init_once`），重试只重复"连接配置热点 → 扫描 → 逐个尝试"；扫描前会先 `esp_wifi_disconnect()`，扫描失败按失败返回而非 abort。

`wifi.cfg`（`/storage/wifi.cfg`）键：`WIFI_SSID`、`WIFI_PASSWORD`、`IP_ADDR`、`IP_NETMASK`、`IP_GATEWAY`（三项齐才启用静态 IP）、`MDNS_HOSTNAME`；`#` 开头为注释，首次生成的模板全部注释。

## 7. 引脚（`dap_config.h`）

| 功能 | GPIO | 备注 |
|---|---|---|
| SWCLK / TCK | 36 | |
| SWDIO / TMS | 38 | 双向，需上拉 |
| TDI | 34 | |
| TDO / SWO | 9 | SWO 走 UART1 RX |
| nRESET | 35 | 开漏，低有效 |
| nTRST | 33 | `DAP_JTAG_HAS_TRST=1` |
| 红灯 | 6 | 低电平亮：连接模式 |
| 紫灯 | 8 | 低电平亮：DAP 状态 |

## 8. 指示灯语义（`status_led.c`，低电平点亮）

- 红：熄灭=启动/故障；快闪(~5Hz)=Wi-Fi 搜索；慢闪(~2Hz)=连接/取 IP 中；常亮=USB 已连或 Wi-Fi 已拿到 IP。
- 紫：熄灭=目标未连接；常亮=已连接；快闪=目标运行中（`DAP_HostStatus` type≠0 的 running 状态）。

## 9. 分区与存储

`partitions.csv`：`nvs`(0x9000,24KB) / `phy_init`(0xF000) / `factory`(0x10000,0x130000) / `storage`(FAT,0x2C0000≈2.75MB，wear-levelling)。storage 内容：`README.TXT`、`wifi.cfg`，并预期放入裁剪版 OpenOCD 与配置（由用户/发布流程放置，固件不生成）。

## 10. 依赖

`espressif/esp_tinyusb ^2.3.0`、`espressif/mdns ^1.2`（`main/idf_component.yml`）；IDF 组件：driver、esp_wifi、esp_netif、esp_event、nvs_flash、fatfs、wear_levelling、vfs、esp_partition、esp_rom、lwip。
