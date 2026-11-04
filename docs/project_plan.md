# LinSight 项目计划：无线 CAN 总线分析仪

> ESP32-S3 手持 CAN 分析仪 + Linux 分析服务。设备独立工作：采集/显示/触发；
> 主机负责长时记录、解码、回放与对照。硬件见 [hardware.md](hardware.md)。

## 1. 定位

- 用户：汽车/工业现场调试人员；不接电脑也能看总线，接电脑可长时记录和分析。
- 卖点：实时采集 + 本地触摸 UI + Wi-Fi 流式 + Linux 主机工具链，不是“桌面副屏”。
- 复用：LVGL/PSRAM/SPI DMA、共享协议解析器、host 线程/队列/测试。
- 不做：CAN FD（S3 不支持）、多通道、多设备集群、内核驱动、云端、GUI 上位机。

## 2. 验收（P0）

| 项 | 标准 |
|---|---|
| 采集 | 500 kbps / 80% 负载，连续 10 min 丢帧 0（与 USB-CAN 对照） |
| 时间戳 | 分辨率 ≤1 ms；主机日志可按时间排序 |
| UI | trace 20 fps；触摸响应 <100 ms；断网不影响本地显示 |
| 主机 | 3000 帧/s 持续写入不丢；断线 5 s 内重连；24 h soak 无崩溃/泄漏 |
| 导出 | ASC/candump 可被 can-utils/Vector 读取；回放时间误差 <5% |
| 故障 | bus-off 自动恢复；ring 溢出计数可见；磁盘满不崩溃 |
| 测试 | 单元/集成/HIL 全绿；协议解析器可 fuzz |

P1：1 Mbps 短时、触发/过滤、DBC、双路对照、systemd。P2：发送、BLE 配网、签名 OTA、SQLite、TF 卡。

## 3. 架构

```text
CAN 总线 ── SN65HVD230 ── ESP32-S3
                            ├─ twai_task ─→ PSRAM ring ─→ ui_task（LVGL trace/统计）
                            │                         └→ net_task ─Wi-Fi/TCP→ linsightd
                            └─ 触摸/用户键
USB-CAN ── SocketCAN(can0) ── linsightd（对照/回放）
linsightd ─→ .lsc 日志 / ASC 导出 / linsight-cli（Unix socket）
```

- 固件任务：twai_task（核 0，优先级 10，收帧/时间戳/告警）；net_task（核 0，5，批量/控制/重连）；ui_task（核 1，4，20 Hz 快照）；stats（1 Hz）。
- UI：Monitor（顶部链路/bitrate/bus 状态，中间 8 行 trace：ID/DLC/data/count，底部 fps/负载/错误/丢帧）；Settings（125/250/500/1000 kbps、ID 过滤、暂停、清统计）；用户键短按暂停、长按设置。
- 内存：ring 1 MB PSRAM（约 5 万条）；批量 16 KB；draw buffer 沿用内部 DMA。
- 主机线程：main/epoll（listener、设备/Unix socket、signalfd/timerfd）；writer（有界 ring → `.lsc`，慢盘丢旧记录并计数）；reference（可选 SocketCAN 对照）。

## 4. 协议 v2.0

5 字节帧头 `AA 55 type len_lo len_hi`，复用 `protocol/frame.c`。

| type | 方向 | 负载 |
|---|---|---|
| 01 HELLO | 设备→主机 | proto、fw、device id、bitrate |
| 02 CAN_BATCH | 设备→主机 | `u16 count` + N 条记录 |
| 03 STATS | 设备→主机 | fps、负载、总数、丢帧、TEC/REC、状态 |
| 04 EVENT | 设备→主机 | bus-off / error-passive / overflow |
| 05 PING / 06 PONG | 双向 | 4 B 回显，测 RTT/保活 |
| 07 CONTROL | 主机→设备 | 过滤、暂停/恢复、清统计、发送（P2） |
| 08 ACK | 设备→主机 | id、code（应答 CONTROL） |
| 09 FLOW | 主机→设备 | 最大批速率/暂停 |

记录 20 B：`seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`；flags：bit0 EXT、bit1 RTR、bit2 ERR。批次 ≤16 KB（约 800 条）。断网继续采集，重连只发新帧；丢帧在 STATS/UI 显示。

## 5. 主机软件

