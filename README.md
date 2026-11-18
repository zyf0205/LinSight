# LinSight

基于 ESP32-S3 的手持式 CAN 总线分析仪：TWAI 抓包、PSRAM 环形缓冲、SD 黑盒记录、LVGL 本地界面与 Wi-Fi TCP 推流。设备可脱机记录，也可向 WSL 主机 `linsightd` 实时推流。

## 功能

- **CAN**：125k / 250k / 500k / 1M；LISTEN_ONLY / NORMAL；仅 CAN 2.0A/B。
- **抓包缓冲**：4 MiB PSRAM 环形队列（约 20.9 万条）；满时覆盖最旧记录并计数，序号暴露缺口。
- **SD 黑盒**：`recNNNNN.log` candump 文本；32 MiB 或 10 分钟轮转；保留最新 8 个；`.trg` 记录手动标记。
- **本地界面**：320×240 LVGL；DASH / SETUP 两页；GPIO0 短按切页、长按在 DASH 标记日志、在 SETUP 清统计（SETUP 页亦提供触控 `CLEAR` 按钮）。
- **网络推流**：Wi-Fi STA + TCP 主动连接主机；UDP 广播自动发现主机；HELLO / CAN_BATCH / EVENT / PING / PONG；断线自动重连。
- **台架工具**：`tools/can_gui.py` 在 Windows 侧一键管理 usbipd 挂载与 WSL2 `can0` 波特率配置，支持单帧/预设发包、`cangen` 连续压测与 `candump` 实时监听。

台架实测：4330 fps、96% 总线负载下零丢帧。

## 硬件

| 项 | 参数 |
|---|---|
| 开发板 | 立创·实战派 ESP32-S3（N16R8：16 MB Flash + 8 MB Octal PSRAM） |
| 屏幕 / 触摸 | 2.0" IPS 320×240 ST7789 / FT6336 |
| CAN | J2 扩展口：GPIO10 = TXD，GPIO11 = RXD，3.3 V 收发器 |
| SD | 板载 1-bit SDMMC：CLK/CMD/D0 = 47/48/21 |
| 调试 | USB Type-C，CH340K 串口 |

引脚表、接线与初始化顺序见 [`docs/hardware.md`](docs/hardware.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `main/` | 固件：`twai.c` 抓包、`can_ring.c` 环形缓冲、`sd_store.c` SD 黑盒、`net.c` 网络、`ui.c` 界面、`bsp.c` 板级、`settings.c` NVS 配置与设备标识 |
| `protocol/` | 与 WSL 主机仓库共享的线格式与解析器；两份 `frame.h` / `frame.c` 必须一致 |
| `docs/` | 固件文档；`hardware.md` 是引脚与外设的唯一事实来源 |
| `tools/can_gui.py` | Windows 台架轻量 GUI（usbipd + WSL2 `can0` 一键连线、发包、压测与监听） |
| `sdkconfig.defaults` | 默认构建配置；`sdkconfig` 为本地生成文件，不入库 |
| `partitions.csv` | 4 MB factory 应用 + 64 KB coredump |

## 构建与烧录

需要 ESP-IDF 5.5.4。

```bash
idf.py set-target esp32s3      # 首次
idf.py reconfigure             # 同步 sdkconfig.defaults（开启 Montserrat 16/20 号字体等）
idf.py menuconfig              # LinSight → Wi-Fi SSID/密码、主机回退 IP/端口、发现端口、默认 CAN 模式
idf.py build
idf.py -p COMx flash monitor
```

- 台架 Wi-Fi 与主机回退地址在 `main/Kconfig.projbuild` 提供默认值（`CONFIG_LINSIGHT_*`）；生产构建用 menuconfig 覆盖。发现端口默认 8889，必须与 `linsightd --discovery-port` 一致。
- `sdkconfig` 是本地配置，可能包含台架网络覆盖值，不要提交。

## 使用

1. **上电**：初始化屏幕、SD、CAN，然后连接 Wi-Fi 和主机。
2. **界面**：底部 DOCK 点击切页，GPIO0 短按在 DASH / SETUP 间切换；长按在 DASH 打 MARK，在 SETUP 清统计（或直接点击 SETUP 页 `CLEAR` 按钮）。SETUP 页可改比特率和控制器模式，写入 NVS；切换在独立任务中执行，不阻塞界面。背光固定 80%。
3. **CAN 安全**：默认 LISTEN_ONLY，不发送也不 ACK。只有恰好两个节点的台架才切 NORMAL。
4. **SD 记录**：插入 FAT32 卡后开机立即挂载并记录，不自动格式化；卡满或写失败时停止记录，不阻塞抓包；未插卡时每 5 秒重试挂载。
5. **主机推流**：在 WSL 的 `~/linsight` 运行 `linsightd`。设备在首次连接、Wi-Fi 重连和连续失败重试时向 UDP 8889 广播发现，使用回复中的主机 IP 和 TCP 端口；无回复时回退到 `CONFIG_LINSIGHT_HOST_IP/PORT`。只接受单个 TCP 客户端。
6. **Windows 台架**：`python tools/can_gui.py`，一键完成 usbipd attach 与 `can0` 配置，可单帧/连续发帧、监听和查看实时统计。

SD 日志行格式与 can-utils 一致，SavvyCAN 可直接打开：

```text
(开机秒.微秒) can0 ID#DATA
(12.345678) can0 123#1122334455667788
```

设备无 RTC，时间戳为开机计时。

## 文档

- 线格式、字段与解析器契约：[`protocol/frame.h`](protocol/frame.h)
- UI 接口与 DASH/SETUP 约定：[`main/ui.h`](main/ui.h)
- 引脚、CAN 接线、LCD/触摸、sdkconfig：[`docs/hardware.md`](docs/hardware.md)
- WSL 主机桥接与台架脚本：`~/linsight/README.md`

## 安全与限制

- CAN 使用 3.3 V 收发器；总线两端 120 Ω 终端并共地。
- 仅 CAN 2.0A/B；默认 LISTEN_ONLY 是安全策略，不要在有其他节点的总线上切 NORMAL。
- 主机发现是无认证的局域网 UDP 广播，仅用于可信台架网络。
- `net_task` 是固件唯一的网络写入者；SD 与网络都不阻塞 `twai_task`。
