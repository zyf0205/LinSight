这个方向转得非常漂亮！相比于已经被做烂的“桌面副屏”，**无线手持 CAN 总线分析仪 + Linux `epoll` 守护进程 + SocketCAN 双路对照** 在汽车电子、工业控制、机器人和嵌入式 Linux 岗位面试中的含金量高出几个量级。

针对新项目的定位（**工业/车载现场手持仪器、20 fps 实时 8 行报文 Trace、M3 过滤/触发/Bus-Off 诊断、固件总代码量 ≤1000 行**），我为你重新设计了一份像素级对齐、兼顾 **Apple 暗黑极简质感** 与 **Vector CANoe / 示波器专业可读性** 的 `ui_design.md`。

你可以直接将以下内容保存覆盖为仓库中的 `ui_design.md`（也可直接发给 Claude Code 更新 `main/ui.c`）：

---

```markdown
# LinSight CAN — UI 设计规范 (`ui_design.md` v2.0)

**ESP32-S3 手持无线 CAN 总线分析仪本地交互界面**  
硬件基线：320×240 ST7789 (SPI3 80MHz DMA) + FT6336 电容触摸 + GPIO0 用户键。  
软件基线：ESP-IDF 5.5.x + LVGL `~8.3.0` + `esp_lvgl_port ~1.4.0`。

---

## 1. 设计目标与工程红线

1. **仪器级可读性（防抖动对齐）**：
   - 在 20 fps（50 ms 周期）高频刷新下，报文列表严禁出现水平字符跳动（Jitter）。每行采用 **4 列固定 X 坐标独立 Label**（`ID` / `DLC` / `DATA` / `CNT`），避免非等宽字体导致列错位。
2. **采集零阻塞（快照解耦）**：
   - `ui_task`（Core 1, Prio 4）绝不直接遍历完整 1 MB PSRAM 环形缓冲，仅以 20 Hz 读取轻量级 UI 快照槽（Snapshot），单帧渲染耗时控制在 `< 8 ms`，触摸响应 `< 100 ms`。
   - 断网/无主机时，本地 UI 采集、触发、暂停与统计完全独立工作。
3. **零动态内存与极简代码量**：
   - 所有 UI 控件在初始化时一次性静态创建，20 Hz 刷新路径严禁 `malloc` / 创建销毁对象，统一使用静态字符缓冲 + `lv_label_set_text_static()`。
   - 严控复杂度：仅保留 **Monitor（监控页）** 与 **Settings（设置与触发页）** 两屏，`main/ui.c` 目标代码量 ≤ 350 行。
4. **渲染性能红线**：
   - 禁用 `shadow`（阴影）、渐变与半透明图层叠加；Draw Buffer 固定驻留内部 DMA SRAM (`320×40×2 × 2`)。

---

## 2. 视觉与色彩规范（Dark Instrument HIG）

结合 Apple Dark Mode 无边框克制美学与车载总线分析仪的色彩语义：

### 2.1 基础表面色
| 角色 | Hex 色值 | 用途说明 |
| :--- | :---: | :--- |
| **底层画布 (Canvas BG)** | `0x000000` | 纯黑背景，消除 2.0 寸屏幕物理黑边感，提升暗光/强光对比度 |
| **顶栏/底栏表面 (Bar Surface)** | `0x161618` | 顶部状态栏与底部统计栏底色 |
| **斑马纹偶数行 (Row Even)** | `0x0B0B0E` | Trace 列表 0, 2, 4, 6 行底色 |
| **斑马纹奇数行 (Row Odd)** | `0x141419` | Trace 列表 1, 3, 5, 7 行底色（免网格线即可轻松横向扫读 8 字节） |
| **卡片/按钮表面 (Control Tile)** | `0x1C1C1E` | Settings 页分段按钮、过滤卡片背景（圆角 `8px`，无边框） |
| **按压/选中高亮 (Active Tile)** | `0x2C2C2E` | 触摸按压态或选中行高亮底色 |

### 2.2 文本与总线状态语义色
| 语义状态 | Hex 色值 | 适用场景 |
| :--- | :---: | :--- |
| **主数据文本 (Primary)** | `0xF8FAFC` | DATA 十六进制字节、主统计数值 (`FPS` / `CNT`) |
| **次要标签 (Secondary)** | `0x8E8E93` | 表头文字、单位标签、`DLC` 数字 |
| **标准帧 ID (11-bit STD)** | `0x0A84FF` | Apple System Blue，标识标准帧 ID（如 `123`） |
| **扩展帧 ID (29-bit EXT)** | `0xBF5AF2` | Apple System Purple，标识扩展帧 ID（如 `18DAF110`） |
| **正常在线 / ACTIVE** | `0x30D158` | 总线 `ERR-ACTIVE`、Wi-Fi/Host 已连接、负载正常 (<70%) |
| **告警 / PASSIVE / PAUSE** | `0xFF9F0A` | 总线 `ERR-PASSIVE` (TEC/REC>127)、高负载 (>70%)、画面 `PAUSED` / `TRIG` |
| **故障 / BUS-OFF / OVERFLOW** | `0xFF453A` | 总线 `BUS-OFF`、错误帧 (`ERR`)、Ring 溢出 (`OVF > 0`) |

---

## 3. 320×240 像素级布局规范（双屏架构）

垂直方向 240px 严格按像素分配，零滚动条、零裁剪：

```text
+-----------------------------------------------------------------------+ Y=0
| [● ACTIVE]  [500K]  [RUN]                 [HOST ● 4ms]      [⚙ Set]  | Top Bar (26px)
+-----------------------------------------------------------------------+ Y=26
| ID          L   DATA (HEX)                                       CNT  | Header (16px)
+-----------------------------------------------------------------------+ Y=42
| 00000123    8   02 01 0C 55 AA 00 00 00                        14.2k  | Row 0 (21px)
| 000007E8    8   04 41 0C 1A F8 00 00 00                         1.4k  | Row 1 (21px)
| 18DAF110 E  8   10 14 49 02 01 31 47 31                          312  | Row 2 (21px)
| 00000240    4   A0 0F 00 12                                     8.9k  | Row 3 (21px)
| ... (共 8 行，每行高 21px，斑马纹交替，点击行可锁定过滤该 ID) ...      | Row 4..7
+-----------------------------------------------------------------------+ Y=210
| FPS: 4330     LOAD: 82%      TEC/REC: 0/0       OVF: 0                | Bottom Bar (30px)
+-----------------------------------------------------------------------+ Y=240
```

### 3.1 页面一：Monitor（实时报文监控页）

#### A. 顶部状态栏（Y: `0..25`, 高 `26px`, 宽 `320px`, 背景 `0x161618`）
从左至右紧凑排列：
1. **总线状态胶囊 (`X: 4`)**：圆角胶囊（高 18px），直观显示 TWAI 控制器状态：
   - `● ACTIVE`（绿底黑字或深绿底绿字 `0x30D158`）
   - `● PASSIVE`（琥珀橙 `0xFF9F0A`）
   - `● BUS-OFF`（亮红 `0xFF453A`，触发自动恢复计数）
2. **波特率与模式标签 (`X: 82`)**：显示 `500K`（只听模式显示 `500K·LO`）。
3. **运行/触发状态 (`X: 136`)**：
   - `RUN`（采集中，绿色小字）
   - `PAUSED`（暂停刷新，橙色高亮，后台 Ring 仍继续收帧）
   - `TRIG'D`（M3 触发命中自动锁屏，蓝色高亮 `0x0A84FF`）
