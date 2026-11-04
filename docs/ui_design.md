# LinSight UI 设计（CAN 分析仪）

> 320×240 触摸屏，iOS 深色设计语言。本文是 `main/ui.c` 的目标稿。
> 协议以 [project_plan.md](project_plan.md) §4 为准；引脚/初始化见 [hardware.md](hardware.md)。

## 1. 原则

- 纯黑底、iOS 系统色、大圆角卡片、清晰字阶；不用阴影、渐变、大面积半透明。
- 采集数据由 20 Hz LVGL timer 拉取（快照 + 8 条 ring tail），UI 不阻塞采集。
- Trace 行做脏检查：内容不变不重绘（暂停时 SPI 零流量）。
- 全部英文（Montserrat 无中文字形）；触控目标 ≥40×40（`lv_obj_set_ext_click_area()` 扩热区）。

## 2. Tokens

| 用途 | 值 |
|---|---|
| 背景 / 卡片 / 控件底轨 / 按压选中 | `000000` / `1C1C1E` / `2C2C2E` / `3A3A3C` |
| 主文字 / 次文字 | `FFFFFF` / `8E8E93` |
| 强调（蓝点 / 扩展 ID） | `0A84FF` / `BF5AF2` |
| OK / 警告 / 错误 | `30D158` / `FF9F0A` / `FF453A` |
| 字体 | Montserrat 12（正文/标签）/ 14（数值）/ 24（品牌） |
| 间距 | 外边距 8，卡片 radius 12，无边框 |

sdkconfig 需开：`LV_COLOR_16_SWAP=y`、`LV_MEM_CUSTOM=y`、字体 12/14/16/20/24。

## 3. 导航

- 顶栏高 28 + 1px 发丝线（`1C1C1E`）：左侧 iOS 分段控件 `Monitor | Settings`（124×22，选中段 `3A3A3C` 白字）；中部橙色 `PAUSED` 胶囊（全局，暂停时出现）；右侧 8×8 状态灯 + 连接文字。
- 点击分段控件任意位置按位置选页；左右滑动手势切页（`LV_OBJ_FLAG_GESTURE_BUBBLE`）；切页 150 ms 淡入。
- GPIO0 用户键：短按暂停/恢复采集（息屏时短按先亮屏）；长按 ≥800ms 开关背光，亮屏时长按切设置页。
- 状态灯：灰=找 Wi-Fi，橙=连主机，绿=在线（旁显 `12 ms`），红=重连中。
- 开屏覆盖层：黑底 + `LinSight`（24 号）+ 蓝色 40×3 强调条 + `CAN ANALYZER`，约 1.2s 后 250ms 淡出。

## 4. Monitor（内容区 y28 起）

| 区域 | 位置 | 内容 |
|---|---|---|
| 状态行 | y33, 14 号 | `500k` 白；`LISTEN` 橙 / `NORMAL` 绿；`RUNNING` 绿 / `BUS_OFF`·`RECOVERING` 红 / `STOPPED` 灰 |
| 表头 | y56 | `SEQ ID F DLC DATA` 灰 12 号，列 x=8/46/112/126/148 |
| Trace 8 行 | y74 起，行高 16 | 最新帧在下；`ID` 3 位十六进制（扩展 8 位、紫色），`F`=S/X+R，DATA=HEX 字节 |
| 统计卡 | (8,202) 304×34 | 4 列（各 76 宽）：大数值 14 号 + 灰标签 12 号 — `FPS` / `LOAD`（>85% 橙）/ `ERR`（>0 红，=bus_errors+missed）/ `DROP`（>0 橙） |

- 环形缓冲空时行留空；暂停/断网时 trace 自然停住，不影响采集恢复。

## 5. Settings（iOS 分组卡片）

| 卡片 | 内容 |
|---|---|
| BITRATE (8,32) 304×84 | 小灰标题 + 4 胶囊 66×32 `125k / 250k / 500k / 1M`，选中 `3A3A3C`；点击立即重启 TWAI |
| MODE (8,124) 304×62 | `Listen-only` / `Normal` 两个胶囊 138×28；Normal 仅限台架（会 ACK 总线） |
| 动作 (8,194) 304×40 | `Pause/Resume`（当前状态取自快照）、`Clear stats` |

- 比特率/模式修改走 `twai_capture_restart()`，LVGL 任务内短阻塞（<100ms）。
- 不提供 ID 过滤 UI（协议 CONTROL 支持，P1 再加界面）。

## 6. 连接状态

- 顶栏文字：`WiFi...` / `Host...` / `12 ms` / `Host...`（红）。
- 断网不影响本地采集与显示；重连后从 ring 续发（旧数据可能被覆盖，drop 计数可见）。

## 7. 对外接口（`main/ui.h`）

```c
void linsight_ui_init(void);                        // app_main 持有 LVGL 锁时调用
void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms);
void linsight_ui_toggle_settings(void);             // 用户键长按
```

- 除 `linsight_ui_init` 外，每个接口内部自己 `lvgl_port_lock()/unlock()`。
- trace/统计不设推送接口：LVGL timer 直接调 `twai_stats_snapshot()` / `can_ring_tail()`。

## 8. LVGL 8.3 约束

- 禁止 `lv_label_set_text_fmt("%f")`；先 `snprintf` 到栈缓冲，再 `lv_label_set_text`。
- 禁用 LVGL v9 API（如 `lv_display_*`）；只由 LVGL 任务改对象。
- 静态 `lv_color_t` 数组不能用 `lv_color_hex()`（运行时函数），用 switch/宏赋值。
- 8KB LVGL 任务栈：长生命周期控件用 `static`；trace 行标签复用，不动态增删。
- draw buffer 在内部 DMA RAM（320×40×2）；无 shadow；页面淡入 150ms 是唯一常驻动画。
