#!/usr/bin/env python3
"""LinSight CAN 台架助手 — Windows usbipd + WSL2 SocketCAN 轻量控制台.

用法: python tools/can_gui.py (纯 Python 3 标准库 + tkinter, 零第三方依赖)
"""

from __future__ import annotations

import ctypes
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import messagebox, ttk

APP_TITLE = "LinSight CAN 台架助手 (WSL2 SocketCAN)"
DEFAULT_VID_PID = "0c72:000c"
DEFAULT_IFACE = "can0"
BITRATES = [("125 kbps", 125000), ("250 kbps", 250000), ("500 kbps", 500000), ("1 Mbps", 1000000)]
PRESETS = [
    ("心跳 0x123", "123", "11 22 33 44 55 66 77 88", False, False),
    ("扩展帧 0x18FF50E5", "18FF50E5", "01 02 03 04 05 06 07 08", True, False),
    ("OBD 请求 0x7DF", "7DF", "02 01 0C 00 00 00 00 00", False, False),
    ("全零复位 0x100", "100", "00 00 00 00 00 00 00 00", False, False),
]
CONFIG_PATH = Path(os.environ.get("APPDATA") or Path.home()) / "LinSight" / "can_gui.json"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# 工业深色主题 (与固件屏幕配色一致)
C_BG, C_CARD, C_BORDER, C_INPUT = "#080D16", "#111A2B", "#1F304D", "#0C1422"
C_TEXT, C_SUB, C_DIM = "#F8FAFC", "#94A3B8", "#64748B"
C_ACCENT, C_OK, C_WARN, C_ERR = "#00F0FF", "#00E676", "#FFB300", "#FF2A54"

CANDUMP_RE = re.compile(
    r"^(?:\((?P<ts>[\d.]+)\)\s+)?(?P<iface>\S+)\s+(?P<id>[0-9A-Fa-f]+)\s+"
    r"\[(?P<dlc>\d+)\]\s*(?P<rest>.*)$"
)
BUSID_LINE_RE = re.compile(
    r"^\s*(?P<busid>\d+-\d+(?:\.\d+)*)\s+(?P<vidpid>[0-9a-fA-F]{4}:[0-9a-fA-F]{4})\s+"
    r"(?P<rest>.+?)\s{2,}(?P<state>Not shared|Shared|Attached.*|Blocked.*)\s*$"
)


def find_usbipd() -> str:
    found = shutil.which("usbipd") or shutil.which("usbipd.exe")
    if found:
        return found
    candidate = Path(r"C:\Program Files\usbipd-win\usbipd.exe")
    return str(candidate) if candidate.exists() else "usbipd"


def run_cmd(args: list[str], timeout: float = 12.0) -> subprocess.CompletedProcess:
    try:
        raw = subprocess.run(args, capture_output=True, timeout=timeout, creationflags=NO_WINDOW)
        out = (raw.stdout or b"").replace(b"\x00", b"").decode("utf-8", errors="replace")
        err = (raw.stderr or b"").replace(b"\x00", b"").decode("utf-8", errors="replace")
        return subprocess.CompletedProcess(args, raw.returncode, out, err)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(args, 124, "", f"命令超时 ({timeout}s)")
    except FileNotFoundError as exc:
        return subprocess.CompletedProcess(args, 127, "", f"找不到命令: {exc}")
    except Exception as exc:
        return subprocess.CompletedProcess(args, 1, "", str(exc))


def run_elevated(exe: str, params: str) -> bool:
    if sys.platform != "win32":
        return False
    return int(ctypes.windll.shell32.ShellExecuteW(None, "runas", exe, params, None, 0)) > 32


def wsl_cmd(distro: str, script: str, root: bool = False) -> list[str]:
    cmd = ["wsl.exe"]
    if distro:
        cmd.extend(["-d", distro])
    if root:
        cmd.extend(["-u", "root"])
    cmd.extend(["--", "bash", "-lc", script])
    return cmd


def list_wsl_distros() -> list[str]:
    res = run_cmd(["wsl.exe", "-l", "-q"], timeout=5.0)
    if res.returncode != 0:
        return ["Ubuntu"]
    items = [line.strip().lstrip("\ufeff") for line in res.stdout.splitlines() if line.strip()]
    return items or ["Ubuntu"]


def parse_usbipd_devices(output: str) -> list[dict[str, str]]:
    devices: list[dict[str, str]] = []
    for line in output.splitlines():
        m = BUSID_LINE_RE.match(line)
        if m:
            devices.append({
                "busid": m.group("busid"),
                "vidpid": m.group("vidpid").lower(),
                "name": m.group("rest").strip(),
                "state": m.group("state").strip(),
            })
    return devices


