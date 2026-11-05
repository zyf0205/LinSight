# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

LinSight = wireless CAN bus analyzer: ESP32-S3 device (TWAI capture + LVGL UI + Wi-Fi streaming), a Linux host daemon, and a Windows bench GUI. The display-prototype business (JPEG/cast, `/proc` dashboard, macro keys, Windows MJPEG tool, old host) has been removed.

Source of truth:
- `docs/project_plan.md` — scope, protocol v2.0, milestones, design decisions
- `docs/hardware.md` — verified pinout, CAN wiring, LCD/touch init, sdkconfig gotchas
- `docs/ui_design.md` — UI tokens, Monitor/Settings layout, C API contract
- `protocol/frame.h` — wire format, limits, parser contract (shared by both sides)
- `tools/can_probe/` — standalone TWAI bench probe used for M0 wiring validation

## Documentation rules

- **All project docs live in `docs/`.** The only markdown at the repo root is `CLAUDE.md`.
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
idf.py menuconfig                  # LinSight → Wi-Fi SSID/password, host IP:port, CAN mode
```

- Wi-Fi credentials and the host IP live in the ignored `sdkconfig` (set once with `idf.py menuconfig`); never put them in `sdkconfig.defaults` or docs.
- Bench testing: PCAN-USB (Windows, python-can) feeds frames over the transceiver; default `TWAI_MODE_LISTEN_ONLY` never ACKs — switch Settings/`menuconfig` to Normal only on the bench.

```bash
# Host daemon — build in WSL/Linux (epoll/signalfd; does not build on Windows)
cmake -S host -B ~/linsight-host-build && cmake --build ~/linsight-host-build -j$(nproc)
ctest --test-dir ~/linsight-host-build --output-on-failure   # unit + integration
~/linsight-host-build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs
~/linsight-host-build/linsight-cli stats|live|pause|resume|clear|export asc|candump FILE.lsc

# Bench GUI — Windows, needs python-can; daemon must expose --ctl-port
py -3.10 host/gui/linsight_gui.py
```

## Architecture

- **Firmware tasks**: `twai_task` (core 0, prio 10) receives frames, timestamps (ms), pushes 20-byte records into the PSRAM ring (~52k records, drop-oldest), mirrors the newest 512 into a capture-side UI history, and maintains counters; `net_task` (core 0, prio 5) owns Wi-Fi/TCP, streams CAN_BATCH frames, sends STATS at 1 Hz / EVENTS / PING, parses CONTROL/FLOW/PING from the host; the LVGL task pulls `twai_ui_tail` + stats at 20 Hz (the history is independent of the ring that `net_task` drains). Verified: 30000-frame boot burst at 4330 fps / 96% load, zero loss.
- **Host daemon** (`host/`): `linsightd` (epoll main loop; TCP listener for the device, Unix socket `/tmp/linsight.sock` for the CLI, optional loopback TCP control port (`--ctl-port`), signalfd, 1 Hz timerfd — signalfd/timerfd created nonblocking and drained once per event) + writer thread (bounded 16384-record ring → per-session `can-YYYYMMDD-HHMMSS-NNN.lsc`, close ordered before open, writes guarded). `linsight-cli` does stats/live/pause/resume/clear/export; `live` is newline-framed candump text. In WSL2 mirrored networking the device reaches the daemon at the Windows IP directly, and Windows reaches `127.0.0.1:<ctl-port>`. Tests: `host/tests/` (record, parser, Python integration with a fake device).
- **Bench GUI** (`host/gui/linsight_gui.py`): Windows Tkinter + python-can; never connects to the device directly — it uses the daemon's `--ctl-port` line protocol for live trace/stats and CONTROL, and drives PCAN directly for mixed/single/ramp/random traffic. `py -3.10` is the interpreter with python-can.
- **Ring** (`can_ring.c`): mutex-protected SPSC-style; overwrite-oldest on overflow with a visible drop counter; sequence numbers expose gaps.
- **Shared protocol**: `protocol/frame.c` is pure C (no IDF, no malloc); host and firmware compile the same parser. 5-byte header `AA 55 type len_lo len_hi`; types 01 HELLO, 02 CAN_BATCH, 03 STATS, 04 EVENT, 05/06 PING/PONG, 07 CONTROL, 08 ACK, 09 FLOW; record `seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`; STATS/CONTROL payloads are small JSON.
- **UI** (`ui.c`): Monitor (status chips, 8×21 px zebra trace with per-ID CNT, 20 Hz telemetry bar) + Settings (bitrate/mode, Std/Ext/locked-ID filter, one-shot error/locked-ID trigger, pause/clear/bus recover); tap a row to lock its ID; GPIO0 short = display pause, long = page switch; host CONTROL mirrors into the settings nav (REMOTE SYNC); cross-task setters (net_task/GPIO) publish into a lock-free mailbox that the 20 Hz LVGL timer drains.

## Hard constraints

- Only the LVGL task touches LVGL. Cross-task setters publish into the lock-free mailbox in `ui.c`; never take the LVGL mutex from `net_task` (`lvgl_port_lock(0)` waits forever, which stalls PING/PONG until the host drops the session).
- LCD CS is **PCA9557 BIT0 @0x19**, not a GPIO. Order: `bsp_i2c_init()` → `pca9557_init()` → `panel_reset()` → `lcd_cs(0)` → `panel_init()`.
- LVGL draw buffers: internal DMA RAM, 320×40 ×2. CAN ring: PSRAM.
- `CONFIG_LV_COLOR_16_SWAP=y`.
- sdkconfig: CPU 240 MHz, `CONFIG_COMPILER_OPTIMIZATION_PERF`, `FREERTOS_HZ=1000`, TCP window 32 KB.
- Single TCP client; `net_task` is the only firmware writer.
- Protocol parser must stay allocation-free and IDF-independent so it can be fuzzed on the host.
- TWAI macros (`TWAI_TIMING_CONFIG_*`, `TWAI_FILTER_CONFIG_ACCEPT_ALL`) expand to struct initializers — they can only initialize objects, never be passed as arguments. Self-test mode in IDF 5.5 is `TWAI_MODE_NO_ACK` (not `TWAI_MODE_SELF_TEST`); pair it with `msg.self = 1` for single-node load testing.
- CAN safety: default LISTEN_ONLY; NORMAL mode only on a bench with exactly two nodes; 3.3 V transceiver, 120 Ω termination, common ground (see `docs/hardware.md` §5).

## Board

立创·实战派 ESP32-S3（无电池/无摄像头）: N16R8, 320×240 ST7789, FT6336 touch, 8 MB PSRAM. CAN uses J2: GPIO10=TXD, GPIO11=RXD; full pin table: `docs/hardware.md` §2 and §5.
