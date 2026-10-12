# AGENTS.md — ESP32-DAPLINK（Eggy CMSIS-DAP）

> 新会话请先读本文件，再按需读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)（模块/数据流/引脚/协议细节）。
> 代码注释与日志均为**中文**，新增内容请保持一致。

## 项目是什么

基于 **ESP32-S3** 的 CMSIS-DAP 调试器固件（ESP-IDF 6.1，`dependencies.lock` 锁定；`main/idf_component.yml` 要求 `>=5.3`），
用 GPIO 位带（bit-bang）实现 SWD / JTAG，另有 SWO(UART) trace。两种运行模式，上电自动选择：

1. **USB 模式**（首选）：原生 USB-OTG 枚举为复合设备 —— CMSIS-DAP v1（HID）+ CMSIS-DAP v2（WinUSB Bulk）+ MSC（FAT 分区，存放 README / `wifi.cfg` / 裁剪版 OpenOCD）。
2. **Wi-Fi 模式**（回退）：`USB_MOUNT_WAIT_MS`(3s) 内未被主机枚举 → 连 Wi-Fi，起 mDNS（`eggydebugger.local`）+ 两个 TCP 服务：
   `3335` OpenOCD `remote_bitbang`、`3333` OpenOCD `cmsis-dap backend tcp`（**二选一使用**）。

## 目录速览

| 路径 | 说明 |
|---|---|
| `main/main.cpp` | `app_main`：LED/NVS/DAP/MSC 初始化 → 装 TinyUSB → 等待挂载 → 决定 USB 或 Wi-Fi 模式 |
| `main/dap_config.h` | **所有引脚、USB VID/PID/字符串、Wi-Fi 默认值、端口、时序校准常量的唯一配置点** |
| `main/dap.c/.h` | CMSIS-DAP 命令解析（`dap_process_command`），SWD/JTAG 传输协议层，紫灯状态 |
| `main/swd_jtag.c/.h` | 底层 GPIO 位带驱动（时序、方向切换、时钟档位）。**对时序极度敏感** |
| `main/swo.c/.h` | SWO：UART1 经 GPIO 矩阵接收（引脚与 TDO 共用），8KB 环形缓冲，Bulk 流式输出 |
| `main/usb_descriptors.c/.h` | 设备/配置/BOS/MS OS 2.0(WinUSB) 描述符；HID / Vendor Bulk 回调 |
| `main/msc_disk.c/.h` | `storage` 分区挂载为 FAT → 生成 README.TXT 与 `wifi.cfg` → 交给 USB 主机 |
| `main/wifi_bridge.c/.h` | STA 连接（配置文件优先，否则扫描 SSID 含 `DEBUG`）→ mDNS → 启动两个 TCP 服务 |
| `main/wifi_config.c/.h` | 解析 `/storage/wifi.cfg`（`KEY=VALUE`） |
| `main/cmsis_dap_tcp.c/.h` | OpenOCD cmsis-dap TCP 后端（8 字节包头 `DAP\0`），复用 `dap_process_command` |
| `main/remote_bitbang.c/.h` | OpenOCD remote_bitbang 服务（`0-7`/`R`/`Q`/`r s t u`），直接操作 JTAG 引脚 |
| `main/status_led.c/.h` | 全局 LED 任务：红=连接模式，紫=DAP 状态（低电平点亮） |
| `partitions.csv` | nvs / phy_init / factory(1.19MB) / storage(FAT, 2.75MB)；Flash 4MB |
| `sdkconfig.defaults` / `sdkconfig` | `sdkconfig` **已纳入 git**（见下方"坑"） |
| `managed_components/`, `build/` | 自动生成，已 gitignore，勿手改 |

## 构建 / 烧录 / 调试

- 目标芯片 `esp32s3`；开发环境为 Windows + VS Code ESP-IDF 扩展（`idf.currentSetup` 指向 `C:\esp\v6.1\esp-idf`，`idf.port` 见 `.vscode/settings.json`）。
- 命令行（需先 `export.ps1` 载入 IDF 环境）：`idf.py set-target esp32s3`、`idf.py build`、`idf.py -p COMx flash monitor`。
- 在 VS Code 中优先用 `espIdfCommands` 工具（build / flash / monitor / menuconfig 等）。
- 仓库**没有单元测试**；验证靠编译通过 + 真机（示波器测 SWCLK、OpenOCD/pyOCD 连接目标）。
- USB-Serial-JTAG 控制台已禁用（`CONFIG_ESP_CONSOLE_SECONDARY_NONE`），否则会抢占内部 USB PHY 导致自定义复合设备无法枚举；因此日志/烧录需走 UART0（外接串口）。

