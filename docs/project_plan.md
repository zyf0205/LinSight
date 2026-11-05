# LinSight 项目计划：无线 CAN 总线分析仪

> ESP32-S3 手持 CAN 分析仪 + Linux 分析服务。设备独立工作：采集/显示/触发；
> 主机负责长时记录、解码、回放与对照。接线见 [hardware.md](hardware.md)，UI 见
> [ui_design.md](ui_design.md)，线格式以 `protocol/frame.h` 为准。

## 1. 定位

- 用户：汽车/工业现场调试人员；不接电脑也能看总线，接电脑可长时记录和分析。
- 卖点：实时采集 + 本地触摸 UI + Wi-Fi 流式 + Linux 主机工具链，不是“桌面副屏”。
- 不做：CAN FD（S3 不支持）、多通道、多设备集群、内核驱动、云端。
- `host/gui` 只是台架测试台；产品级主机 UI 在 M5 重做。

## 2. 现状与本次修复（2026-11-05）

M0–M2 已完成：采集、LVGL UI、Wi-Fi 流式、daemon/CLI/`.lsc`、CONTROL/ACK 均跑通。

台架“持续发送就卡死、必须重插 USB-CAN”的根因链：任一次 ≥100 ms 的 ACK 中断
（设备重启/掉电/毛刺）→ PCAN 无 ACK 重发 → 硬件 bus-off；python-can 默认不自动复位、
GUI 不读 status 也不 reset → `send()` 永远抛错但 `sent > 0` 使循环不退出；设备端
恢复需 128×11 隐性位静默，PCAN 持续重发与其互锁。

已落地：
- GUI：`auto_reset=True`、status 解码、检测 BUSOFF 后 `reset()` + 0.5 s 退避、
  连续 200 次失败熔断；不再无限自旋。
- 固件：net_task 的 UI 调用改为无锁邮箱（消除 `lvgl_port_lock(0)` 永久等待）；
  `SO_RCVTIMEO` 500→50 ms；`tools/gui_tx_smoke.py` 覆盖恢复路径。
- 待实机定案：`idf.py monitor` + Flash coredump（已开启）确认设备是否重启。

## 3. 验收（P0）

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

## 4. 架构

```text
CAN 总线 ── SN65HVD230 ── ESP32-S3
                            ├─ twai_task ─→ PSRAM ring ─→ ui_task（LVGL trace/统计）
                            │                         └→ net_task ─Wi-Fi/TCP→ linsightd
                            └─ 触摸/用户键
USB-CAN ── SocketCAN(can0) ── linsightd（对照/回放）
linsightd ─→ .lsc 日志 / ASC 导出 / linsight-cli（Unix socket）
```

- 固件任务：twai_task（核 0，10，收帧/时间戳/告警）；net_task（核 0，5，批量/控制/重连）；
  LVGL 任务（核 1，20 Hz 拉取快照）；STATS 1 Hz。跨任务 UI 更新只写无锁邮箱。
- 内存：ring ≈1 MB PSRAM（52428 条）；批量 4×16 KB；draw buffer 内部 DMA。

## 5. 协议 v2.0（详见 `protocol/frame.h`）

5 字节帧头 `AA 55 type len_lo len_hi`；记录 20 B：
`seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`，flags：EXT/RTR/ERR。

| type | 方向 | 负载 |
|---|---|---|
| 01 HELLO | 设备→主机 | proto、fw、device id、bitrate |
| 02 CAN_BATCH | 设备→主机 | `u16 count` + N 条记录 |
| 03 STATS | 设备→主机 | fps、负载、总数、丢帧、TEC/REC、状态 |
| 04 EVENT | 设备→主机 | bus-off / error-passive / overflow |
| 05 PING / 06 PONG | 双向 | 4 B 回显，测 RTT/保活 |
| 07 CONTROL | 主机→设备 | 过滤、暂停/恢复、清统计、发送（P2） |
| 08 ACK | 设备→主机 | id、code |
| 09 FLOW | 主机→设备 | 最大批速率/暂停 |

断网继续采集，重连只发新帧；丢帧在 STATS/UI 显示。

## 6. 主机软件与 UI 路线

