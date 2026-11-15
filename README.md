# LinSight

基于 ESP32-S3 的手持式 CAN 总线分析仪固件：TWAI 抓包、PSRAM 环形缓冲、SD 黑盒记录、LVGL 界面与 Wi-Fi TCP 推流。

## 功能

- **CAN 2.0A/B**：125k / 250k / 500k / 1M 比特率；LISTEN_ONLY / NORMAL / SELFTEST 三种模式。
- **抓包缓冲**：4 MiB PSRAM 环形队列（约 209k 条记录），满时丢弃最旧记录并计数。
- **SD 黑盒**：`recNNNNN.log` candump 文本，32 MiB 或 10 分钟轮转，保留最新 8 个文件；`.trg` 记录手动标记。
- **本地 UI**：320×240 LVGL，DASH / DIAG / SETUP 三页，GPIO0 按键切换。
- **网络推流**：Wi-Fi STA + TCP，向主机 `linsightd` 发送 HELLO / CAN_BATCH / EVENT / PING / PONG。
- **台架工具**：`tools/can_gui.py`，Windows 侧通过 usbipd 连接 WSL2 SocketCAN。

## 硬件

| 项 | 参数 |
|---|---|
| 开发板 | 立创·实战派 ESP32-S3（N16R8：16 MB Flash + 8 MB Octal PSRAM） |
| 屏幕 / 触摸 | 2.0" IPS 320×240 ST7789 / FT6336 |
| CAN | J2 扩展口：GPIO10 = TXD，GPIO11 = RXD，3.3 V 收发器 |
| SD | 板载 1-bit SDMMC：CLK/CMD/D0 = 47/48/21 |
| 调试 | USB Type-C，CH340K 串口 |

完整引脚、LCD/touch 初始化顺序与 sdkconfig 注意事项见 [`docs/hardware.md`](docs/hardware.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `main/` | 固件：`twai.c` 抓包、`can_ring.c` 环形缓冲、`sd_store.c` SD 黑盒、`net.c` 网络、`ui.c` LVGL 界面、`bsp.c` 板级初始化 |
| `protocol/` | 与 WSL 主机仓库共享的帧格式与解析器（**两份必须保持一致**，见 `protocol/frame.h`） |
| `docs/` | 固件文档；`hardware.md` 是引脚与外设的唯一事实来源 |
| `tools/can_gui.py` | Windows 台架 GUI：连接 WSL、发帧、cangen、监听/过滤/CSV、统计、日志 |
| `sdkconfig.defaults` | 默认构建配置；`sdkconfig` 为本地生成文件，不入库 |
| `partitions.csv` | 4 MB factory 应用 + 64 KB coredump |

## 构建与烧录

需要 **ESP-IDF 5.5.4**。

```bash
idf.py set-target esp32s3      # 首次
idf.py menuconfig              # LinSight → Wi-Fi SSID/密码、主机 IP/端口、默认 CAN 模式
idf.py build
idf.py -p COMx flash monitor
```

- 台架 Wi-Fi 与主机地址在 `main/Kconfig.projbuild` 中提供默认值（`CONFIG_LINSIGHT_*`）；生产构建请用 menuconfig 覆盖。
- `sdkconfig` 是本地配置，不要提交或删除；它可能包含台架网络的覆盖值。

## 使用

1. **上电**：设备初始化屏幕、SD 卡与 CAN，然后连接 Wi-Fi。
2. **界面**：底部 DOCK 或 GPIO0 短按在 DASH → DIAG → SETUP 间切换；GPIO0 长按执行当前页动作（DASH 暂停、DIAG 清统计、SETUP 返回 DASH）；熄屏时任意按键先唤醒。
3. **CAN 安全**：默认 `LISTEN_ONLY`，不发送、不应答 ACK。只有在**恰好两个节点**的台架上才切换到 `NORMAL`。
4. **SD 记录**：插入 FAT32 卡后自动挂载并开始记录；卡满或写失败会停止记录，不会阻塞抓包。
5. **主机推流**：在 WSL 的 `~/linsight` 仓库运行 `linsightd`，设备按 `CONFIG_LINSIGHT_HOST_IP/PORT` 主动连接。
6. **Windows 台架**：`python tools/can_gui.py`，通过 usbipd 把 PCAN-USB 挂入 WSL，并对 `can0` 做配置、发帧和监听。PCAN 驱动加载与 `can0` 创建以主机仓库脚本/手动步骤为准。

SD 日志行格式（与 can-utils 一致，可由 SavvyCAN 打开）：

```text
(开机秒.微秒) can0 ID#DATA
(12.345678) can0 123#1122334455667788
```

设备无 RTC，时间戳为开机计时。

## 协议与文档

- 线格式、字段与解析器契约：[`protocol/frame.h`](protocol/frame.h)
- 固件 UI 接口与 DASH/DIAG/SETUP 约定：[`main/ui.h`](main/ui.h)
- 引脚、CAN 接线、LCD/触摸、sdkconfig：[`docs/hardware.md`](docs/hardware.md)
- WSL 主机侧桥接与台架说明：`~/linsight/README.md`

## 安全与限制

- CAN 侧使用 **3.3 V** 收发器；总线两端 120 Ω 终端并共地。
- 仅支持 CAN 2.0A/B；默认 LISTEN_ONLY 是安全策略，不要在有其他节点的总线上随意切 NORMAL。
- 主机只允许单个 TCP 客户端；`net_task` 是固件唯一的网络写入者。

## 验证状态

- ESP-IDF 5.5.4 `idf.py build` 通过。
- SD 挂载/重挂载、跨复位序号连续、按字节/时间轮转与保留 8 文件已在台架验证（2026-11-13）。
- CAN 抓包在 4330 fps / 96% 负载下零丢帧；网络与 SD 的详细台架数据见 `CLAUDE.md` 与主机仓库文档。