## 开发约定

- 引脚/参数改动只改 `dap_config.h`；不要把常量散落到各 .c 文件。
- 业务层改 LED 一律用 `status_led_set()`，不要直接操作 GPIO。紫灯由 `dap.c`/`remote_bitbang.c` 经 `dap_led_set_connected/running` 维护，红灯由 `main.cpp`/`wifi_bridge.c` 维护。
- 协议层（`dap.c`）与 IO 层（`swd_jtag.c`）严格分离：`dap.c` 只调 `dap_io_*`。
- 单个 CMSIS-DAP 命令处理函数 `dap_process_command(req, len, resp)` 是三条传输路径（HID / Bulk / TCP）的**共同入口**，改它要三条路径都考虑。
- 文件编码 UTF-8；注释只写必要的“为什么”。

## 关键陷阱（改动前务必读）

1. **位带时序热路径**：`swd_jtag.c` 里 `half_period_delay` 强制内联、放 `IRAM_ATTR`、用 `gpio_ll_*` 无锁寄存器操作、方向切换只翻 output-enable 位。**不要**在其中改用 `gpio_set_level/gpio_set_direction/esp_rom_delay_us`，会引入抖动（曾表现为 ACK 读到 JUNK、频率不准）。高频（>500kHz）档用 `s_io_mux` 自旋锁保护单段位序列。
2. **时钟档位**：`dap_io_set_clock` 用编译期 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`（240MHz）换算周期，不要换成 `esp_rom_get_cpu_ticks_per_us()`。fast 档（半周期 ≤ `DAP_FAST_OVERHEAD_CYCLES`）约 4.7–5MHz 且**不受请求频率控制**；校准常量需示波器实测。改 CPU 频率后需重新校准。
3. **sdkconfig 与 sdkconfig.defaults 已对齐**：Vendor(Bulk) 接口需要 `CONFIG_TINYUSB_VENDOR_COUNT=1`（RX 64 / TX 512 / EPSIZE 512），两个文件现在一致，删除 `sdkconfig` 重新生成也不会丢失。
4. `sdkconfig.defaults` 为 UTF-8（无 BOM）；用默认编码的 PowerShell `Get-Content` 读取会显示成乱码，属显示问题，勿据此“修复”文件。
5. **MSC 无只读开关**：esp_tinyusb 2.3.0 未提供，主机仍可写 `storage` 分区；“只读”仅是使用约定。`wifi.cfg` 通过 MSC 被用户编辑，固件仅在启动时（`msc_disk_init`，挂载到 APP 端时）读取一次。
6. **MSC 挂载切换**：`msc_disk_init` 先把存储挂到 APP 端写 README/wifi.cfg 并 `wifi_config_load()`，之后再 `tinyusb_msc_set_storage_mount_point(..., USB)` 交给主机。Wi-Fi 模式下读配置依赖此次加载。
7. **HID/Bulk 包大小 = 64**（`DAP_PACKET_SIZE`，`DAP_PACKET_COUNT=1`）；TCP 模式在 `cmsis_dap_tcp_start` 里**全局**改成 1024/4（`dap_set_packet_size/count`）。
8. `remote_bitbang` 的 `r/s/t/u` 复位映射沿用开源实现约定，未对所有 OpenOCD 版本验证。
9. SWO 仅支持 UART 模式（Manchester 未实现）；`SWO_TRANSPORT_WINUSB` 经 Vendor Bulk IN 流式输出，`SWO_TRANSPORT_DATA` 走 `DAP_SWO_Data` 轮询。
10. 存在硬编码默认 Wi-Fi 密码（`WIFI_DEBUG_FIXED_PASSWORD`）与序列号 `EGGY0001`（`usb_descriptors.c`、`dap.c` 各一处），改序列号需两处同步。

## 提交历史脉络（便于理解演进）

USB HID 识别 → SWD 烧录调试成功 → 速度优化（1MHz → ~4.7MHz，频率不准）→ 指示灯 → OpenOCD cmsis-dap over TCP → 增大 storage 以放裁剪版 OpenOCD → `wifi.cfg` 配置网络参数 → SWO NRZ(UART) → USB Bulk(CMSIS-DAP v2) 支持。

## 文档维护

改动模块、引脚、端口、协议或新增文件后，请同步更新本文件与 `docs/ARCHITECTURE.md`。