- `linsightd`：epoll + 非阻塞 TCP；协议解析；writer 线程 + 有界 ring；Unix socket /
  `--ctl-port` 控制口；SIGTERM 优雅退出。
- `linsight-cli`：`stats`、`live`、`pause`/`resume`/`clear`、`export asc|candump`。
- 主机 UI 路线（M3 后定稿；本次先修问题，暂缓决策）：
  - **Web（推荐）**：FastAPI + WebSocket + Canvas；跨平台、图表与远程查看容易。
  - PySide6：原生桌面、离线好；依赖与打包更重。
  - Tkinter：零依赖但图表/交互上限低，仅保留为台架测试台。

## 7. 分步计划

### M3 台架健壮性（1 周，优先）

1. 对照验证：PCAN 发 500 kbps/80% ×10 min，设备 `.lsc` 与发送端逐帧/序号对比。
2. bus-off 注入：运行中摘 CAN_H、停设备 ACK，确认 PCAN 自动复位、GUI 不断流、EVENT 可见。
3. 断线重连：拔网线/关 daemon 5 s 内恢复；设备继续采集，重连只补新帧。
4. 1 Mbps 短时（≤60 s）：记录 fps/负载/丢帧，写入本文档。
5. 稳定性：10 min 后检查空闲堆/PSRAM；若设备重启，抓 coredump 并修复。

验收：P0 采集/故障两行全绿；全程无需重插 USB-CAN。

### M4 工具链（1 周）

1. 回放：`linsight-cli replay FILE.lsc` → SocketCAN/vcan，时间误差 <5%。
2. DBC：Python + cantools 解码，导出 CSV/绘图。
3. 双路对照：`--reference can0` 报告缺失/重复/延迟分位。
4. 部署：systemd unit + 日志轮转；README 命令。
5. 测试：协议 parser fuzz（host harness）+ CI（build + ctest）。

验收：ASC/candump 可被 can-utils 读取；fuzz/CI 全绿。

### M5 主机 UI 重做（1–2 周，路线见 §6）

1. 定稿路线与信息架构（实时 trace、统计、过滤、回放、导出）。
2. 后端：复用 daemon 控制口，加 WebSocket 推送与历史查询。
3. 前端：虚拟化 trace 列表 + 负载/错误图 + 控制面板；断线自动重连。
4. 联调：CONTROL/FLOW、REMOTE SYNC、暂停/恢复/清统计。

验收：浏览器 1 h 不卡；3000 帧/s 渲染不丢（抽样校验）；Windows/WSL 均可访问。

### M6 soak 与交付（3–5 天）

1. 24 h soak（设备 + daemon + UI），监控 RSS/句柄/磁盘。
2. 演示：接线图、脚本、3 min 视频；简历数字按实测回填。
3. 发布 tag + changelog，docs 与代码对齐。

验收：P0 全部达标。

## 8. 风险与约束

- 台架物理层：3.3 V 收发器、120 Ω 终端、共地；默认 LISTEN_ONLY，NORMAL 仅两节点。
- 高负载：ring + 批量 + 对照；1 Mbps 只做短时验证。
- Wi-Fi 抖动：本地缓冲 + 重连 + 状态可见；写盘阻塞：writer 线程 + 有界 ring + 丢弃计数。
- 范围膨胀：M3 验收前不做 UI 重写、不加新功能。

## 9. 简历写法

> **LinSight CAN — 无线 CAN 总线分析仪（ESP32-S3 + Linux）**
> - TWAI 500 kbps/1 Mbps 采集，PSRAM 环形缓冲与时间戳；LVGL 触摸屏实时显示 trace、总线负载、错误计数，20 fps。
> - Linux C 守护进程：epoll + 非阻塞 TCP + 环形缓冲 + 二进制日志/ASC 导出 + Unix socket CLI；SocketCAN 回放与 USB-CAN 双路对照。
> - 500 kbps/80% 负载 10 min 零丢帧；bus-off 自动恢复；24 h soak 无泄漏；协议解析器 fuzz + CI。
> - 可选：DBC 解码、触发/过滤、签名 OTA。

数字（丢帧、fps、延迟、soak）实现后按实测填写，不预先写死。
