# CLAUDE.md

This file provides guidance to Claude Code when working in the Windows firmware checkout.

## Repository state

LinSight ESP32-S3 firmware: TWAI capture, PSRAM ring, SD black-box recording, LVGL UI, Wi-Fi streaming.
The Linux host (epoll bridge, tests, optional SocketCAN forwarding) lives in
the separate WSL repository `~/linsight`; do not keep, build, or run host code in this checkout.
Bench Wi-Fi credentials and the host IP are Kconfig defaults (`CONFIG_LINSIGHT_*` in
`main/Kconfig.projbuild`); production builds should override them via menuconfig.

## Source of truth

- `README.md` — project overview, features, build/flash, repository layout
- `docs/hardware.md` — verified pinout, CAN wiring, LCD/touch init, sdkconfig gotchas
- `protocol/frame.h` — wire format, limits, parser contract (shared with the host repo; keep both copies identical)
- `main/ui.h` — UI API and the DASH/SETUP contract consumed by `ui.c`
- `~/linsight/README.md` — host bridge daemon and bench reference

## Documentation rules

- **Firmware docs live in `docs/`; host docs live in the WSL repo (`README.md`, `docs/hardware.md`).** Root markdown is limited to `README.md` (project entry) and `CLAUDE.md` (agent guidance).
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
- Bench: the device joins the PC hotspot and connects to `linsightd` in WSL; PCAN traffic is generated on WSL `can0` (usbipd + `peak_usb`), not from Windows. Default `TWAI_MODE_LISTEN_ONLY` never ACKs — switch the SETUP page/`menuconfig` to Normal only on a two-node bench.

## Architecture

- **Firmware tasks**: `twai_task` (core 1, prio 10) receives frames, timestamps (ms), pushes 20-byte records into the PSRAM ring (4 MiB, ~209k records, drop-oldest), fans out to the SD queue, and maintains counters; `net_task` (core 0, prio 5) owns Wi-Fi/TCP, discovers the host via UDP broadcast (`CONFIG_LINSIGHT_DISCOVERY_PORT`, default 8889; fallback `CONFIG_LINSIGHT_HOST_IP/PORT`), streams CAN_BATCH frames, sends EVENTS/PING and answers PING with PONG; `sd_task` (core 1, prio 3) drains the 16k PSRAM queue and writes `recNNNNN.log`; the LVGL task pulls stats and SD status at 20 Hz. Bench-validated at 4330 fps / 96% load with zero loss; overflow policy is covered by the host repo's `test_ring`.
- **Ring** (`can_ring.c`): mutex-protected SPSC-style; overwrite-oldest on overflow with a visible drop counter; sequence numbers expose gaps.
- **SD black box** (`sd_store.c`): independent PSRAM queue (16k), never blocks `twai_task`; 1-bit SDMMC CLK/CMD/D0 = 47/48/21; FAT32/8.3 `recNNNNN.log` in candump text (`(uptime_s.usec) can0 ID#DATA`, same syntax as can-utils and the host `--dump`; opens directly in SavvyCAN), 32 MiB or 10 min rotation, keep 8 files, `.trg` mark sidecars, `README.TXT` rewritten at each mount. Timestamps are device uptime (no RTC), so SavvyCAN shows 1970 + uptime. Bench-validated 2026-11-13: mount/remount and index continuation across resets, case-insensitive 8.3 scan/prune (FATFS returns upper-case names), byte- and time-based rotation plus keep-N pruning (exercised with temporary reduced limits), zero SD drops at ~850 fps.
- **Shared protocol**: `protocol/frame.c` is pure C (no IDF, no malloc); the WSL host repo compiles a mirror of the same parser. 5-byte header `AA 55 type len_lo len_hi`; types 01 HELLO, 02 CAN_BATCH, 03 STATS, 04 EVENT, 05/06 PING/PONG, 07 CONTROL, 08 ACK, 09 FLOW, 10 DEVICE_INFO; record `seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`; HELLO carries `device_id`/`boot`/`uptime_ms`. The firmware implements only HELLO/CAN_BATCH/EVENT/PING/PONG; the other types remain in the shared header for the host mirror.
- **UI** (`ui.c`): two tabs DASH/SETUP. DASH shows bus load (with dynamic green/amber/red progress bar), FPS, RX total, a 2×2 diagnostic grid (TEC/REC/ERR+MISS/RING), SD recording/free space and a MARK button; SETUP selects bitrate (125k/250k/500k/1M) and controller mode (LISTEN/NORMAL, with amber ACK warning), provides a touch `CLEAR` button for stats, and shows the discovered host endpoint plus link/RTT. The bottom dock switches views; GPIO0 short toggles DASH/SETUP, long marks the log on DASH and clears counters on SETUP. Bitrate/mode changes run in a one-shot task outside the LVGL lock; the 20 Hz tick only reads bounded snapshots (stats mutex, SD status mutex, atomic mailbox, portMUX-protected endpoint). `net_task` setters publish into the lock-free mailbox, while the GPIO0 handler takes the LVGL lock for its short action. `settings.c` owns NVS config, boot counter, and the eFuse MAC `device_id`.
- **Touch** (`bsp.c`): a dedicated task (core 0, prio 2, 30 ms) polls FT6336 and publishes x/y/pressed under a portMUX; the LVGL indev `read_cb` only reads that cache. A stalled I2C bus cannot block `lv_timer_handler`.