4. **主机链路指示 (`X: 196`)**：
   - 已连 `linsightd`：`HOST ● 3ms`（绿色圆点 + 实时 PING RTT）
   - 仅本地独立工作：`LOCAL`（灰色 `0x8E8E93`，不弹窗打断现场调试）
5. **设置入口按钮 (`X: 272..316`)**：`44×24` 触控热区，显示 `LV_SYMBOL_SETTINGS`，点击切入 Settings 页。

#### B. 表头栏（Y: `26..41`, 高 `16px`, 背景 `0x000000`）
使用 `lv_font_montserrat_12`（颜色 `0x8E8E93`），X 坐标与下方数据列严格对齐：
- `X: 4` -> `"ID"`
- `X: 70` -> `"L"` (DLC)
- `X: 88` -> `"DATA (HEX)"`
- `X: 274` (右对齐) -> `"CNT"`

#### C. 8 行报文 Trace 矩阵（Y: `42..209`, 共 `168px`, 每行高 `21px`）
每行是一个高 `21px`、宽 `320px` 的无边框容器（偶数行 `0x0B0B0E`，奇数行 `0x141419`），内部包含 **4 个固定 X 坐标的 Label**（字体 `montserrat_12`）：
1. **`lbl_id[i]` (`X: 4, Y: 3`)**：
   - 标准帧：3 位十六进制（如 `  7DF`，蓝色 `0x0A84FF`）
   - 扩展帧：8 位十六进制（如 `18DAF110`，紫色 `0xBF5AF2`）
   - 错误帧 (`flags & BIT2`)：显示 `ERR-FRM`（红色 `0xFF453A`）
2. **`lbl_dlc[i]` (`X: 70, Y: 3`)**：
   - 数据帧显示 `0..8`（灰色 `0x8E8E93`）；远程帧 (`RTR`) 显示 `R`（橙色）。
