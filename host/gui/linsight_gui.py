#!/usr/bin/env python3
"""LinSight bench GUI: live CAN trace + PCAN traffic generator.

The device has exactly one TCP peer (linsightd).  This GUI therefore never
connects to the device directly; it uses the daemon's loopback control port,
which speaks the same newline protocol as linsight-cli:

    linsightd --port 8888 --ctl-port 8889 --log-dir ~/can-logs
    py -3.10 host/gui/linsight_gui.py

Run it on Windows so python-can can drive the PCAN-USB adapter; the daemon may
live in WSL.  WSL2 mirrored networking makes 127.0.0.1:8889 reach the daemon.
"""

import json
import os
import queue
import random
import re
import socket
import subprocess
import threading
import time
import tkinter as tk
from collections import Counter, deque
from tkinter import ttk

try:
    import can
except Exception:  # GUI still works for viewing without python-can
    can = None

# ---------------------------------------------------------------- palette

BG = "#0b0b0e"
BAR = "#161618"
TILE = "#1c1c1e"
TILE2 = "#242428"
ACTIVE = "#2c2c2e"
TEXT = "#f8fafc"
DIM = "#8e8e93"
FAINT = "#5a5a60"
BLUE = "#0a84ff"
GREEN = "#30d158"
ORANGE = "#ff9f0a"
RED = "#ff453a"
PURPLE = "#bf5af2"

UI = ("Microsoft YaHei UI", 10)
UI_SM = ("Microsoft YaHei UI", 9)
TITLE = ("Microsoft YaHei UI", 15, "bold")
MONO = ("Consolas", 10)
MONO_SM = ("Consolas", 9)

MAX_ROWS = 500
PUMP_MS = 100
BAUD = {"125k": 125000, "250k": 250000, "500k": 500000, "1M": 1000000}
MIXED_IDS = (0x123, 0x7E8, 0x18DAF110, 0x240, 0x7DF, 0x200, 0x7E0)
PATTERNS = {
    "混合 ID": "mixed",
    "单 ID": "single",
    "递增 ID": "ramp",
    "随机 ID": "random",
}
PATTERN_NAMES = {v: k for k, v in PATTERNS.items()}

CANDUMP_RE = re.compile(r"^\((\d+\.\d+)\) can0 (.*?) \[(\d+)\](.*)$")
CANDUMP_RTR_RE = re.compile(r"^\((\d+\.\d+)\) can0 (.*?) remote request \((\d+)\)$")

# PCAN hardware status bits (PCAN-Basic GetStatus); 0x0 = OK.
PCAN_STATUS_BITS = (
    (0x10, "BUSOFF"),
    (0x1, "XMTFULL"),   # TX queue full: pacing too fast for the driver
    (0x2, "OVERRUN"),   # RX overrun on the PCAN side
    (0x8, "BUSWARNING"),
    (0x40000, "BUSPASSIVE"),
)
PCAN_STATUS_BUSOFF = 0x10
# Give up after this many consecutive send errors and report to the user;
# before that, bus-off gets a reset + backoff and transient errors retry.
TX_MAX_CONSEC_ERRORS = 200
TX_BUSOFF_BACKOFF_S = 0.5


def stamp(ts):
    whole = int(ts)
    return time.strftime("%H:%M:%S", time.localtime(whole)) + \
        f".{int((ts - whole) * 1000):03d}"


def parse_live_line(line):
    """Return (ts, id, extended, dlc, data_text) or None."""
    m = CANDUMP_RE.match(line)
    if m:
        raw_id = m.group(2)
        return (
            float(m.group(1)),
            int(raw_id.strip(), 16),
            not raw_id.startswith("   "),
            int(m.group(3)),
            m.group(4).strip(),
        )
    m = CANDUMP_RTR_RE.match(line)
    if m:
        raw_id = m.group(2)
        return (
            float(m.group(1)),
            int(raw_id.strip(), 16),
            not raw_id.startswith("   "),
            int(m.group(3)),
            "远程帧",
        )
    return None


def make_button(parent, text, cmd, fg=TEXT, bg=ACTIVE, width=None):
    b = tk.Button(
        parent, text=text, command=cmd, bg=bg, fg=fg,
        activebackground=TILE2, activeforeground=TEXT,
        relief="flat", bd=0, padx=10, pady=4, font=UI_SM,
        cursor="hand2", highlightthickness=0,
    )
    if width:
        b.config(width=width)
    return b


