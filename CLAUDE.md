# CLAUDE.md

This file provides guidance to Claude Code when working in the Windows firmware checkout.

## Repository state

LinSight ESP32-S3 firmware: TWAI capture, PSRAM ring, SD black-box recording, LVGL UI, Wi-Fi streaming.
The Linux host (epoll daemon, CLI, `.lsc` logging, tests, optional SocketCAN/ROS 2 bridge) lives in
the separate WSL repository `~/linsight`; do not keep, build, or run host code in this checkout.
Bench Wi-Fi credentials and the host IP are Kconfig defaults (`CONFIG_LINSIGHT_*` in
`main/Kconfig.projbuild`); production builds should override them via menuconfig.

## Source of truth

- `docs/hardware.md` — verified pinout, CAN wiring, LCD/touch init, sdkconfig gotchas
- `protocol/frame.h` — wire format, limits, parser contract (shared with the host repo; keep both copies identical)
- `main/ui.h` — UI API and the Overview/Raw/Settings contract consumed by `ui.c`
- `~/linsight/host/README.md` — host daemon, CLI, report/export and bench reference
- `tools/can_probe/` — standalone TWAI bench probe used for M0 wiring validation

## Documentation rules

- **Firmware docs live in `docs/`; host docs live in the WSL repo (`host/README.md`, `docs/hardware.md`).** The only markdown at this repo root is `CLAUDE.md`.
- **Do not create new documents without explicit permission.** Update an existing file instead.
- Keep docs short and scannable: tables and bullets over prose; target ≤120 lines per doc.
- Do not duplicate content across docs; link to the source of truth instead.

## Git commits

- Write commit messages in **Chinese**, clear and concise: a one-line subject, then only the necessary bullets.
- Commit source, config, and docs; keep `build/`, `sdkconfig`, and `managed_components/` out of git (already ignored).

## Commands

```bash
# Firmware — run at repo root (ESP-IDF 5.5.4; path in .vscode/settings.json)
idf.py set-target esp32s3          # once
idf.py build
idf.py -p COMx flash monitor
idf.py menuconfig                  # LinSight → CAN mode, Wi-Fi SSID/password, host IP/port
```

- Bench Wi-Fi/TCP endpoint is configured in `main/Kconfig.projbuild` (`CONFIG_LINSIGHT_*`, bench values as defaults); production builds override it in menuconfig. The device sets the CN 1–13 regulatory domain for 2.4 GHz bench channels.
- Bench: the device joins the PC hotspot and connects to `linsightd` in WSL; PCAN traffic is generated on WSL `can0` (usbipd + `peak_usb`), not from Windows. Default `TWAI_MODE_LISTEN_ONLY` never ACKs — switch Settings/`menuconfig` to Normal only on a two-node bench.

## Architecture

