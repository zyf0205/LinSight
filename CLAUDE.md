# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

LinSight = wireless CAN bus analyzer: ESP32-S3 device (TWAI capture + LVGL UI + Wi-Fi streaming), a Linux host daemon, and a Windows bench GUI. The display-prototype business (JPEG/cast, `/proc` dashboard, macro keys, Windows MJPEG tool, old host) has been removed.

Source of truth:
- `docs/project_plan.md` — scope, protocol v2.0, milestones, design decisions
- `docs/hardware.md` — verified pinout, CAN wiring, LCD/touch init, sdkconfig gotchas
- `docs/ui_design.md` — UI tokens, Monitor/Settings layout, input/refresh contract (API in `main/ui.h`)
- `protocol/frame.h` — wire format, limits, parser contract (shared by both sides)
- `tools/can_probe/` — standalone TWAI bench probe used for M0 wiring validation
- `tools/bench_can.py` — bench RX/TX generator with echo watchdog and CSV record

Working copies:
- `~/linsight` (WSL2 ext4; `~/LinSight` symlink) — host working copy: build/test/run `host/`, daemon, CLI, and docs. Do host work here only; never build or run `host/` on Windows.
- `D:\_study\projects\_MyProject\LinSight` — Windows checkout: ESP-IDF firmware build/flash only. The bench GUI is legacy; the primary bench path is WSL2 SocketCAN (`can0`).
- The two trees share history and are synced by hand. After firmware edits on Windows, rsync to WSL; after host/doc edits in WSL, rsync to Windows. Never leave them diverged across a commit.

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
- Bench testing: PCAN-USB via WSL2 SocketCAN (`can0` over usbipd) is the primary traffic source; the Windows GUI is legacy and needs the PCAN detached back to Windows. Default `TWAI_MODE_LISTEN_ONLY` never ACKs — switch Settings/`menuconfig` to Normal only on the bench.

```bash
# Host daemon — WSL2 working copy (epoll/signalfd; does not build on Windows)
cd ~/linsight/host
cmake -S . -B build && cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure   # unit + integration
build/linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs
build/linsight-cli stats|live|pause|resume|clear|export asc|candump FILE.lsc
# M4 adds: linsight-cli config (explicit fields, waits for ACK)

# Bench traffic — WSL2 SocketCAN (PCAN via usbipd; no Windows echo needed)
# one-time admin bind: usbipd bind --busid 2-1
# after each replug/WSL restart: usbipd attach --wsl --busid 2-1
sudo modprobe peak_usb
sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up
cangen can0 -n 500 -g 20 -I 0x123 -L 8   # 50 fps x 10 s; device total is ground truth

# Sync the two working copies (run in WSL; build dirs and sdkconfig stay put)
# Windows -> WSL after firmware edits; WSL -> Windows after host/doc edits.
# Windows -> WSL:
rsync -a --delete --exclude 'build/' --exclude 'managed_components/' \
  --exclude sdkconfig --exclude sdkconfig.old \
  /mnt/d/_study/projects/_MyProject/LinSight/ ~/linsight/
# WSL -> Windows: same command, source and destination swapped.

# Bench GUI — Windows, needs python-can; daemon must expose --ctl-port
py -3.10 host/gui/linsight_gui.py
# GUI starts $HOME/linsight/host/build/linsightd in WSL by default; override with
# LINSIGHT_DAEMON_DIR for a different WSL checkout.
```

## Architecture

- **Firmware tasks**: `twai_task` (core 0, prio 10) receives frames, timestamps (ms), pushes 20-byte records into the PSRAM ring (~52k records, drop-oldest), mirrors the newest 512 into a capture-side UI history, and maintains counters; `net_task` (core 0, prio 5) owns Wi-Fi/TCP, streams CAN_BATCH frames, sends STATS at 1 Hz / EVENTS / PING, parses CONTROL/FLOW/PING from the host; the LVGL task pulls `twai_ui_tail` + stats at 20 Hz (the history is independent of the ring that `net_task` drains). Verified: 30000-frame boot burst at 4330 fps / 96% load, zero loss (30k < ~52k capacity, so overflow policy is covered by `host/tests/test_ring.c`).
- **Host daemon** (`host/`): `linsightd` (epoll main loop; TCP listener for the device, Unix socket `/tmp/linsight.sock` for the CLI, optional loopback TCP control port (`--ctl-port`), signalfd, 1 Hz timerfd — signalfd/timerfd created nonblocking and drained once per event) + writer thread (bounded 16384-record ring → per-session `can-YYYYMMDD-HHMMSS-NNN.lsc`, close ordered before open, writes guarded). `linsight-cli` does stats/live/pause/resume/clear/export; `live` is newline-framed candump text. In WSL2 mirrored networking the device reaches the daemon at the Windows IP directly, and Windows reaches `127.0.0.1:<ctl-port>`. Tests: `host/tests/` (record, parser, fuzz, ring, Python integration, GUI TX smoke with fake PCAN). On reconnect the device first drains the ring backlog (up to ~52k records), not only new frames.
- **Bench GUI** (`host/gui/linsight_gui.py`): Windows Tkinter + python-can; never connects to the device directly — it uses the daemon's `--ctl-port` line protocol for live trace/stats and CONTROL, and drives PCAN directly for mixed/single/ramp/random traffic. `py -3.10` is the interpreter with python-can. The primary bench path is now WSL2 SocketCAN (`can0` via usbipd; `cangen` at 50/1000 fps delivered every frame with zero loss); the GUI is legacy and only works after detaching the PCAN back to Windows.
- **Ring** (`can_ring.c`): mutex-protected SPSC-style; overwrite-oldest on overflow with a visible drop counter; sequence numbers expose gaps.
- **Shared protocol**: `protocol/frame.c` is pure C (no IDF, no malloc); host and firmware compile the same parser. 5-byte header `AA 55 type len_lo len_hi`; types 01 HELLO, 02 CAN_BATCH, 03 STATS, 04 EVENT, 05/06 PING/PONG, 07 CONTROL, 08 ACK, 09 FLOW; record `seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]`; STATS/CONTROL payloads are small JSON.
- **UI** (`ui.c`): Monitor (status chips, 8×21 px zebra trace with per-ID CNT, 20 Hz telemetry bar) + Settings (bitrate/mode, Std/Ext/locked-ID filter, one-shot error/locked-ID trigger, pause/clear/bus recover); tap a row to lock its ID; GPIO0 short = display pause, long = page switch; host CONTROL mirrors into the settings nav (REMOTE SYNC); net_task setters publish into a lock-free mailbox that the 20 Hz LVGL timer drains, while the GPIO0 handler takes the LVGL lock for its short action.

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

## Board

立创·实战派 ESP32-S3（无电池/无摄像头）: N16R8, 320×240 ST7789, FT6336 touch, 8 MB PSRAM. CAN uses J2: GPIO10=TXD, GPIO11=RXD; full pin table: `docs/hardware.md` §2 and §5.