class App:
    def __init__(self, root, host="127.0.0.1", port=8889):
        self.root = root
        self.q = queue.Queue()
        self._stop = threading.Event()
        self._tx_stop = threading.Event()
        self._frames = deque(maxlen=4000)
        self._frames_lock = threading.Lock()
        self._tx_thread = None
        self._tx_running = False

        self._host = host
        self._port = port
        self._daemon_online = False
        self._device_online = False
        self._live_online = False
        self._last_tx = {}
        self._id_counts = Counter()
        self._view_skipped = 0
        self._row_parity = 0
        self._last_id_refresh = 0.0

        self.host_var = tk.StringVar(value=self._host)
        self.port_var = tk.StringVar(value=str(self._port))
        self.pattern_var = tk.StringVar(value="混合 ID")
        self.iface_var = tk.StringVar(value="pcan")
        self.channel_var = tk.StringVar(value="PCAN_USBBUS1")
        self.tx_baud_var = tk.StringVar(value="500k")
        self.rate_var = tk.StringVar(value="500")
        self.duration_var = tk.StringVar(value="0")
        self.tx_id_var = tk.StringVar(value="0x123")
        self.tx_ext_var = tk.BooleanVar(value=False)
        self.freeze_var = tk.BooleanVar(value=False)

        self._build_style()
        self._build_ui()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(PUMP_MS, self._pump)

        threading.Thread(target=self._stats_loop, daemon=True).start()
        threading.Thread(target=self._live_loop, daemon=True).start()
        self._log("就绪。守护进程连接后自动开始接收。", "dim")

    # ------------------------------------------------------------ styling

    def _build_style(self):
        s = ttk.Style(self.root)
        s.theme_use("clam")
        s.configure(
            "Trace.Treeview",
            background=TILE, fieldbackground=TILE, foreground=TEXT,
            font=MONO, rowheight=22, borderwidth=0,
        )
        s.map(
            "Trace.Treeview",
            background=[("selected", BLUE)],
            foreground=[("selected", "#ffffff")],
        )
        s.configure(
            "Trace.Treeview.Heading",
            background=ACTIVE, foreground=DIM, font=UI_SM,
            relief="flat", borderwidth=0,
        )
        s.map("Trace.Treeview.Heading", background=[("active", ACTIVE)])
        s.configure(
            "Dark.Vertical.TScrollbar",
            background=TILE2, troughcolor=BG, arrowcolor=DIM,
            bordercolor=BG, darkcolor=TILE2, lightcolor=TILE2,
        )
        s.configure(
            "TCombobox",
            fieldbackground=TILE2, background=TILE2, foreground=TEXT,
            arrowcolor=DIM, bordercolor=BG, lightcolor=TILE2,
            darkcolor=TILE2, padding=3,
        )
        s.map(
            "TCombobox",
            fieldbackground=[("readonly", TILE2)],
            foreground=[("readonly", TEXT)],
            selectbackground=[("readonly", TILE2)],
            selectforeground=[("readonly", TEXT)],
        )
        self.root.option_add("*TCombobox*Listbox.background", TILE)
        self.root.option_add("*TCombobox*Listbox.foreground", TEXT)
        self.root.option_add("*TCombobox*Listbox.selectBackground", ACTIVE)
        self.root.option_add("*TCombobox*Listbox.selectForeground", TEXT)

    # ---------------------------------------------------------------- ui

    def _build_ui(self):
        self.root.title("LinSight 测试台")
        self.root.geometry("1280x820")
        self.root.minsize(1120, 800)
        self.root.configure(bg=BG)

        header = tk.Frame(self.root, bg=BAR, height=52)
        header.pack(side="top", fill="x")
        header.pack_propagate(False)
        tk.Label(header, text="LinSight", fg=TEXT, bg=BAR,
                 font=TITLE).pack(side="left", padx=(16, 8))
        tk.Label(header, text="无线 CAN 测试台", fg=DIM, bg=BAR,
                 font=UI_SM).pack(side="left", pady=(4, 0))

        self.live_lbl = tk.Label(header, text="实时流 ● 等待", fg=FAINT, bg=BAR, font=UI_SM)
        self.live_lbl.pack(side="right", padx=(6, 16))
        self.device_lbl = tk.Label(header, text="设备 ● 未连接", fg=FAINT, bg=BAR, font=UI_SM)
        self.device_lbl.pack(side="right", padx=6)
        self.daemon_lbl = tk.Label(header, text="守护进程 ● 离线", fg=RED, bg=BAR, font=UI_SM)
        self.daemon_lbl.pack(side="right", padx=6)

        statusbar = tk.Frame(self.root, bg=BAR, height=24)
        statusbar.pack(side="bottom", fill="x")
        statusbar.pack_propagate(False)
        self.status_var = tk.StringVar(value="就绪")
        tk.Label(statusbar, textvariable=self.status_var, fg=DIM, bg=BAR,
                 font=UI_SM).pack(side="left", padx=12)
        self.conn_lbl = tk.Label(statusbar, text="", fg=FAINT, bg=BAR, font=MONO_SM)
        self.conn_lbl.pack(side="right", padx=12)

        main = tk.Frame(self.root, bg=BG)
        main.pack(fill="both", expand=True, padx=10, pady=8)
        main.columnconfigure(0, weight=3)
        main.columnconfigure(1, weight=2)
        main.rowconfigure(0, weight=1)

        left = tk.Frame(main, bg=BG)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 8))
        right = tk.Frame(main, bg=BG)
        right.grid(row=0, column=1, sticky="nsew")

        self._build_trace(left)
        self._build_stats(right)
        self._build_connection(right)
        self._build_traffic(right)
        self._build_ids(right)
        self._build_log(right)

    def _build_trace(self, parent):
        bar = tk.Frame(parent, bg=BG)
        bar.pack(fill="x")
        make_button(bar, "设备暂停", lambda: self._control("pause")).pack(side="left")
        make_button(bar, "设备恢复", lambda: self._control("resume")).pack(side="left", padx=6)
        make_button(bar, "设备清零", lambda: self._control("clear")).pack(side="left")
        make_button(bar, "清空视图", self._clear_view).pack(side="left", padx=6)
        tk.Checkbutton(
            bar, text="冻结视图", variable=self.freeze_var,
            bg=BG, fg=TEXT, selectcolor=TILE, activebackground=BG,
            activeforeground=TEXT, font=UI_SM, highlightthickness=0,
        ).pack(side="left", padx=(8, 0))
        self.count_lbl = tk.Label(bar, text="显示 0", fg=DIM, bg=BG, font=UI_SM)
        self.count_lbl.pack(side="right")

        frame = tk.Frame(parent, bg=TILE)
        frame.pack(fill="both", expand=True, pady=(6, 0))
        cols = ("time", "id", "kind", "dlc", "data")
        self.tree = ttk.Treeview(
            frame, columns=cols, show="headings", style="Trace.Treeview",
        )
        for col, text, width, anchor, stretch in (
            ("time", "时间", 110, "center", False),
            ("id", "ID", 110, "center", False),
            ("kind", "类型", 54, "center", False),
            ("dlc", "DLC", 46, "center", False),
            ("data", "数据", 280, "w", True),
        ):
            self.tree.heading(col, text=text)
            self.tree.column(col, width=width, anchor=anchor, stretch=stretch)
        self.tree.tag_configure("even", background=TILE)
        self.tree.tag_configure("odd", background="#111115")
        sb = ttk.Scrollbar(
            frame, orient="vertical", command=self.tree.yview,
            style="Dark.Vertical.TScrollbar",
        )
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

    def _build_stats(self, parent):
        card = tk.Frame(parent, bg=TILE, padx=10, pady=6)
        card.pack(fill="x", pady=(0, 6))
        for c in range(3):
            card.columnconfigure(c, weight=1)
        tk.Label(card, text="设备统计", fg=DIM, bg=TILE, font=UI_SM,
                 anchor="w").grid(row=0, column=0, columnspan=3, sticky="w")
        self.v_fps = self._tile(card, 1, 0, "FPS")
        self.v_total = self._tile(card, 1, 1, "总帧数")
        self.v_state = self._tile(card, 1, 2, "状态")
        self.v_ring_drop = self._tile(card, 2, 0, "环形丢失")
        self.v_missed = self._tile(card, 2, 1, "FIFO 丢失")
        self.v_bus_err = self._tile(card, 2, 2, "总线错误")
        self.v_ring = self._tile(card, 3, 0, "缓冲")
        self.v_mode = self._tile(card, 3, 1, "模式")
        self.v_host = self._tile(card, 3, 2, "主机记录")
        self.stats_detail = tk.Label(
            card, text="", fg=FAINT, bg=TILE, font=MONO_SM, anchor="w",
            justify="left", wraplength=390,
        )
        self.stats_detail.grid(row=4, column=0, columnspan=3, sticky="w",
                               pady=(2, 0))

    def _tile(self, parent, row, col, title):
        frame = tk.Frame(parent, bg=TILE)
        frame.grid(row=row, column=col, sticky="w", pady=(2, 0))
        tk.Label(frame, text=title, fg=DIM, bg=TILE, font=UI_SM,
                 anchor="w").pack(anchor="w")
        value = tk.Label(frame, text="—", fg=TEXT, bg=TILE,
                         font=("Consolas", 12, "bold"), anchor="w")
        value.pack(anchor="w")
        return value

    def _build_connection(self, parent):
        card = tk.Frame(parent, bg=TILE, padx=10, pady=8)
        card.pack(fill="x", pady=(0, 6))
        row = tk.Frame(card, bg=TILE)
        row.pack(fill="x")
        tk.Label(row, text="守护进程", fg=DIM, bg=TILE, font=UI_SM).pack(side="left")
        tk.Entry(row, textvariable=self.host_var, width=11, bg=TILE2, fg=TEXT,
                 insertbackground=TEXT, relief="flat", font=MONO_SM).pack(
            side="left", padx=(8, 2))
        tk.Label(row, text=":", fg=DIM, bg=TILE, font=UI_SM).pack(side="left")
        tk.Entry(row, textvariable=self.port_var, width=5, bg=TILE2, fg=TEXT,
                 insertbackground=TEXT, relief="flat", font=MONO_SM).pack(
            side="left", padx=(2, 6))
        make_button(row, "重连", self._reconnect).pack(side="left")
        make_button(row, "启动 WSL 守护进程", self._start_daemon,
                    bg=TILE2).pack(side="left", padx=(6, 0))
        self.conn_detail = tk.Label(
            card, text="需要 linsightd --port 8888 --ctl-port 8889",
            fg=FAINT, bg=TILE, font=MONO_SM, anchor="w",
            justify="left", wraplength=390,
        )
        self.conn_detail.pack(fill="x", pady=(4, 0))

    def _build_traffic(self, parent):
        card = tk.Frame(parent, bg=TILE, padx=10, pady=6)
        card.pack(fill="x", pady=(0, 6))
        head = tk.Frame(card, bg=TILE)
        head.pack(fill="x")
        tk.Label(head, text="PCAN 流量发生器", fg=TEXT, bg=TILE,
                 font=UI).pack(side="left")
        self.tx_badge = tk.Label(head, text="空闲", fg=FAINT, bg=TILE, font=UI_SM)
        self.tx_badge.pack(side="right")

        row1 = tk.Frame(card, bg=TILE)
        row1.pack(fill="x", pady=(4, 1))
        self._label(row1, "接口")
        ttk.Combobox(
            row1, textvariable=self.iface_var, width=8, state="readonly",
            values=("pcan", "socketcan", "virtual"),
        ).pack(side="left", padx=(4, 8))
        self._label(row1, "通道")
        tk.Entry(row1, textvariable=self.channel_var, width=15, bg=TILE2,
                 fg=TEXT, insertbackground=TEXT, relief="flat",
                 font=MONO_SM).pack(side="left", padx=(4, 8))
        self._label(row1, "速率")
        ttk.Combobox(
            row1, textvariable=self.tx_baud_var, width=6, state="readonly",
            values=("125k", "250k", "500k", "1M"),
        ).pack(side="left", padx=4)
        self.iface_var.trace_add("write", self._iface_changed)

        row2 = tk.Frame(card, bg=TILE)
        row2.pack(fill="x", pady=1)
        self._label(row2, "模式")
        self.pattern_box = ttk.Combobox(
            row2, textvariable=self.pattern_var, width=8, state="readonly",
            values=tuple(PATTERNS),
        )
        self.pattern_box.pack(side="left", padx=(4, 8))
        self.pattern_box.bind("<<ComboboxSelected>>", self._pattern_changed)
        self._label(row2, "ID")
        self.id_entry = tk.Entry(
            row2, textvariable=self.tx_id_var, width=9, bg=TILE2, fg=TEXT,
            insertbackground=TEXT, relief="flat", font=MONO_SM,
            disabledbackground=TILE, disabledforeground=FAINT,
        )
        self.id_entry.pack(side="left", padx=(4, 8))
        self.ext_check = tk.Checkbutton(
            row2, text="扩展帧", variable=self.tx_ext_var, bg=TILE, fg=DIM,
            selectcolor=TILE2, activebackground=TILE, activeforeground=TEXT,
            font=UI_SM, highlightthickness=0,
        )
        self.ext_check.pack(side="left")

        row3 = tk.Frame(card, bg=TILE)
        row3.pack(fill="x", pady=1)
        self._label(row3, "帧率")
        tk.Entry(row3, textvariable=self.rate_var, width=6, bg=TILE2, fg=TEXT,
                 insertbackground=TEXT, relief="flat", font=MONO_SM).pack(
            side="left", padx=(4, 2))
        self._label(row3, "fps")
        self._label(row3, "时长", padx=(12, 0))
        tk.Entry(row3, textvariable=self.duration_var, width=5, bg=TILE2,
                 fg=TEXT, insertbackground=TEXT, relief="flat",
                 font=MONO_SM).pack(side="left", padx=(4, 2))
        self._label(row3, "s (0=持续)")

        row4 = tk.Frame(card, bg=TILE)
        row4.pack(fill="x", pady=(4, 1))
        self.start_btn = make_button(row4, "开始发送", self._tx_start,
                                     fg="#000000", bg=GREEN)
        self.start_btn.pack(side="left")
        self.stop_btn = make_button(row4, "停止", self._tx_stop_click,
                                    bg=TILE2)
        self.stop_btn.pack(side="left", padx=6)
        self.stop_btn.config(state="disabled")
        make_button(row4, "发送 20 帧", self._tx_burst,
                    bg=TILE2).pack(side="left")

        self.tx_detail = tk.Label(
            card, text="python-can 未安装" if can is None else "已发送 0 · 错误 0",
            fg=DIM, bg=TILE, font=MONO_SM, anchor="w",
            justify="left", wraplength=390,
        )
        self.tx_detail.pack(fill="x", pady=(2, 0))
        self._pattern_changed()

    def _label(self, parent, text, padx=(0, 0)):
        tk.Label(parent, text=text, fg=DIM, bg=TILE, font=UI_SM).pack(
            side="left", padx=padx)

    def _build_ids(self, parent):
        card = tk.Frame(parent, bg=TILE, padx=10, pady=6)
        card.pack(fill="x", pady=(0, 6))
        tk.Label(card, text="ID 统计（本地视图）", fg=DIM, bg=TILE,
                 font=UI_SM, anchor="w").pack(fill="x")
        self.id_tree = ttk.Treeview(
            card, columns=("id", "count"), show="headings",
            height=3, style="Trace.Treeview",
        )
        self.id_tree.heading("id", text="ID")
        self.id_tree.heading("count", text="帧数")
        self.id_tree.column("id", width=130, anchor="center", stretch=False)
        self.id_tree.column("count", width=100, anchor="e", stretch=True)
        self.id_tree.pack(fill="x", pady=(3, 0))

    def _build_log(self, parent):
        card = tk.Frame(parent, bg=TILE, padx=10, pady=6)
        card.pack(fill="both", expand=True)
        head = tk.Frame(card, bg=TILE)
        head.pack(fill="x")
        tk.Label(head, text="日志", fg=DIM, bg=TILE, font=UI_SM).pack(side="left")
        make_button(head, "清空", self._log_clear, bg=TILE2).pack(side="right")
        self.log_text = tk.Text(
            card, height=4, width=40, bg="#0f0f12", fg=DIM,
            insertbackground=TEXT, relief="flat", bd=0, font=MONO_SM,
            wrap="word", state="disabled", padx=6, pady=4,
        )
        self.log_text.pack(fill="both", expand=True, pady=(3, 0))
        self.log_text.tag_configure("ok", foreground=GREEN)
        self.log_text.tag_configure("warn", foreground=ORANGE)
        self.log_text.tag_configure("err", foreground=RED)
        self.log_text.tag_configure("dim", foreground=DIM)

    # ---------------------------------------------------------- daemon IO

    def _daemon_request(self, cmd, timeout=2.0):
        with socket.create_connection((self._host, self._port), timeout=timeout) as s:
            s.settimeout(timeout)
            s.sendall(cmd.encode("ascii") + b"\n")
            data = b""
            while b"\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
        return data.decode(errors="replace").strip()

    def _stats_loop(self):
        online = None
        while not self._stop.is_set():
            try:
                st = json.loads(self._daemon_request("stats"))
                self.q.put(("stats", st))
                if online is not True:
                    self.q.put(("log", "守护进程已连接", "ok"))
                online = True
            except Exception as exc:
                self.q.put(("stats", None))
                if online is not False:
                    self.q.put(("log", f"守护进程未连接: {exc}", "err"))
                    self.q.put(("hint", "确认已运行: linsightd --port 8888 --ctl-port 8889"))
                online = False
            self._stop.wait(1.0 if online else 1.5)

    def _live_loop(self):
        online = False
        while not self._stop.is_set():
            try:
                with socket.create_connection((self._host, self._port), timeout=2) as s:
                    s.settimeout(1.0)
                    s.sendall(b"live\n")
                    if not online:
                        self.q.put(("log", "实时流已订阅", "ok"))
                        online = True
                    self.q.put(("live", True))
                    buf = b""
                    while not self._stop.is_set():
                        try:
                            chunk = s.recv(65536)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            parsed = parse_live_line(line.decode(errors="replace").strip())
                            if parsed is not None:
                                with self._frames_lock:
                                    self._frames.append(parsed)
                        if len(buf) > 65536:
                            buf = b""
            except Exception:
                pass
            if online:
                self.q.put(("log", "实时流断开，重试中", "warn"))
                online = False
            self.q.put(("live", False))
            self._stop.wait(1.0)

    def _control(self, action):
        def worker():
            try:
                reply = self._daemon_request(action)
                ok = '"code":0' in reply
                self.q.put(("log", f"{action}: {reply or '无响应'}", "ok" if ok else "warn"))
                if ok and action == "clear":
                    self.q.put(("clear_view", None))
            except Exception as exc:
                self.q.put(("log", f"{action} 失败: {exc}", "err"))
        threading.Thread(target=worker, daemon=True).start()

    def _reconnect(self):
        try:
            self._host = self.host_var.get().strip() or "127.0.0.1"
            self._port = int(self.port_var.get())
        except ValueError:
            self._log("端口必须是数字", "err")
            return
        self.conn_lbl.config(text=f"{self._host}:{self._port}")
        self._log(f"重连目标 {self._host}:{self._port}", "dim")

    def _start_daemon(self):
        build = os.environ.get("LINSIGHT_DAEMON_DIR",
                               "$HOME/linsight-host-build/linsightd")
        logs = os.environ.get("LINSIGHT_LOG_DIR", "$HOME/can-logs")
        cmd = (
            'if pgrep -x linsightd >/dev/null; then echo already-running; '
            f'else nohup "{build}" --port 8888 '
            f'--ctl-port 8889 --log-dir "{logs}" '
            '>/tmp/linsightd.log 2>&1 & fi'
        )
        try:
            kwargs = {"creationflags": 0x08000000} if os.name == "nt" else {}
            subprocess.Popen(["wsl.exe", "-e", "bash", "-lc", cmd], **kwargs)
            self._log("正在启动 WSL 守护进程…", "dim")
        except Exception as exc:
            self._log(f"无法启动 wsl.exe: {exc}", "err")

    # ---------------------------------------------------------- traffic

    def _iface_changed(self, *_):
        iface = self.iface_var.get()
        channel = self.channel_var.get()
        if iface == "virtual" and channel == "PCAN_USBBUS1":
            self.channel_var.set("vcan0")
        elif iface == "socketcan" and channel == "PCAN_USBBUS1":
            self.channel_var.set("can0")
        elif iface == "pcan" and channel in ("vcan0", "can0"):
            self.channel_var.set("PCAN_USBBUS1")

    def _pattern_changed(self, *_):
        pattern = PATTERNS.get(self.pattern_var.get(), "mixed")
        manual = pattern in ("single", "ramp")
        self.id_entry.config(state="normal" if manual else "disabled")
        self.ext_check.config(state="normal" if manual else "disabled")

    def _collect_tx(self):
        if can is None:
            raise ValueError("python-can 未安装；请用装有 python-can 的 Python 运行")
        rate = int(float(self.rate_var.get()))
        duration = float(self.duration_var.get())
        if rate < 1 or rate > 20000:
            raise ValueError("帧率范围 1..20000")
        if duration < 0:
            raise ValueError("时长不能为负")
        try:
            ident = int(self.tx_id_var.get(), 0)
        except ValueError:
            raise ValueError("ID 格式错误，例如 0x123")
        if not 0 <= ident <= 0x1FFFFFFF:
            raise ValueError("ID 超出 29 位范围")
        return {
            "iface": self.iface_var.get(),
            "channel": self.channel_var.get().strip(),
            "bitrate": BAUD[self.tx_baud_var.get()],
            "pattern": PATTERNS.get(self.pattern_var.get(), "mixed"),
            "id": ident,
            "ext": bool(self.tx_ext_var.get()) or ident > 0x7FF,
            "rate": rate,
            "duration": duration,
        }

    def _make_msg(self, cfg, i):
        pattern = cfg["pattern"]
        if pattern == "mixed":
            ident = MIXED_IDS[i % len(MIXED_IDS)]
            ext = ident > 0x7FF
        elif pattern == "single":
            ident, ext = cfg["id"], cfg["ext"]
        elif pattern == "ramp":
            ident = cfg["id"] + (i % 16)
            ext = cfg["ext"] or ident > 0x7FF
        else:
            ident, ext = random.randint(0, 0x7FF), False
        data = bytes((
            (i >> 8) & 0xFF, i & 0xFF, 0x11, 0x22,
            0x33, 0x44, 0x55, (i * 7) & 0xFF,
        ))
        return can.Message(arbitration_id=ident, is_extended_id=ext,
                           data=data, is_fd=False)

    def _open_bus(self, cfg):
        kwargs = {"interface": cfg["iface"], "channel": cfg["channel"]}
        if cfg["iface"] != "virtual":
            kwargs["bitrate"] = cfg["bitrate"]
        if cfg["iface"] == "pcan":
            # PCAN_BUSOFF_AUTORESET: the adapter re-initializes itself after
            # a bus-off instead of wedging every later Write() until unplug.
            kwargs["auto_reset"] = True
        return can.interface.Bus(**kwargs)

    def _bus_state(self, bus):
        try:
            status = int(bus.status())
        except Exception:
            status = None
        if status is None:
            return "?"
        if status == 0:
            return "OK"
        names = [name for bit, name in PCAN_STATUS_BITS if status & bit]
        return f"0x{status:X}" + (f" {'|'.join(names)}" if names else "")

    def _bus_reset(self, bus):
        """Best-effort controller reset (PCAN: leaves bus-off cleanly)."""
        try:
            return bool(bus.reset())
        except Exception:
            return False

    def _tx_start(self):
        if self._tx_running:
            return
        try:
            cfg = self._collect_tx()
        except ValueError as exc:
            self._log(str(exc), "err")
            return
        self._tx_stop.clear()
        self._tx_running = True
        self.start_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        self.tx_badge.config(text="启动中", fg=ORANGE)
        self._tx_thread = threading.Thread(
            target=self._tx_worker, args=(cfg,), daemon=True,
        )
        self._tx_thread.start()

    def _tx_stop_click(self):
        self._tx_stop.set()
        self.tx_badge.config(text="停止中", fg=ORANGE)

    def _tx_worker(self, cfg):
        bus = None
        sent = 0
        errors = 0
        consec_errors = 0
        busoff_resets = 0
        try:
            bus = self._open_bus(cfg)
            rate = cfg["rate"]
            tick = 0.02 if rate >= 50 else 1.0 / rate
            per_tick = max(1, int(round(rate * tick)))
            start = time.perf_counter()
            next_t = start
            next_report = start
            last_sent, last_t = 0, start
            if cfg["iface"] == "pcan":
                self.q.put(("log", f"PCAN 已打开: {cfg['channel']} "
                            f"{cfg['bitrate'] // 1000}k（bus-off 自动复位已启用）", "ok"))
            else:
                self.q.put(("log", f"总线已打开: {cfg['channel']}", "ok"))
            while not self._tx_stop.is_set():
                for _ in range(per_tick):
                    try:
                        # PCAN's send() ignores `timeout`; errors raise here.
                        bus.send(self._make_msg(cfg, sent + errors))
                        sent += 1
                        consec_errors = 0
                    except can.CanError as exc:
                        errors += 1
                        consec_errors += 1
                        if consec_errors == 1:
                            self.q.put(("log", f"发送错误: {exc}", "warn"))
                        # Bus-off: reset the controller, back off (its recovery
                        # needs ~128*11 recessive bits of silence), then retry.
                        status = self._bus_status(bus)
                        if status is not None and status & PCAN_STATUS_BUSOFF:
                            busoff_resets += 1
                            self.q.put(("log",
                                        f"PCAN bus-off，自动复位 #{busoff_resets}，"
                                        f"退避 {TX_BUSOFF_BACKOFF_S:.1f}s", "warn"))
                            self._bus_reset(bus)
                            self._tx_stop.wait(TX_BUSOFF_BACKOFF_S)
                            if self._tx_stop.is_set():
                                break
                        if sent == 0 and errors >= 20:
                            raise RuntimeError(str(exc))
                        if consec_errors >= TX_MAX_CONSEC_ERRORS:
                            raise RuntimeError(
                                f"连续 {consec_errors} 次发送失败（总线无 ACK 或已 bus-off）；"
                                "已停止。请检查对端节点/接线/比特率")
                        break  # skip the rest of this tick's batch
                now = time.perf_counter()
                if now - next_report >= 0.4:
                    fps = (sent - last_sent) / max(now - last_t, 1e-6)
                    self.q.put(("tx", {
                        "running": True, "sent": sent, "errors": errors,
                        "fps": fps, "state": self._bus_state(bus),
                        "started": start,
                    }))
                    last_sent, last_t, next_report = sent, now, now
                if cfg["duration"] > 0 and now - start >= cfg["duration"]:
                    break
                next_t += tick
                delay = next_t - time.perf_counter()
                if delay > 0:
                    self._tx_stop.wait(delay)
                else:
                    next_t = time.perf_counter()
            self.q.put(("tx", {
                "running": False, "sent": sent, "errors": errors,
                "state": self._bus_state(bus), "msg": "已停止",
            }))
        except Exception as exc:
            self.q.put(("tx", {
                "running": False, "sent": sent, "errors": errors,
                "state": "", "msg": f"PCAN 错误: {exc}",
            }))
            self.q.put(("log", f"PCAN 错误: {exc}", "err"))
        finally:
            if bus is not None:
                try:
                    bus.shutdown()
                except Exception:
                    pass
            self.q.put(("tx_done", None))

    def _bus_status(self, bus):
        """Raw PCAN status word, or None when unavailable."""
        try:
            return int(bus.status())
        except Exception:
            return None

    def _tx_burst(self):
        if self._tx_running:
            self._log("正在持续发送，先停止再突发", "warn")
            return
        try:
            cfg = self._collect_tx()
        except ValueError as exc:
            self._log(str(exc), "err")
            return
        self._tx_running = True
        self.start_btn.config(state="disabled")
        self.tx_badge.config(text="突发中", fg=ORANGE)

        def worker():
            bus = None
            try:
                bus = self._open_bus(cfg)
                for i in range(20):
                    bus.send(self._make_msg(cfg, i))
                    time.sleep(0.01)
                time.sleep(0.2)  # let the controller report ACK/error state
                self.q.put(("tx", {"running": False, "sent": 20, "errors": 0,
                                   "state": self._bus_state(bus), "msg": "突发完成"}))
            except Exception as exc:
                self.q.put(("tx", {"running": False, "sent": 0, "errors": 0,
                                   "state": "", "msg": f"PCAN 错误: {exc}"}))
                self.q.put(("log", f"突发失败: {exc}", "err"))
            finally:
                if bus is not None:
                    try:
                        bus.shutdown()
                    except Exception:
                        pass
                self.q.put(("tx_done", None))

        self._tx_thread = threading.Thread(target=worker, daemon=True)
        self._tx_thread.start()

    # ------------------------------------------------------------- pump

    def _pump(self):
        try:
            self._pump_once()
        except Exception as exc:
            self._log(f"界面刷新错误: {exc}", "err")
        self.root.after(PUMP_MS, self._pump)

    def _pump_once(self):
        self._drain_frames()
        while True:
            try:
                item = self.q.get_nowait()
            except queue.Empty:
                break
            ev = item[0]
            payload = item[1] if len(item) > 1 else None
            if ev == "stats":
                self._apply_stats(payload)
            elif ev == "live":
                self._live_online = bool(payload)
                self._set_live()
            elif ev == "tx":
                self._apply_tx(payload)
            elif ev == "tx_done":
                self._tx_running = False
                self.start_btn.config(state="normal")
                self.stop_btn.config(state="disabled")
            elif ev == "log":
                level = item[2] if len(item) > 2 else "dim"
                self._log(payload, level)
            elif ev == "hint":
                self.conn_detail.config(text=payload)
            elif ev == "clear_view":
                self._clear_view()
        now = time.time()
        if now - self._last_id_refresh >= 1.0:
            self._last_id_refresh = now
            self._refresh_ids()

    def _drain_frames(self):
        with self._frames_lock:
            batch = list(self._frames)
            self._frames.clear()
        if not batch:
            return
        for _ts, ident, _ext, _dlc, _data in batch:
            self._id_counts[ident] += 1
        if self.freeze_var.get():
            self._view_skipped += len(batch)
            return
        if len(batch) > 200:
            # Keep the GUI responsive at high frame rates; the daemon and
            # .lsc still have every record.
            self._view_skipped += len(batch) - 200
            batch = batch[-200:]
        for ts, ident, ext, dlc, data in batch:
            tag = "even" if self._row_parity % 2 == 0 else "odd"
            self.tree.insert(
                "", 0,
                values=(stamp(ts), f"0x{ident:08X}", "EXT" if ext else "STD",
                        dlc, data or "—"),
                tags=(tag,),
            )
            self._row_parity += 1
        children = self.tree.get_children()
        if len(children) > MAX_ROWS:
            self.tree.delete(*children[MAX_ROWS:])
        self.count_lbl.config(
            text=f"显示 {min(len(children), MAX_ROWS)} · 本地 {sum(self._id_counts.values()):,}"
                 + (f" · 跳过 {self._view_skipped:,}" if self._view_skipped else "")
        )

    def _apply_stats(self, st):
        if not st:
            self._set_daemon(False)
            self._set_device(False)
            return
        self._set_daemon(True)
        self._set_device(bool(st.get("device")))
        ds = st.get("stats") or {}
        self.v_fps.config(text=str(ds.get("fps", "—")))
        self.v_total.config(text=f"{ds.get('total', 0):,}")
        state = ds.get("state", "—")
        color = GREEN if state == "RUNNING" else (
            RED if state == "BUS_OFF" else ORANGE)
        self.v_state.config(text=state, fg=color)
        self.v_ring_drop.config(text=f"{ds.get('dropped', 0):,}")
        self.v_missed.config(text=f"{ds.get('missed', 0):,}")
        err = int(ds.get("bus_err", 0))
        self.v_bus_err.config(text=f"{err:,}", fg=ORANGE if err else TEXT)
        self.v_ring.config(
            text=f"{ds.get('ring_used', 0):,}/{ds.get('ring_cap', 0):,}")
        mode = ds.get("mode", "—")
        baud = ds.get("bitrate", 0)
        paused = ds.get("paused", 0)
        self.v_mode.config(
            text=f"{mode} · {baud // 1000}k" + (" · 暂停" if paused else ""),
            fg=ORANGE if paused else TEXT,
        )
        self.v_host.config(text=f"{st.get('records', 0):,}")
        self.stats_detail.config(
            text=f"bus_off {ds.get('bus_off', 0)} · err_pass {ds.get('err_pass', 0)}"
                 f" · TEC {ds.get('tec', 0)} · REC {ds.get('rec', 0)}"
                 f" · 主机丢弃 {st.get('dropped', 0):,}"
        )
        log = st.get("log") or "—"
        self.conn_detail.config(text=log)
        self.status_var.set(
            f"设备 {ds.get('total', 0):,} 帧 · 环形 {ds.get('ring_used', 0):,}"
            f" · {state}"
        )
        self.conn_lbl.config(text=f"{self._host}:{self._port}")

    def _set_daemon(self, up):
        if up == self._daemon_online:
            return
        self._daemon_online = up
        self.daemon_lbl.config(
            text="守护进程 ● 在线" if up else "守护进程 ● 离线",
            fg=GREEN if up else RED,
        )

    def _set_device(self, up):
        if up == self._device_online:
            return
        self._device_online = up
        self.device_lbl.config(
            text="设备 ● 已连接" if up else "设备 ● 未连接",
            fg=GREEN if up else FAINT,
        )

    def _set_live(self):
        self.live_lbl.config(
            text="实时流 ● 接收中" if self._live_online else "实时流 ● 等待",
            fg=GREEN if self._live_online else FAINT,
        )

    def _apply_tx(self, tx):
        if tx.get("msg"):
            self.tx_detail.config(
                text=f"{tx['msg']} · 共 {tx.get('sent', 0):,} 帧"
                     f" · 错误 {tx.get('errors', 0)} · PCAN {tx.get('state', '?')}"
            )
            self.tx_badge.config(
                text="空闲" if tx.get("sent", 0) == 0 else "完成",
                fg=FAINT if tx.get("sent", 0) == 0 else GREEN,
            )
            return
        fps = tx.get("fps", 0)
        self.tx_badge.config(text="发送中", fg=GREEN)
        self.tx_detail.config(
            text=f"已发送 {tx.get('sent', 0):,} · {fps:.0f} fps"
                 f" · 错误 {tx.get('errors', 0)} · PCAN {tx.get('state', '?')}"
        )

    def _refresh_ids(self):
        top = self._id_counts.most_common(6)
        children = self.id_tree.get_children()
        self.id_tree.delete(*children)
        for ident, count in top:
            self.id_tree.insert("", "end",
                                values=(f"0x{ident:08X}", f"{count:,}"))

    def _clear_view(self):
        self.tree.delete(*self.tree.get_children())
        self.id_tree.delete(*self.id_tree.get_children())
        self._id_counts.clear()
        self._view_skipped = 0
        self._row_parity = 0
        self.count_lbl.config(text="显示 0")

    def _log(self, text, level="dim"):
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"{time.strftime('%H:%M:%S')}  {text}\n", level)
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 300:
            self.log_text.delete("1.0", "101.0")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _log_clear(self):
        self.log_text.config(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.config(state="disabled")

    def _on_close(self):
        self._stop.set()
        self._tx_stop.set()
        if self._tx_thread is not None and self._tx_thread.is_alive():
            self._tx_thread.join(1.5)
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