def format_cansend_arg(id_str: str, data_str: str, extended: bool, rtr: bool, dlc_str: str = "8") -> str:
    clean_id = id_str.strip().upper().removeprefix("0X")
    if not clean_id or not all(c in "0123456789ABCDEF" for c in clean_id):
        raise ValueError("CAN ID 必须是十六进制 (例如 123 或 18FF50E5)")
    id_val = int(clean_id, 16)
    if id_val > 0x1FFFFFFF:
        raise ValueError("CAN ID 超出 29-bit 范围 (最大 1FFFFFFF)")
    if not extended and id_val > 0x7FF:
        extended = True
    id_fmt = f"{id_val:08X}" if extended else f"{id_val:03X}"

    if rtr:
        dlc = int(dlc_str.strip() or "0")
        if not 0 <= dlc <= 8:
            raise ValueError("RTR DLC 必须在 0..8 之间")
        return f"{id_fmt}#R{dlc}" if dlc > 0 else f"{id_fmt}#R"

    hex_clean = re.sub(r"[\s,:-]+", "", data_str.strip()).upper()
    if len(hex_clean) % 2 != 0:
        raise ValueError("数据字节必须是偶数个十六进制字符 (如 11 22 33)")
    if not all(c in "0123456789ABCDEF" for c in hex_clean):
        raise ValueError("数据包含非十六进制字符")
    if len(hex_clean) // 2 > 8:
        raise ValueError("经典 CAN 单帧最多 8 字节")
    return f"{id_fmt}#{hex_clean}"


class ToolApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(APP_TITLE)
        self.root.geometry("1120x720")
        self.root.minsize(980, 620)
        self.root.configure(bg=C_BG)

        self.ui_queue: queue.Queue = queue.Queue()
        self.busy = False
        self.candump_proc: subprocess.Popen | None = None
        self.cangen_proc: subprocess.Popen | None = None
        self.rx_paused = False
        self.rx_seq = 0
        self.last_rx_ts: float | None = None
        self.prev_tx_frames: int | None = None
        self.prev_rx_frames: int | None = None
        self.prev_stat_time: float | None = None
        self.custom_presets: list[dict] = []

        self.distro_var = tk.StringVar(value="Ubuntu")
        self.iface_var = tk.StringVar(value=DEFAULT_IFACE)
        self.vidpid_var = tk.StringVar(value=DEFAULT_VID_PID)
        self.busid_var = tk.StringVar(value="自动识别")
        self.bitrate_var = tk.StringVar(value=BITRATES[2][0])

        self.usb_badge_var = tk.StringVar(value="USB: 未检测")
        self.can_badge_var = tk.StringVar(value="can0: 未就绪")
        self.stats_var = tk.StringVar(value="TX 0  |  RX 0  |  0 fps  |  TEC 0 / REC 0  |  STATE -")

        self.tx_id_var = tk.StringVar(value="123")
        self.tx_ext_var = tk.BooleanVar(value=False)
        self.tx_rtr_var = tk.BooleanVar(value=False)
        self.tx_data_var = tk.StringVar(value="11 22 33 44 55 66 77 88")
        self.tx_dlc_var = tk.StringVar(value="8 B")
        self.custom_preset_var = tk.StringVar(value="")

        self.gen_gap_var = tk.StringVar(value="10")
        self.gen_cnt_var = tk.StringVar(value="-1")
        self.gen_mode_var = tk.StringVar(value="递增")
        self.gen_status_var = tk.StringVar(value="待命")

        self.filter_var = tk.StringVar(value="")
        self.rx_count_var = tk.StringVar(value="0 帧")

        self._load_config()
        self._init_styles()
        self._build_ui()

        self.tx_data_var.trace_add("write", lambda *_: self._update_dlc_label())
        self.tx_id_var.trace_add("write", lambda *_: self._auto_check_ext())
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.root.after(40, self._drain_ui_queue)
        self.root.after(200, self.refresh_environment)
        self.root.after(2000, self._periodic_status_tick)

    def _load_config(self) -> None:
        try:
            if CONFIG_PATH.exists():
                cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
                self.distro_var.set(cfg.get("distro", self.distro_var.get()))
                self.iface_var.set(cfg.get("iface", self.iface_var.get()))
                self.vidpid_var.set(cfg.get("vidpid", self.vidpid_var.get()))
                self.bitrate_var.set(cfg.get("bitrate", self.bitrate_var.get()))
                self.tx_id_var.set(cfg.get("tx_id", self.tx_id_var.get()))
                self.tx_data_var.set(cfg.get("tx_data", self.tx_data_var.get()))
                self.tx_ext_var.set(bool(cfg.get("tx_ext", False)))
                self.gen_gap_var.set(cfg.get("gen_gap", self.gen_gap_var.get()))
                self.custom_presets = cfg.get("custom_presets", [])
        except Exception:
            pass

    def _save_config(self) -> None:
        try:
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            cfg = {
                "distro": self.distro_var.get(),
                "iface": self.iface_var.get(),
                "vidpid": self.vidpid_var.get(),
                "bitrate": self.bitrate_var.get(),
                "tx_id": self.tx_id_var.get(),
                "tx_data": self.tx_data_var.get(),
                "tx_ext": self.tx_ext_var.get(),
                "gen_gap": self.gen_gap_var.get(),
                "custom_presets": self.custom_presets,
            }
            CONFIG_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception:
            pass

    def _init_styles(self) -> None:
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure(".", background=C_BG, foreground=C_TEXT, font=("Segoe UI", 10))
        style.configure("Treeview", background=C_INPUT, fieldbackground=C_INPUT, foreground=C_TEXT, rowheight=25, borderwidth=0, font=("Consolas", 10))
        style.configure("Treeview.Heading", background=C_CARD, foreground=C_SUB, relief="flat", font=("Segoe UI Semibold", 9))
        style.map("Treeview", background=[("selected", "#19335A")], foreground=[("selected", C_ACCENT)])
        style.configure("TCombobox", fieldbackground=C_INPUT, background=C_CARD, foreground=C_TEXT, arrowcolor=C_ACCENT)
        style.map("TCombobox", fieldbackground=[("readonly", C_INPUT)], foreground=[("readonly", C_TEXT)])

    def _make_btn(self, parent: tk.Widget, text: str, cmd, bg: str = C_BORDER, fg: str = C_TEXT) -> tk.Button:
        return tk.Button(
            parent, text=text, command=cmd, bg=bg, fg=fg,
            activebackground=C_ACCENT, activeforeground=C_BG,
            relief="flat", bd=0, padx=12, pady=5, cursor="hand2", font=("Segoe UI Semibold", 9),
        )

    def _make_entry(self, parent: tk.Widget, var: tk.StringVar, width: int = 12, mono: bool = False) -> tk.Entry:
        return tk.Entry(
            parent, textvariable=var, width=width, bg=C_INPUT, fg=C_TEXT,
            insertbackground=C_ACCENT, relief="flat", highlightthickness=1,
            highlightbackground=C_BORDER, highlightcolor=C_ACCENT,
            font=("Consolas", 10) if mono else ("Segoe UI", 10),
        )

    def _build_ui(self) -> None:
        # 1. 顶部连接与状态栏
        top = tk.Frame(self.root, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1, padx=14, pady=10)
        top.pack(fill="x", padx=12, pady=(12, 6))

        row1 = tk.Frame(top, bg=C_CARD)
        row1.pack(fill="x")
        tk.Label(row1, text="LINSIGHT BENCH", bg=C_CARD, fg=C_ACCENT, font=("Segoe UI Bold", 12)).pack(side="left", padx=(0, 14))
        tk.Label(row1, text="WSL:", bg=C_CARD, fg=C_SUB).pack(side="left")
        self.distro_cb = ttk.Combobox(row1, textvariable=self.distro_var, width=12, state="readonly")
        self.distro_cb.pack(side="left", padx=(4, 10))
        tk.Label(row1, text="接口:", bg=C_CARD, fg=C_SUB).pack(side="left")
        self._make_entry(row1, self.iface_var, width=6, mono=True).pack(side="left", padx=(4, 10))
        tk.Label(row1, text="VID:PID:", bg=C_CARD, fg=C_SUB).pack(side="left")
        self._make_entry(row1, self.vidpid_var, width=10, mono=True).pack(side="left", padx=(4, 10))
        tk.Label(row1, text="波特率:", bg=C_CARD, fg=C_SUB).pack(side="left")
        self.br_cb = ttk.Combobox(row1, textvariable=self.bitrate_var, values=[b[0] for b in BITRATES], width=10, state="readonly")
        self.br_cb.pack(side="left", padx=(4, 12))

        self.connect_btn = self._make_btn(row1, "⚡ 一键连接 USB-CAN", lambda: self.do_connect(False), bg=C_OK, fg=C_BG)
        self.connect_btn.pack(side="left", padx=(0, 6))
        self._make_btn(row1, "应用波特率", self.do_apply_bitrate).pack(side="left", padx=(0, 6))
        self._make_btn(row1, "强制重挂", lambda: self.do_connect(True)).pack(side="left", padx=(0, 6))
        self._make_btn(row1, "断开", self.do_disconnect).pack(side="left")

        row2 = tk.Frame(top, bg=C_CARD)
        row2.pack(fill="x", pady=(8, 0))
        self.usb_lbl = tk.Label(row2, textvariable=self.usb_badge_var, bg=C_INPUT, fg=C_SUB, padx=10, pady=3, font=("Consolas", 9))
        self.usb_lbl.pack(side="left", padx=(0, 8))
        self.can_lbl = tk.Label(row2, textvariable=self.can_badge_var, bg=C_INPUT, fg=C_SUB, padx=10, pady=3, font=("Consolas", 9))
        self.can_lbl.pack(side="left", padx=(0, 12))
        tk.Label(row2, textvariable=self.stats_var, bg=C_CARD, fg=C_ACCENT, font=("Consolas", 10)).pack(side="right")

        # 2. 发送与压测面板
        mid = tk.Frame(self.root, bg=C_BG)
        mid.pack(fill="x", padx=12, pady=6)

        tx_card = tk.Frame(mid, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1, padx=14, pady=10)
        tx_card.pack(side="left", fill="both", expand=True, padx=(0, 6))
        tk.Label(tx_card, text="单帧发送 (cansend)", bg=C_CARD, fg=C_TEXT, font=("Segoe UI Bold", 10)).pack(anchor="w")

        f_row = tk.Frame(tx_card, bg=C_CARD)
        f_row.pack(fill="x", pady=(8, 6))
        tk.Label(f_row, text="ID(Hex):", bg=C_CARD, fg=C_SUB).pack(side="left")
        id_ent = self._make_entry(f_row, self.tx_id_var, width=10, mono=True)
        id_ent.pack(side="left", padx=(4, 10))
        id_ent.bind("<Return>", lambda _: self.do_send_once())
        tk.Checkbutton(f_row, text="29-bit 扩展帧", variable=self.tx_ext_var, bg=C_CARD, fg=C_SUB, selectcolor=C_INPUT, activebackground=C_CARD).pack(side="left", padx=(0, 6))
        tk.Checkbutton(f_row, text="RTR", variable=self.tx_rtr_var, bg=C_CARD, fg=C_SUB, selectcolor=C_INPUT, activebackground=C_CARD).pack(side="left", padx=(0, 10))
        tk.Label(f_row, text="Data(Hex):", bg=C_CARD, fg=C_SUB).pack(side="left")
        data_ent = self._make_entry(f_row, self.tx_data_var, width=25, mono=True)
        data_ent.pack(side="left", padx=(4, 8), fill="x", expand=True)
        data_ent.bind("<Return>", lambda _: self.do_send_once())
        tk.Label(f_row, textvariable=self.tx_dlc_var, bg=C_INPUT, fg=C_ACCENT, width=5, font=("Consolas", 9)).pack(side="left", padx=(0, 8))
        self._make_btn(f_row, "发送 (Enter)", self.do_send_once, bg=C_ACCENT, fg=C_BG).pack(side="left")

        p_row = tk.Frame(tx_card, bg=C_CARD)
        p_row.pack(fill="x", pady=(4, 0))
        tk.Label(p_row, text="快捷帧:", bg=C_CARD, fg=C_DIM, font=("Segoe UI", 9)).pack(side="left", padx=(0, 6))
        for name, cid, data, ext, rtr in PRESETS:
            self._make_btn(p_row, name, lambda c=cid, d=data, e=ext, r=rtr: self._apply_and_send_preset(c, d, e, r), bg=C_INPUT, fg=C_SUB).pack(side="left", padx=(0, 6))

        self.preset_cb = ttk.Combobox(p_row, textvariable=self.custom_preset_var, width=14, state="readonly")
        self.preset_cb.pack(side="left", padx=(6, 4))
        self.preset_cb.bind("<<ComboboxSelected>>", self._on_select_custom_preset)
        self._make_btn(p_row, "+存预设", self._save_custom_preset, bg=C_INPUT, fg=C_ACCENT).pack(side="left", padx=(0, 4))
        self._make_btn(p_row, "删", self._delete_custom_preset, bg=C_INPUT, fg=C_DIM).pack(side="left")
        self._refresh_custom_preset_combo()

        # 右: 连续流量发生器 (cangen)
        gen_card = tk.Frame(mid, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1, padx=14, pady=10)
        gen_card.pack(side="right", fill="y")
        h_gen = tk.Frame(gen_card, bg=C_CARD)
        h_gen.pack(fill="x")
        tk.Label(h_gen, text="连续压测 (cangen)", bg=C_CARD, fg=C_TEXT, font=("Segoe UI Bold", 10)).pack(side="left")
        self.gen_badge = tk.Label(h_gen, textvariable=self.gen_status_var, bg=C_INPUT, fg=C_DIM, padx=8, font=("Consolas", 9))
        self.gen_badge.pack(side="right")

        g_row1 = tk.Frame(gen_card, bg=C_CARD)
        g_row1.pack(fill="x", pady=(8, 6))
        tk.Label(g_row1, text="间隔(ms):", bg=C_CARD, fg=C_SUB).pack(side="left")
        self._make_entry(g_row1, self.gen_gap_var, width=5, mono=True).pack(side="left", padx=(4, 10))
        tk.Label(g_row1, text="帧数(-1无限):", bg=C_CARD, fg=C_SUB).pack(side="left")
        self._make_entry(g_row1, self.gen_cnt_var, width=6, mono=True).pack(side="left", padx=(4, 10))
        tk.Label(g_row1, text="数据:", bg=C_CARD, fg=C_SUB).pack(side="left")
        ttk.Combobox(g_row1, textvariable=self.gen_mode_var, values=["递增", "当前Data", "随机"], width=8, state="readonly").pack(side="left", padx=(4, 0))

        g_row2 = tk.Frame(gen_card, bg=C_CARD)
        g_row2.pack(fill="x", pady=(4, 0))
        self.gen_start_btn = self._make_btn(g_row2, "▶ 启动连续发包", self.start_cangen, bg=C_OK, fg=C_BG)
        self.gen_start_btn.pack(side="left", fill="x", expand=True, padx=(0, 6))
        self.gen_stop_btn = self._make_btn(g_row2, "■ 停止", self.stop_cangen, bg=C_ERR, fg=C_TEXT)
        self.gen_stop_btn.pack(side="left")

        # 3. 实时报文监控 + 底部精简控制台
        bot = tk.PanedWindow(self.root, orient="vertical", bg=C_BG, sashwidth=4, bd=0)
        bot.pack(fill="both", expand=True, padx=12, pady=(6, 12))

        rx_card = tk.Frame(bot, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1, padx=12, pady=8)
        bot.add(rx_card, stretch="always", minsize=260)

        rx_bar = tk.Frame(rx_card, bg=C_CARD)
        rx_bar.pack(fill="x", pady=(0, 6))
        tk.Label(rx_bar, text="实时总线监控 (candump)", bg=C_CARD, fg=C_TEXT, font=("Segoe UI Bold", 10)).pack(side="left", padx=(0, 12))
        tk.Label(rx_bar, text="ID/数据过滤:", bg=C_CARD, fg=C_SUB).pack(side="left")
        self._make_entry(rx_bar, self.filter_var, width=14, mono=True).pack(side="left", padx=(4, 12))
        self.pause_btn = self._make_btn(rx_bar, "暂停滚动", self.toggle_rx_pause)
        self.pause_btn.pack(side="left", padx=(0, 6))
        self._make_btn(rx_bar, "清空列表", self.clear_rx_table).pack(side="left")
        tk.Label(rx_bar, textvariable=self.rx_count_var, bg=C_CARD, fg=C_ACCENT, font=("Consolas", 10)).pack(side="right")

        cols = ("seq", "time", "dt", "id", "type", "dlc", "data", "ascii")
        self.tree = ttk.Treeview(rx_card, columns=cols, show="headings", selectmode="browse")
        headers = [("#", 55), ("时间", 95), ("Δt(ms)", 75), ("CAN ID", 95), ("类型", 60), ("DLC", 45), ("数据 (Hex)", 250), ("ASCII", 110)]
        for col, (title, w) in zip(cols, headers):
            self.tree.heading(col, text=title)
            self.tree.column(col, width=w, anchor="w" if col in ("data", "ascii") else "center")

        sb = ttk.Scrollbar(rx_card, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        log_card = tk.Frame(bot, bg=C_CARD, highlightbackground=C_BORDER, highlightthickness=1, padx=12, pady=6)
        bot.add(log_card, stretch="never", minsize=105)
        self.log_text = tk.Text(log_card, height=4, bg=C_INPUT, fg=C_SUB, relief="flat", font=("Consolas", 9), state="disabled")
        self.log_text.pack(fill="both", expand=True)

    def log(self, msg: str) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        self.ui_queue.put(("log", f"[{ts}] {msg}\n"))

    def _update_dlc_label(self) -> None:
        raw = re.sub(r"[\s,:-]+", "", self.tx_data_var.get())
        self.tx_dlc_var.set(f"{len(raw) // 2} B")

    def _auto_check_ext(self) -> None:
        raw = self.tx_id_var.get().strip().upper().removeprefix("0X")
        try:
            if raw and int(raw, 16) > 0x7FF:
                self.tx_ext_var.set(True)
        except ValueError:
            pass

    def _selected_bitrate(self) -> int:
        label = self.bitrate_var.get()
        for name, val in BITRATES:
            if name == label:
                return val
        return 500000

    def _run_async(self, fn) -> None:
        if self.busy:
            self.log("已有后台操作正在执行，请稍候...")
            return
        self.busy = True
        self.connect_btn.configure(state="disabled")

        def wrapper() -> None:
            try:
                fn()
            except Exception as exc:
                self.log(f"错误: {exc}")
            finally:
                self.ui_queue.put(("busy_done", None))

        threading.Thread(target=wrapper, daemon=True).start()

    def _refresh_custom_preset_combo(self) -> None:
        names = [p.get("name", "") for p in self.custom_presets if p.get("name")]
        self.preset_cb.configure(values=names)
        if names and not self.custom_preset_var.get():
            self.custom_preset_var.set(names[0])

    def _on_select_custom_preset(self, _event=None) -> None:
        name = self.custom_preset_var.get()
        for p in self.custom_presets:
            if p.get("name") == name:
                self._apply_and_send_preset(p["id"], p["data"], bool(p.get("ext")), bool(p.get("rtr")))
                break

    def _save_custom_preset(self) -> None:
        cid = self.tx_id_var.get().strip().upper()
        data = self.tx_data_var.get().strip().upper()
        nbytes = len(re.sub(r"[\s,:-]+", "", data)) // 2
        name = f"0x{cid} ({nbytes}B)"
        self.custom_presets = [p for p in self.custom_presets if p.get("name") != name]
        self.custom_presets.append({"name": name, "id": cid, "data": data, "ext": self.tx_ext_var.get(), "rtr": self.tx_rtr_var.get()})
        self._refresh_custom_preset_combo()
        self.custom_preset_var.set(name)
        self._save_config()
        self.log(f"已保存预设: {name}")

    def _delete_custom_preset(self) -> None:
        name = self.custom_preset_var.get()
        if not name:
            return
        self.custom_presets = [p for p in self.custom_presets if p.get("name") != name]
        self.custom_preset_var.set("")
        self._refresh_custom_preset_combo()
        self._save_config()
        self.log(f"已删除预设: {name}")

    def refresh_environment(self) -> None:
        def worker() -> None:
            self.ui_queue.put(("distros", list_wsl_distros()))
            self._probe_usb_and_can()

        threading.Thread(target=worker, daemon=True).start()

    def _find_target_usb(self) -> dict[str, str] | None:
        res = run_cmd([find_usbipd(), "list"], timeout=6.0)
        if res.returncode != 0:
            return None
        devices = parse_usbipd_devices(res.stdout)
        target_vp = self.vidpid_var.get().strip().lower()
        for d in devices:
            if d["vidpid"] == target_vp:
                return d
        for d in devices:
            if any(k in d["name"].lower() for k in ("pcan", "peak", "can", "gs_usb", "candlelight")):
                return d
        return None

    def _probe_usb_and_can(self) -> dict[str, str] | None:
        dev = self._find_target_usb()
        if dev:
            self.busid_var.set(dev["busid"])
            col = C_OK if "Attached" in dev["state"] else C_WARN
            self.ui_queue.put(("usb_badge", (f"USB {dev['busid']} ({dev['vidpid']}) · {dev['state']}", col)))
        else:
            self.ui_queue.put(("usb_badge", ("USB: 未检测到 CAN 适配器", C_ERR)))

        distro = self.distro_var.get().strip()
        iface = self.iface_var.get().strip() or DEFAULT_IFACE
        res = run_cmd(wsl_cmd(distro, f"ip -j -d -s link show {iface}"), timeout=5.0)
        if res.returncode == 0 and res.stdout.strip():
            self._parse_ip_link_json(res.stdout, iface)
        else:
            self.ui_queue.put(("can_badge", (f"{iface}: 未就绪", C_DIM)))
        return dev

    def _parse_ip_link_json(self, raw_json: str, iface: str) -> None:
        try:
            info = json.loads(raw_json)[0]
            oper = info.get("operstate", "UNKNOWN")
            is_up = "UP" in info.get("flags", []) or oper == "UP"
            li = info.get("linkinfo", {}).get("info_data", {})
            br = li.get("bittiming", {}).get("bitrate", 0)
            state = li.get("state", oper)
            berr = li.get("berr_counter", {})
            tec, rec = berr.get("tx", 0), berr.get("rx", 0)
            st = info.get("stats64", info.get("stats", {}))
            tx_f, rx_f = st.get("tx", {}).get("packets", 0), st.get("rx", {}).get("packets", 0)

            now = time.monotonic()
            fps = 0
            if self.prev_stat_time and now > self.prev_stat_time:
                df = max(0, (tx_f - (self.prev_tx_frames or tx_f)) + (rx_f - (self.prev_rx_frames or rx_f)))
                fps = int(round(df / (now - self.prev_stat_time)))
            self.prev_tx_frames, self.prev_rx_frames, self.prev_stat_time = tx_f, rx_f, now

            br_str = f"{br // 1000}k" if br else "?"
            badge_col = C_OK if is_up and state not in ("BUS-OFF", "STOPPED") else (C_ERR if "BUS-OFF" in state else C_WARN)
            self.ui_queue.put(("can_badge", (f"{iface}: {'UP' if is_up else 'DOWN'} @ {br_str}", badge_col)))
            self.ui_queue.put(("stats", f"TX {tx_f}  |  RX {rx_f}  |  {fps} fps  |  TEC {tec} / REC {rec}  |  {state}"))
        except Exception:
            pass

    def do_connect(self, force_reattach: bool = False) -> None:
        self._save_config()

        def task() -> None:
            usbipd = find_usbipd()
            distro = self.distro_var.get().strip()
            iface = self.iface_var.get().strip() or DEFAULT_IFACE
            bitrate = self._selected_bitrate()

            self.log("正在检查 USB-CAN 适配器状态...")
            dev = self._find_target_usb()
            if not dev:
                self.log("未找到 USB-CAN 设备，请确认适配器已插入电脑 USB 端口。")
                self._probe_usb_and_can()
                return

            busid, state = dev["busid"], dev["state"]
            self.log(f"识别到 {dev['name']} [BUSID {busid}, 状态: {state}]")

            if force_reattach and "Attached" in state:
                self.log(f"正在强制卸载 BUSID {busid}...")
                run_cmd([usbipd, "detach", "--busid", busid], timeout=8.0)
                time.sleep(0.6)
                dev = self._find_target_usb() or dev
                state = dev["state"]

            if state == "Not shared":
                self.log(f"首次绑定 BUSID {busid} (如弹出 UAC 窗口请点击“是”)...")
                res = run_cmd([usbipd, "bind", "--busid", busid], timeout=8.0)
                if res.returncode != 0:
                    if not run_elevated(usbipd, f"bind --busid {busid}"):
                        self.log("UAC 提权 bind 被取消或失败。")
                        return
                    time.sleep(1.5)

            run_cmd(wsl_cmd(distro, "true"), timeout=8.0)
            dev = self._find_target_usb() or dev
            if "Attached" not in dev["state"]:
                self.log(f"正在将 BUSID {busid} 挂载到 WSL2 ({distro})...")
                att = run_cmd([usbipd, "attach", "--wsl", "--busid", busid], timeout=12.0)
                if att.returncode != 0 and "already attached" not in (att.stdout + att.stderr).lower():
                    self.log(f"attach 失败: {(att.stderr or att.stdout).strip()}")
                    return

            self.log(f"正在配置 WSL2 {iface} 波特率 {bitrate} bps...")
            setup_sh = (
                "modprobe peak_usb 2>/dev/null || true; modprobe gs_usb 2>/dev/null || true; "
                f"for i in $(seq 1 15); do ip link show {iface} >/dev/null 2>&1 && break; sleep 0.2; done; "
                f"ip link set {iface} down 2>/dev/null || true; "
                f"ip link set {iface} type can bitrate {bitrate} restart-ms 100 && "
                f"ip link set {iface} txqueuelen 1000 up"
            )
            cfg = run_cmd(wsl_cmd(distro, setup_sh, root=True), timeout=10.0)
            if cfg.returncode != 0:
                self.log(f"{iface} 配置失败: {(cfg.stderr or cfg.stdout).strip()}")
                self._probe_usb_and_can()
                return

            self.log(f"{iface} 已就绪 ({bitrate // 1000} kbps)，自动开启 candump 实时监听。")
            self._probe_usb_and_can()
            self.ui_queue.put(("start_candump", None))

        self._run_async(task)

    def do_apply_bitrate(self) -> None:
        self._save_config()

        def task() -> None:
            distro = self.distro_var.get().strip()
            iface = self.iface_var.get().strip() or DEFAULT_IFACE
            bitrate = self._selected_bitrate()
            sh = (
                f"ip link set {iface} down && "
                f"ip link set {iface} type can bitrate {bitrate} restart-ms 100 && "
                f"ip link set {iface} txqueuelen 1000 up"
            )
            res = run_cmd(wsl_cmd(distro, sh, root=True), timeout=8.0)
            if res.returncode == 0:
                self.log(f"已切换 {iface} 波特率为 {bitrate // 1000} kbps")
            else:
                self.log(f"切换波特率失败: {(res.stderr or res.stdout).strip()}")
            self._probe_usb_and_can()

        self._run_async(task)

    def do_disconnect(self) -> None:
        self.stop_cangen()
        self.stop_candump()

        def task() -> None:
            dev = self._find_target_usb()
            if dev and "Attached" in dev["state"]:
                run_cmd([find_usbipd(), "detach", "--busid", dev["busid"]], timeout=8.0)
                self.log(f"已从 WSL2 卸载 USB-CAN ({dev['busid']})")
            self._probe_usb_and_can()

        self._run_async(task)

    def _apply_and_send_preset(self, cid: str, data: str, ext: bool, rtr: bool) -> None:
        self.tx_id_var.set(cid)
        self.tx_data_var.set(data)
        self.tx_ext_var.set(ext)
        self.tx_rtr_var.set(rtr)
        self.do_send_once()

    def do_send_once(self) -> None:
        try:
            frame_arg = format_cansend_arg(self.tx_id_var.get(), self.tx_data_var.get(), self.tx_ext_var.get(), self.tx_rtr_var.get())
        except ValueError as exc:
            messagebox.showwarning("报文格式错误", str(exc), parent=self.root)
            return

        self._save_config()
        distro, iface = self.distro_var.get().strip(), self.iface_var.get().strip() or DEFAULT_IFACE

        def worker() -> None:
            res = run_cmd(wsl_cmd(distro, f"cansend {iface} '{frame_arg}'"), timeout=4.0)
            if res.returncode != 0:
                self.log(f"发送失败 ({frame_arg}): {(res.stderr or res.stdout).strip()}")

        threading.Thread(target=worker, daemon=True).start()

    def start_cangen(self) -> None:
        if self.cangen_proc and self.cangen_proc.poll() is None:
            return
        distro, iface = self.distro_var.get().strip(), self.iface_var.get().strip() or DEFAULT_IFACE
        gap, cnt = self.gen_gap_var.get().strip() or "10", self.gen_cnt_var.get().strip() or "-1"
        cid = self.tx_id_var.get().strip().upper().removeprefix("0X") or "123"
        mode = self.gen_mode_var.get()

        cmd_parts = ["exec", "cangen", iface, "-g", gap, "-I", cid, "-L", "8"]
        if self.tx_ext_var.get() or int(cid, 16) > 0x7FF:
            cmd_parts.append("-e")
        if mode == "递增":
            cmd_parts.extend(["-D", "i"])
        elif mode == "当前Data":
            clean_hex = re.sub(r"[\s,:-]+", "", self.tx_data_var.get()).upper() or "1122334455667788"
            cmd_parts.extend(["-D", clean_hex])
        if cnt != "-1" and cnt.isdigit() and int(cnt) > 0:
            cmd_parts.extend(["-n", cnt])

        try:
            self.cangen_proc = subprocess.Popen(
                wsl_cmd(distro, " ".join(cmd_parts)),
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, creationflags=NO_WINDOW,
            )
            self.gen_status_var.set(f"运行中 ({gap}ms)")
            self.gen_badge.configure(fg=C_OK)
            self.log(f"已启动 cangen: {' '.join(cmd_parts[1:])}")
        except Exception as exc:
            self.log(f"启动 cangen 失败: {exc}")

    def stop_cangen(self) -> None:
        if self.cangen_proc and self.cangen_proc.poll() is None:
            try:
                self.cangen_proc.terminate()
            except Exception:
                pass
        self.cangen_proc = None
        distro = self.distro_var.get().strip()
        threading.Thread(target=lambda: run_cmd(wsl_cmd(distro, "pkill -f 'cangen ' 2>/dev/null || true"), timeout=3.0), daemon=True).start()
        self.gen_status_var.set("待命")
        self.gen_badge.configure(fg=C_DIM)

    def start_candump(self) -> None:
        self.stop_candump()
        distro, iface = self.distro_var.get().strip(), self.iface_var.get().strip() or DEFAULT_IFACE
        try:
            self.candump_proc = subprocess.Popen(
                wsl_cmd(distro, f"exec stdbuf -oL candump -ta -x {iface}"),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=NO_WINDOW,
            )
        except Exception as exc:
            self.log(f"启动 candump 失败: {exc}")
            return

        proc = self.candump_proc

        def reader() -> None:
            assert proc.stdout is not None
            for raw in iter(proc.stdout.readline, b""):
                if proc is not self.candump_proc:
                    break
                m = CANDUMP_RE.match(raw.decode("utf-8", errors="replace").strip())
                if not m:
                    continue
                ts = float(m.group("ts") or time.time())
                cid, dlc, rest = m.group("id").upper(), int(m.group("dlc")), m.group("rest").strip()
                is_rtr = rest.upper().startswith("RTR")
                ftype = "RTR" if is_rtr else ("EXT" if len(cid) > 3 else "STD")
                hex_bytes = [] if is_rtr else [t.upper() for t in rest.split() if len(t) == 2 and all(c in "0123456789ABCDEFabcdef" for c in t)][:dlc]
                data_str = "REMOTE REQUEST" if is_rtr else " ".join(hex_bytes)
                ascii_str = "" if is_rtr else "".join(chr(int(b, 16)) if 32 <= int(b, 16) <= 126 else "." for b in hex_bytes)
                self.ui_queue.put(("rx", (ts, f"0x{cid}", ftype, dlc, data_str, ascii_str)))

        threading.Thread(target=reader, daemon=True).start()

    def stop_candump(self) -> None:
        if self.candump_proc and self.candump_proc.poll() is None:
            try:
                self.candump_proc.terminate()
            except Exception:
                pass
        self.candump_proc = None

    def toggle_rx_pause(self) -> None:
        self.rx_paused = not self.rx_paused
        self.pause_btn.configure(text="恢复滚动" if self.rx_paused else "暂停滚动", fg=C_WARN if self.rx_paused else C_TEXT)

    def clear_rx_table(self) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        self.rx_seq = 0
        self.last_rx_ts = None
        self.rx_count_var.set("0 帧")

    def _periodic_status_tick(self) -> None:
        if self.cangen_proc and self.cangen_proc.poll() is not None:
            self.cangen_proc = None
            self.gen_status_var.set("已完成")
            self.gen_badge.configure(fg=C_DIM)

        if not self.busy:
            distro, iface = self.distro_var.get().strip(), self.iface_var.get().strip() or DEFAULT_IFACE

            def poll() -> None:
                res = run_cmd(wsl_cmd(distro, f"ip -j -d -s link show {iface}"), timeout=3.0)
                if res.returncode == 0 and res.stdout.strip():
                    self._parse_ip_link_json(res.stdout, iface)

            threading.Thread(target=poll, daemon=True).start()
        self.root.after(2000, self._periodic_status_tick)

    def _drain_ui_queue(self) -> None:
        rx_batch = []
        try:
            while True:
                kind, payload = self.ui_queue.get_nowait()
                if kind == "rx":
                    rx_batch.append(payload)
                elif kind == "log":
                    self.log_text.configure(state="normal")
                    self.log_text.insert("end", payload)
                    self.log_text.see("end")
                    self.log_text.configure(state="disabled")
                elif kind == "usb_badge":
                    self.usb_badge_var.set(payload[0])
                    self.usb_lbl.configure(fg=payload[1])
                elif kind == "can_badge":
                    self.can_badge_var.set(payload[0])
                    self.can_lbl.configure(fg=payload[1])
                elif kind == "stats":
                    self.stats_var.set(payload)
                elif kind == "distros":
                    self.distro_cb.configure(values=payload)
                    if self.distro_var.get() not in payload and payload:
                        self.distro_var.set(payload[0])
                elif kind == "start_candump":
                    self.start_candump()
                elif kind == "busy_done":
                    self.busy = False
                    self.connect_btn.configure(state="normal")
        except queue.Empty:
            pass

        if rx_batch and not self.rx_paused:
            flt = self.filter_var.get().strip().upper()
            for ts, cid, ftype, dlc, data_str, ascii_str in rx_batch[-120:]:
                if flt and flt not in cid and flt not in data_str:
                    continue
                self.rx_seq += 1
                dt_ms = (ts - self.last_rx_ts) * 1000.0 if self.last_rx_ts else 0.0
                self.last_rx_ts = ts
                t_str = datetime.fromtimestamp(ts).strftime("%H:%M:%S.%f")[:-3]
                self.tree.insert("", "end", values=(self.rx_seq, t_str, f"{dt_ms:.1f}", cid, ftype, dlc, data_str, ascii_str))

            children = self.tree.get_children()
            if len(children) > 400:
                for old in children[: len(children) - 400]:
                    self.tree.delete(old)
            if children:
                self.tree.see(children[-1])
            self.rx_count_var.set(f"{self.rx_seq} 帧")

        self.root.after(40, self._drain_ui_queue)

    def on_close(self) -> None:
        self._save_config()
        self.stop_cangen()
        self.stop_candump()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    ToolApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
