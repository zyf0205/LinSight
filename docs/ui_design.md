# LinSight UI 设计（CAN 分析仪）

> 320×240 触摸屏，Apple watchOS / iOS 深色风格。本文是 `main/ui.c` 的目标稿。
> 协议以 [project_plan.md](project_plan.md) §4 为准；引脚/初始化见 [hardware.md](hardware.md)。

## 1. 原则

- 纯黑底、大圆角卡片、高对比文字；不用阴影、渐变、大面积半透明。
- 采集数据由 20 Hz LVGL timer 拉取（快照 + 8 条 ring tail），UI 不阻塞采集。
- 全部英文（Montserrat 无中文字形）；帧数据用列标签对齐，不用等宽字体。
- 触控目标 ≥40×40（视觉可小，用 `lv_obj_set_ext_click_area()` 扩大热区）。

## 2. Tokens

| 用途 | 值 |
|---|---|
| 背景 / 卡片 / 控件底轨 | `0x000000` / `0x1C1C1E` / `0x2C2C2E` |
| 按压态 / 选中段 | `0x3A3A3C` |
| 主文字 / 次文字 | `0xFFFFFF` / `0x8E8E93` |
| 强调蓝（扩展 ID） | `0xBF5AF2` 紫 |
| OK / 警告 / 错误 | `0x30D158` / `0xFF9F0A` / `0xFF453A` |
| 字体 | Montserrat 12 / 14 / 16 / 20 / 24 |
| 间距 | 外边距 6，卡片 radius 14，无边框 |

sdkconfig 需开：`LV_COLOR_16_SWAP=y`、`LV_MEM_CUSTOM=y`、以上 5 个字体。

## 3. 导航

- 顶栏高 28：左侧胶囊 `Monitor | Settings`，选中 `#3A3A3C`；右侧 8×8 状态灯 + 连接文字。
- 点击胶囊或左右滑动手势切页（`LV_OBJ_FLAG_GESTURE_BUBBLE`）。
- GPIO0 用户键：短按暂停/恢复采集（息屏时短按先亮屏）；长按 ≥800ms 开关背光，亮屏时长按切设置页。
- 状态灯：灰=找 Wi-Fi，橙=连主机，绿=在线（旁显 `12 ms`），红=重连中。
- 开屏覆盖层：黑底 + `LinSight` 字标 + `CAN ANALYZER`，约 1.2s 后 250ms 淡出。

## 4. Monitor（内容 320×212）

| 区域 | 位置 | 内容 |
|---|---|---|
| 状态行 | y32, 14 号 | `500k` 白；`LISTEN` 橙 / `NORMAL` 绿；`RUNNING` 绿 / `BUS_OFF`·`RECOVERING` 红 / `STOPPED` 灰；`PAUSED` 橙（暂停时出现） |
| 表头 | y50 | `SEQ  ID  F  DLC  DATA` 灰色 12 号 |
| Trace 8 行 | y66 起，行高 18 | 最新帧在下；`SEQ`=序号，`ID`=4 位（扩展 8 位，紫色），`F`=S/X+R，`DLC`，`DATA`=HEX 字节 |
| 底部统计 | y220 | `%u fps   load %u%%   err %u   drop %u`（err=bus_errors+missed；drop=ring 覆盖丢弃） |

- 环形缓冲空时行留空；暂停/断网时 trace 自然停住，不影响采集恢复。
- 列宽固定 x：8/46/112/126/148，标签分列避免比例字体抖动。

## 5. Settings（内容 320×212）

| 卡片 | 内容 |
|---|---|
| Bitrate (h88) | 4 个胶囊 `125k / 250k / 500k / 1M`，选中 `#3A3A3C`；点击立即重启 TWAI |
| Mode (h62) | `Listen-only` / `Normal` 两个胶囊；Normal 仅限台架（会 ACK 总线） |
| 动作 (h40) | `Pause/Resume`（当前状态取自快照）、`Clear stats` |

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
- 静态 `lv_color_t` 数组不能用 `lv_color_hex()`（运行时函数），用 switch 赋值。
- 8KB LVGL 任务栈：长生命周期控件用 `static`；trace 行标签复用，不动态增删。
- draw buffer 仍在内部 DMA RAM（320×40×2）；性能优先：无 shadow、无长动画。