## Hard constraints

- Only the LVGL task and the GPIO0 key handler touch LVGL. `linsight_ui_handle_key` runs on the button task and takes `lvgl_port_lock` for its short view-switch action; `net_task` must only publish into the lock-free mailbox in `ui.c` — never take the LVGL mutex from `net_task` (`lvgl_port_lock(0)` waits forever, which stalls PING/PONG until the host drops the session). The touch task must never call LVGL; only the indev `read_cb` (LVGL task) reads the cached point. Never call SD/FAT, NVS, TWAI restart or blocking I2C from the 20 Hz LVGL tick.
- LCD CS is **PCA9557 BIT0 @0x19**, not a GPIO. Order: `bsp_i2c_init()` → `pca9557_init()` → `panel_reset()` → write PCA9557 output `0x04` (CS low) → `panel_init()`.
- LVGL draw buffers: internal DMA RAM, 320×40 ×2. CAN ring: PSRAM.
- `CONFIG_LV_COLOR_16_SWAP=y`.
- sdkconfig: CPU 240 MHz, `CONFIG_COMPILER_OPTIMIZATION_PERF`, `FREERTOS_HZ=1000`, TCP window 32 KB.
- Single TCP client; `net_task` is the only firmware writer.
- Protocol parser must stay allocation-free and IDF-independent so it can be fuzzed on the host.
- TWAI macros (`TWAI_TIMING_CONFIG_*`, `TWAI_FILTER_CONFIG_ACCEPT_ALL`) expand to struct initializers — they can only initialize objects, never be passed as arguments.
- CAN safety: default LISTEN_ONLY; NORMAL mode only on a bench with exactly two nodes; 3.3 V transceiver, 120 Ω termination, common ground (see `docs/hardware.md` §5).
- SD fan-out must stay non-blocking on `twai_task`: `sd_store_push` only does `xQueueSend(...,0)` and increments a drop counter; the SD task is the only writer, and `net_task`/SD never take the LVGL mutex.

## Board

立创·实战派 ESP32-S3（无电池/无摄像头）: N16R8, 320×240 ST7789, FT6336 touch, 8 MB PSRAM, onboard TF slot (1-bit SDMMC: GPIO47/48/21, no card-detect). CAN uses J2: GPIO10=TXD, GPIO11=RXD; full pin table: `docs/hardware.md` §2 and §5.