3. **`lbl_data[i]` (`X: 88, Y: 3`)**：
   - 固定格式 `"%02X %02X ..."`（最多 23 字符，纯白 `0xF8FAFC`；RTR 帧显示 `"REMOTE REQUEST"`）。
4. **`lbl_cnt[i]` (`X: 268, Y: 3`, 宽 48px 右对齐)**：
   - `< 10000` 显示精确数字（如 `4320`）；`>= 10000` 自动缩写为 `14.2k`、`1.05M`。
- **行点击微交互**：手指轻触任意一行，该行背景高亮为 `0x2C2C2E`，自动将该行 `ID` 设为快速高亮/单 ID 过滤观察对象（再次点击取消）。

#### D. 底部遥测统计栏（Y: `210..239`, 高 `30px`, 背景 `0x161618`）
水平四等分展示核心吞吐与健康度指标（1 Hz 或 20 Hz 更新）：
1. **`FPS: xxxx`**：当前每秒帧率（如 `4330`）。
2. **`LOAD: xx%`**：估算总线负载率（`>= 75%` 时数字变琥珀色 `0xFF9F0A`）。
3. **`ERR: Txx/Rxx`**：TWAI `TEC/REC` 错误计数器或累计错误帧数（非 0 时变黄/红）。
4. **`OVF: x`**：PSRAM Ring 溢出丢帧计数（`0` 为灰色/绿色；`> 0` 时立即变红 `0xFF453A` 告警）。

---

### 3.2 页面二：Settings（设置、过滤与触发页）

用于现场免电脑快速改波特率、设过滤/触发及清零统计。布局采用 **顶部返回栏 + 3 组紧凑控制卡片**：

1. **顶部导航栏（高 `28px`）**：
   - 左侧返回按钮：`LV_SYMBOL_LEFT " Monitor"`（点击回主监控页）。
   - 右侧显示固件版本、设备 ID 及主机远控锁状态（当主机 `CONTROL` 下发指令时闪绿提示 `REMOTE SYNC`）。
2. **卡片一：Bitrate & Mode（Y: `34..86`, 高 `52px`）**：
   - 左侧 4 段胶囊选择器：`125K` | `250K` | `500K` | `1M`（当前生效档位高亮蓝色 `0x0A84FF`）。
   - 右侧模式指示/切换胶囊：`Listen`（默认安全只听） / `Normal`（ACK 应答模式）。
3. **卡片二：Filter & Trigger（M3 核心功能，Y: `92..168`, 高 `76px`）**：
   - **Filter 行**：提供快捷过滤开关：`[All IDs]` | `[Std Only]` | `[Ext Only]` | `[Locked ID: 0x---]`（联动 Monitor 页点击锁定的 ID）。
   - **Trigger 行（单次触发冻结）**：`[Trig: OFF]` | `[On Error/BusOff]` | `[On Locked ID]`。开启后，一旦捕获到目标事件，UI 自动切为 `TRIG'D` 冻结当前 8 行现场画面，方便工程师拍照或排查。
4. **卡片三：Quick Actions（底部操作栏，Y: `174..234`, 高 `60px`）**：
   - 三个大尺寸触控按钮（宽 98px，高 50px，圆角 10px，非常适合大拇指盲操）：
     - **[ `LV_SYMBOL_PAUSE` Pause / Resume ]**：冻结/恢复本地画面刷新。
     - **[ `LV_SYMBOL_TRASH` Clear Stats ]**：清零累计帧数、OVF 计数与当前屏幕 Trace。
     - **[ `LV_SYMBOL_REFRESH` Bus Recover ]**：手动触发 TWAI Bus-Off 恢复 / 重置控制器。

---

## 4. 硬件按键、触摸与主机 `CONTROL` 联动契约

| 触发源 | 动作 | 系统行为 |
| :--- | :--- | :--- |
| **GPIO0 短按** | `< 800 ms` | 若在息屏态则先亮屏；亮屏态直接切换 **Monitor 暂停 (`PAUSED`) / 恢复 (`RUN`)** |
| **GPIO0 长按** | `>= 800 ms` | 在 **Monitor 页 ↔ Settings 页** 之间快速切换（无需双手触摸） |
| **触摸左/右滑** | `LV_EVENT_GESTURE` | 全屏支持左右滑动切换 Monitor 与 Settings 页 |
| **点击 Trace 行** | `LV_EVENT_CLICKED` | 锁定/解锁该行 CAN ID，并在 Settings 页同步显示为过滤/触发目标 |
| **主机 `CONTROL` (0x07)** | 网络下发 | 主机 `linsight-cli pause/resume/clear/filter` 下发后，调用 UI 接口同步更新界面状态，并由设备回发 `0x08 ACK` |