- **Firmware tasks**: `twai_task` (core 1, prio 10) receives frames, timestamps (ms), pushes 20-byte records into the PSRAM ring (4 MiB, ~209k records, drop-oldest), mirrors the newest 512 into a capture-side UI history, fan-outs to the SD queue, and maintains counters; `net_task` (core 0, prio 5) owns Wi-Fi/TCP, streams CAN_BATCH frames, sends STATS at 1 Hz / DEVICE_INFO every 5 s / EVENTS / PING, parses CONTROL/FLOW/PING from the host; `sd_task` (core 1, prio 3) drains the 16k PSRAM queue and writes `recNNNNN.lsc`; the LVGL task pulls `twai_ui_tail` + stats at 20 Hz (the history is independent of the ring that `net_task` drains). Verified: 30000-frame boot burst at 4330 fps / 96% load, zero loss (30k < ~209k capacity, so overflow policy is covered by the host repo's `test_ring`).
- **Ring** (`can_ring.c`): mutex-protected SPSC-style; overwrite-oldest on overflow with a visible drop counter; sequence numbers expose gaps.
- **SD black box** (`sd_store.c`): independent PSRAM queue (16k, 4k internal fallback), never blocks `twai_task`; 1-bit SDMMC CLK/CMD/D0 = 47/48/21; FAT32/8.3 `recNNNNN.lsc` with the shared 32-byte header, 32 MiB or 10 min rotation, keep 8 files, `.met`/`.trg` sidecars. A `README.TXT` written at mount explains that `.lsc` is binary and points at the host commands (`linsight-cli report`, `export csv`). Code compiles; bench validation (mount, power loss, rotation) still pending.
- **Shared protocol**: `protocol/frame.c` is pure C (no IDF, no malloc); the WSL host repo compiles a mirror of the same parser. 5-byte header `AA 55 type len_lo len_hi`; types 01 HELLO, 02 CAN_BATCH, 03 STATS, 04 EVENT, 05/06 PING/PONG, 07 CONTROL, 08 ACK, 09 FLOW, 10 DEVICE_INFO; record `seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`; STATS/CONTROL payloads are small JSON; HELLO carries `device_id`/`boot`/`uptime_ms`; pause accepts a 5 s–30 min `lease_ms`.
- **UI** (`ui.c`): two pages, three views. Monitor defaults to Overview (4 health counters + 6 large top-ID rows ranked by the tumbling 1 s rate with activity bars) and switches to Raw (8×21 px zebra trace with per-ID CNT); both share the status bar and the 20 Hz telemetry bar whose fourth tile shows OVF or `REC nF`/`SD READY`/`SD ERROR`/`NO SD`. Settings holds bitrate/mode, pause, clear, bus recover and a right-aligned status label. GPIO0 short = display pause, long = cycle Overview → Raw → Settings → Overview. Host CONTROL mirrors into the settings nav (REMOTE SYNC); net_task setters publish into a lock-free mailbox that the 20 Hz LVGL timer drains, while the GPIO0 handler takes the LVGL lock for its short action.

## Hard constraints

- Only the LVGL task and the GPIO0 key handler touch LVGL. `linsight_ui_handle_key` runs on the button task and takes `lvgl_port_lock` for its short page/pause action; `net_task` must only publish into the lock-free mailbox in `ui.c` — never take the LVGL mutex from `net_task` (`lvgl_port_lock(0)` waits forever, which stalls PING/PONG until the host drops the session).
- LCD CS is **PCA9557 BIT0 @0x19**, not a GPIO. Order: `bsp_i2c_init()` → `pca9557_init()` → `panel_reset()` → `lcd_cs(0)` → `panel_init()`.
- LVGL draw buffers: internal DMA RAM, 320×40 ×2. CAN ring: PSRAM.
- `CONFIG_LV_COLOR_16_SWAP=y`.
- sdkconfig: CPU 240 MHz, `CONFIG_COMPILER_OPTIMIZATION_PERF`, `FREERTOS_HZ=1000`, TCP window 32 KB.
- Single TCP client; `net_task` is the only firmware writer.
- Protocol parser must stay allocation-free and IDF-independent so it can be fuzzed on the host.
- TWAI macros (`TWAI_TIMING_CONFIG_*`, `TWAI_FILTER_CONFIG_ACCEPT_ALL`) expand to struct initializers — they can only initialize objects, never be passed as arguments. Self-test mode in IDF 5.5 is `TWAI_MODE_NO_ACK` (not `TWAI_MODE_SELF_TEST`); pair it with `msg.self = 1` for single-node load testing.
- CAN safety: default LISTEN_ONLY; NORMAL mode only on a bench with exactly two nodes; 3.3 V transceiver, 120 Ω termination, common ground (see `docs/hardware.md` §5).
- SD fan-out must stay non-blocking on `twai_task`: `sd_store_push` only does `xQueueSend(...,0)` and increments a drop counter; the SD task is the only writer, and `net_task`/SD never take the LVGL mutex.

## Board

立创·实战派 ESP32-S3（无电池/无摄像头）: N16R8, 320×240 ST7789, FT6336 touch, 8 MB PSRAM, onboard TF slot (1-bit SDMMC: GPIO47/48/21, no card-detect). CAN uses J2: GPIO10=TXD, GPIO11=RXD; full pin table: `docs/hardware.md` §2 and §5.