- `linsightd`：epoll + 非阻塞 TCP；协议解析；ring + writer 线程；Unix socket 控制；SIGTERM 优雅退出。用法 `linsightd [--port N] [--ctl PATH] [--log-dir DIR]`。
- `linsight-cli`：`stats`、`live`、`pause`/`resume`/`clear`（转发设备 CONTROL）、`export asc|candump FILE.lsc [-o OUT]`。回放/对照在 M4。
- `.lsc`：32 B 文件头（magic "LSC1"/version/bitrate/起始时间）+ 20 B 记录；尾部截断可恢复读取。
- DBC/绘图用 Python + cantools/matplotlib；`--reference can0` 双路对照报告缺失/重复/延迟分位（P1）。
- 设备可独立工作；主机是增强，不是依赖。

## 6. 代码结构（目标）

| 位置 | 模块 |
|---|---|
| `main/twai.c` / `can_ring.c` | 初始化、收帧、ring、序号、统计快照 |
| `main/net.c` / `ui.c` | 批量/控制/重连；trace、统计条、设置页 |
| `host/src/main.c` / `canlog.c` / `cli/cli.c` | epoll daemon、`.lsc`/ASC/candump、CLI 与 Unix socket |
| `protocol/frame.c` / `can.c` | 帧解析、CAN 记录编解码（两端共用，无分配） |

## 7. 里程碑

| 阶段 | 工时 | 交付 | 验收 |
|---|---|---|---|
| M0 台架 ✅ | 2d | USB-CAN + 3.3V 收发器（GPIO10=TX，GPIO11=RX）；TWAI listen-only 500k | 139/139 帧逐字节比对通过，中位间隔 10.1 ms |
| M1 设备 UI ✅ | 1w | ring、trace、统计、设置、Wi-Fi 流式 | 自测突发 30000 帧 @4330 fps（96% 负载）零丢失；ring 52428 条无溢出 |
| M2 主机 MVP ✅ | 1w | epoll daemon、`.lsc`、CLI stats/live/export、CONTROL/ACK | ctest 3/3 绿（单元 + 假设备集成）；慢速写盘路径有界 ring |
| M3 健壮性 | 1w | 过滤、触发、bus-off 恢复验证、双路对照 | 注入故障可恢复 |
| M4 工具链 | 1w | 回放、DBC、systemd、fuzz/CI | ASC 可读，fuzz 全绿 |
| M5 soak/演示 | 3–5d | 24h soak、文档、演示视频 | P0 指标 |

M0–M2 已完成（2026-11）。bus-off 自动恢复与 overflow 计数已在 M1 实现；M3 待做真实总线对照。

## 8. 复杂度与风险

- 单设备/单通道/单主机；UI 两屏；主机只做 CLI；DBC/绘图放 Python。
- 删除旧业务：JPEG/投屏、`/proc` 仪表盘、宏键盘。
- 核心 C 目标：固件 ≤1000 行、host ≤1500 行、CLI ≤700 行；先 listen-only 闭环。
- 关键决策：批量+ring；丢旧保新、统计不丢；UI 快照不阻塞采集；发送默认关闭。
- 风险：收发器/终端/共地接错（3.3V、120Ω、先 listen-only）；高负载丢帧（高优先级任务+ring+对照）；Wi-Fi 抖动（本地缓冲+重连）；写盘阻塞（writer 线程+有界 ring）；范围膨胀（P0/P1/P2 裁剪）。

## 9. 简历写法

> **LinSight CAN — 无线 CAN 总线分析仪（ESP32-S3 + Linux）**
> - TWAI 500 kbps/1 Mbps 采集，PSRAM 环形缓冲与时间戳；LVGL 触摸屏实时显示 trace、总线负载、错误计数，20 fps。
> - Linux C 守护进程：epoll + 非阻塞 TCP + 环形缓冲 + 二进制日志/ASC 导出 + Unix socket CLI；SocketCAN 回放与 USB-CAN 双路对照。
> - 500 kbps/80% 负载 10 min 零丢帧；bus-off 自动恢复；24 h soak 无泄漏；协议解析器 fuzz + CI。
> - 可选：DBC 解码、触发/过滤、签名 OTA。

数字（丢帧、fps、延迟、soak）实现后按实测填写，不预先写死。