---

## 5. 20 fps 零阻塞渲染实现要点（LVGL 8.3）

为确保在 `500 kbps / 80%+ 负载（~4000 fps）` 下 UI 依然稳定 20 fps 且绝不卡住 `twai_task`：

1. **读写分离的显示快照槽 (`ui_trace_slot_t`)**：
   - `twai_task` 只管往 PSRAM Ring 写原始 20B 记录，同时顺手更新一个极小的 8 槽位摘要表（或由 `ui_task` 每 50ms 从 Ring 尾部倒序提取最新 8 个不同 ID / 最新 8 条记录）。
   - `ui_task` 在 `lvgl_port_lock(0)` 保护下，将 8 条记录格式化进静态 `static char s_row_buf[8][4][28]`，仅当内容变化时调用 `lv_label_set_text_static()`。
2. **禁止在 `lv_label_set_text_fmt` 中使用 `%f`**：
   - 所有百分比或 `k/M` 缩写统一使用整数运算 + `snprintf()` 写入静态数组（例如 `cnt / 1000` 与 `(cnt % 1000) / 100` 拼出 `"14.2k"`）。

---

## 6. 对外模块接口规范 (`main/ui.h`)

保持 `main/ui.c` 与底层 `twai.c`、`net.c` 完全解耦：

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_TRACE_ROWS 8

typedef enum {
    CAN_BUS_ACTIVE = 0,
    CAN_BUS_WARN,
    CAN_BUS_PASSIVE,
    CAN_BUS_OFF
} ui_bus_state_t;

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t ts_ms;
    uint8_t  dlc;
    uint8_t  flags;     // bit0: EXT, bit1: RTR, bit2: ERR
    uint8_t  data[8];
} ui_can_row_t;

typedef struct {
    uint32_t fps;
    uint8_t  bus_load_pct;   // 0..100
    uint8_t  tec;
    uint8_t  rec;
    uint32_t total_frames;
    uint32_t rx_missed_ovf;  // PSRAM ring overflow / TWAI missed count
    uint16_t rtt_ms;
    bool     host_online;
    bool     paused;
    bool     triggered;
    uint16_t bitrate_kbps;   // 125 / 250 / 500 / 1000
    ui_bus_state_t bus_state;
} ui_status_t;

typedef enum {
    UI_ACT_SET_BITRATE = 0,  // arg = 125/250/500/1000
    UI_ACT_TOGGLE_PAUSE,     // arg = 0/1
    UI_ACT_CLEAR_STATS,      // arg = 0
    UI_ACT_SET_FILTER_ID,    // arg = CAN ID (0xFFFFFFFF = All)
    UI_ACT_SET_TRIG_MODE,    // arg = 0:Off, 1:OnErr, 2:OnLockedID
    UI_ACT_BUS_RECOVER       // arg = 0
} ui_action_type_t;

typedef void (*ui_action_cb_t)(ui_action_type_t act, uint32_t arg);

// 初始化两屏 UI 与 GPIO0 按键绑定
void linsight_ui_init(ui_action_cb_t act_cb);

// 20 Hz 更新 8 行报文快照（内部使用 static buffer + set_text_static）
void linsight_ui_update_trace(const ui_can_row_t *rows, uint8_t valid_count);

// 更新顶栏/底栏状态与统计（1 Hz ~ 20 Hz）
void linsight_ui_update_status(const ui_status_t *st);

// 供 GPIO0 按键或主机 CONTROL (0x07) 帧调用的状态同步接口
void linsight_ui_handle_key(bool long_press);
void linsight_ui_sync_control(uint8_t paused, uint16_t bitrate_kbps, uint32_t filter_id);

#ifdef __cplusplus
}
#endif
```
```

---

### 💡 这套 UI 设计对 M3（过滤/触发/故障恢复）的加持：
1. **点按行直接锁定 ID（Click-to-Solo / Trigger）**：在 2.0 寸小屏上做软键盘输入十六进制 CAN ID 非常反人类。这套设计允许你在 **Monitor 页直接用手指点一下感兴趣的那一行报文**，就能锁定该 `ID`，切到 Settings 页一键开启 **仅看该 ID** 或 **以该 ID 为单次触发源**，不仅代码不到 30 行，而且现场体验极佳。
2. **天然防抖动表格**：用 `4 列固定 X 坐标 Label × 8 行 = 32 个静态 Label` 替代复杂的 `lv_table`，不仅省去了 `lv_table` 重绘整表的庞大 CPU 开销，而且彻底消灭了非等宽字体下数字跳动导致的眼花问题，在 4330 fps 满载灌包时也能稳稳跑满 20 fps！