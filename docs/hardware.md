# LinSight 硬件

立创·实战派 ESP32-S3（无电池、无摄像头）。引脚、外设和初始化顺序以本文件为准。

## 1. 板卡

| 项 | 参数 |
|---|---|
| 模组 | ESP32-S3-WROOM-1-N16R8（16 MB Flash + 8 MB Octal PSRAM） |
| 屏幕 | 2.0" IPS 320×240，ST7789，SPI3 |
| 触摸 / IMU | FT6336 / QMI8658，I2C0 |
| USB | Type-C → CH334F HUB → CH340K 串口 + 原生 USB |
| 按键 | RESET + GPIO0 用户键（低有效） |

## 2. 引脚

| GPIO | 用途 | 备注 |
|---|---|---|
| 0 | BOOT / 用户键 | strapping，低有效 |
| 1 / 2 | I2C0 SDA / SCL | 触摸、PCA9557、IMU、音频共用；100 kHz |
| 3–9, 15–18, 46 | 摄像头 | 本版本无，保留；46 必须低，3 也是 strapping |
| 10 / 11 | J2 扩展口 | CAN TXD / RXD；唯一自由 GPIO |
| 13 / 14 / 38 / 45 | I2S WS / BCK / MCLK / DO | 音频；45 是 strapping |
| 19 / 20 | USB D- / D+ | 保留 |
| 21 / 47 / 48 | TF D0 / CLK / CMD | 1-bit SDMMC |
| 26–37 | Flash / Octal PSRAM | **保留**（35/36/37 明确被 PSRAM 用） |
| 39 / 40 / 41 / 42 | LCD DC / MOSI / CLK / 背光 | 背光低电平点亮 |
| 43 / 44 | UART0 TX / RX | CH340K |

- 除 J2 外没有引出其他 GPIO（GPIO12、22–25 需焊测试点）；所有 IO 为 3.3 V，不耐 5 V。
- SD：板载 1-bit SDMMC，无卡检测脚；20 MHz 挂载，失败后降 10 MHz 重试；每 5 秒重试。
- 用户键：GPIO0 每 20 ms 轮询消抖，长按阈值 800 ms；背光 GPIO42 用 LEDC 5 kHz、10-bit 占空比。

## 3. 显示与触摸

| 项 | 参数 |
|---|---|
| LCD CS | **PCA9557 BIT0 @0x19**，不是 GPIO |
| LCD RST | 未连接 |
| SPI | Mode 2，80 MHz，`trans_queue_depth=10`，16 bpp |
| 面板 | `invert_color=true`，`swap_xy=true`，`mirror_x=true` |
| 触摸 | 地址 0x38，INT/RST 未接，轮询；`x_max=240, y_max=320, swap_xy=1, mirror_x=1` |

初始化顺序（漏了会黑屏）：

```text
i2c_bus_init() → pca9557_init() → panel_reset() → 写 PCA9557 输出 0x04（LCD_CS 低） → panel_init()
```

- Draw buffer：320×40、双缓冲、内部 DMA RAM（`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`）。
- LVGL 任务固定在 core 1，优先级 4，栈 8 KB；`CONFIG_LV_COLOR_16_SWAP=y`。

## 4. PCA9557 与 I2C

地址 **0x19**；寄存器：`0x00` 输入、`0x01` 输出、`0x02` 极性、`0x03` 配置（1=输入，0=输出）。

| Bit | 信号 | 初始值 |
|---|---|---|
| 0 | LCD_CS | 1 |
| 1 | PA_EN | 0 |
| 2 | DVP_PWDN | 1 |

- 初始化：输出寄存器写 `0x05`，配置寄存器写 `0xF8`（低 3 位输出）；LCD 初始化时再写 `0x04` 拉低 CS。
- 总线设备：PCA9557 `0x19`、FT6336 `0x38`、QMI8658 `0x6A`、ES7210 `0x41`、ES8311 `0x18`。

## 5. 扩展口

```text
多功能 J2：5V  3V3  GND  IO10  IO11
           5    4    3    2     1
I2C   J1：5V  3V3  GND  SDA   SCL
           5    4    3    2     1
```

- J2：1=**GPIO11**，2=**GPIO10**；支持 GPIO/UART/I2C/CAN/PWM/ADC。GPIO10=ADC1_CH9，GPIO11=ADC2_CH0（Wi-Fi 下受限）。
- J1：1=**GPIO2/SCL**，2=**GPIO1/SDA**；与板载设备共用总线，地址不能冲突；线序以连接器小三角和丝印为准。
- J2/J1 均为 HC-GH-5PWT（5P）；5 V 只在 USB 供电时存在。

### CAN 接线

```text
USB-CAN(PCAN-USB) CAN_L ── 收发器 CANL
USB-CAN            CAN_H ── 收发器 CANH
收发器 VCC ── 3V3 (J2 引脚4)   收发器 GND ── GND (J2 引脚3)
收发器 TXD ── GPIO10 (J2 引脚2)  收发器 RXD ── GPIO11 (J2 引脚1)
```

- 3.3 V 收发器（SN65HVD230 类）；两端 120 Ω 终端、共地；仅 CAN 2.0A/B，默认 500k。
- 默认 `LISTEN_ONLY`，不 ACK；单对节点台架需切 `NORMAL`，否则发送方收不到 ACK 会重发。

| 比特率 | 8B 标准帧理论上限 | 80% 负载 | 台架建议 |
|---|---:|---:|---:|
| 500k | ~3800 fps | ~3000 fps | ≤2000 fps |
| 1M | ~7600 fps | ~6000 fps | ≤4000 fps |

### 台架

- 高速连续发送会让部分 PCAN-USB 假活（`CAN_Write` OK、status 0x0，回显/上行停摆）；软复位无效，只能拔插或管理员 `pnputil /restart-device`。
- 推荐 WSL2 SocketCAN：Windows 侧 `python tools/can_gui.py` 自动 `usbipd attach`、配置 `can0`；首次或换 USB 口需管理员 bind（程序自动弹 UAC）；主机仓库提供 `tools/can-up.sh [BUSID] [BITRATE]` 和 `tools/can-down.sh --status`。
- 手动等价：`usbipd bind/attach --wsl --busid <id>`，WSL 内 `sudo modprobe peak_usb`、`sudo ip link set can0 type can bitrate 500000 restart-ms 100 && sudo ip link set can0 up`。
- PCAN-USB 经 usbip 约 1 ms/帧，持续上限约 830–920 fps；发帧用 `cangen can0 -n 500 -g 20 -I 0x123 -L 8`，以设备 `total` 为判据。

## 6. sdkconfig 关键项

| 项 | 值 |
|---|---|
| CPU / 优化 / RTOS | 240 MHz，`CONFIG_COMPILER_OPTIMIZATION_PERF=y`，`CONFIG_FREERTOS_HZ=1000` |
| Flash / PSRAM | 16 MB QIO；Octal PSRAM 80 MHz；`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`（≤1 KB 留内部 RAM） |
| TCP | WND 32768、SND 32768、RECVMBOX 32 |
| Wi-Fi 缓冲 | static RX 4、dynamic RX/TX 12、mgmt 16 |
| LVGL | `CONFIG_LV_COLOR_16_SWAP=y`、`CONFIG_LV_MEM_CUSTOM=y` |
| FATFS | `CONFIG_FATFS_LFN_NONE=y`、`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` |
| 分区表 | 4 MB factory 应用 + 64 KB coredump |

## 7. 上电自检

- [ ] `esp_psram_get_size()` = 8 MB；I2C 扫描到 0x19 / 0x38 / 0x6A
- [ ] 纯色全屏正常（红/绿）
- [ ] 背光 0% / 100% 可控，触摸四角坐标准确
