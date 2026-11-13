#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LinSight CAN 台架控制台（Windows 侧单文件 GUI）

功能:
  - 一键把 PCAN-USB 透传进 WSL2 并配置 can0（自动拉起 WSL，按需提权 bind）
  - 切换 CAN 比特率
  - 发送单帧 / 连续测试帧（cangen）
  - 监听总线报文（candump）与实时 TX/RX 统计

运行:
  python tools\\can_gui.py
"""

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
from pathlib import Path
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

try:
    from ctypes import windll
    windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
USBIPD_FALLBACK = r"C:\Program Files\usbipd-win\usbipd.exe"
DEFAULT_HWID = "0c72:000c"
CONFIG_PATH = Path(os.environ.get("APPDATA", Path.home())) / "LinSight" / "can_gui.json"

USBIPD_LINE = re.compile(
    r"^\s*(?P<busid>\S+)\s+(?P<vidpid>[0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})\s+"
    r"(?P<name>.+?)\s{2,}(?P<state>Not shared|Shared|Attached)\s*$"
)
CANDUMP_LINE = re.compile(
    r"^\s*(?P<iface>\S+)\s+(?P<canid>[0-9A-Fa-f]{1,8})\s+"
    r"\[(?P<dlc>\d{1,2})\]\s*(?P<data>.*)$"
)

COLORS = {
    "ok":   ("#e6f4ea", "#137333"),
    "warn": ("#fef7e0", "#b06000"),
    "bad":  ("#fce8e6", "#c5221f"),
    "idle": ("#eef1f4", "#5f6368"),
}


# ---------------------------------------------------------------- helpers ---
def decode_bytes(raw):
    """WSL 宿主侧消息常为 UTF-16LE，Linux 进程输出为 UTF-8，这里统一解码。"""
    if not raw:
        return ""
    if raw.count(b"\x00") > len(raw) // 4:
        return raw.decode("utf-16-le", errors="replace")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("mbcs", errors="replace")


def run_cmd(args, timeout=30):
    try:
        result = subprocess.run(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            creationflags=CREATE_NO_WINDOW,
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            args, -1, "", f"命令超时（{timeout}s）：{args[0]}")
    result.stdout = decode_bytes(result.stdout)
    result.stderr = decode_bytes(result.stderr)
    return result


def output_of(result):
    return ((result.stdout or "") + (result.stderr or "")).strip()


def find_usbipd():
    path = shutil.which("usbipd")
    if path:
        return path
    if os.path.exists(USBIPD_FALLBACK):
        return USBIPD_FALLBACK
    return None


def parse_usbipd_devices(text):
    devices = []
    for line in text.splitlines():
        m = USBIPD_LINE.match(line)
        if m:
            devices.append({
                "busid": m.group("busid"),
                "vidpid": m.group("vidpid").lower(),
                "name": m.group("name").strip(),
                "state": m.group("state"),
            })
    return devices


def parse_candump_line(line):
    m = CANDUMP_LINE.match(line)
    if not m:
        return None
    return {
        "canid": int(m.group("canid"), 16),
        "dlc": int(m.group("dlc")),
        "data": " ".join(m.group("data").split()),
    }


def parse_can_id(text):
    raw = text.strip().lower().replace("0x", "")
    if not raw or len(raw) > 8:
        raise ValueError("ID 需为 1–8 位十六进制，如 123 或 18FF50E5")
    try:
        value = int(raw, 16)
    except ValueError:
        raise ValueError("ID 含非十六进制字符")
    if value > 0x1FFFFFFF:
        raise ValueError("ID 超出 29 位范围")
    return value


def parse_payload(text):
    raw = re.sub(r"[\s,;:_-]+", "", text.strip())
    if not raw:
        return b""
    if len(raw) % 2:
        raise ValueError("数据需为偶数个十六进制字符")
    if len(raw) > 16:
        raise ValueError("最多 8 字节（16 个十六进制字符）")
    try:
        return bytes.fromhex(raw)
    except ValueError:
        raise ValueError("数据含非十六进制字符")


def cansend_frame(can_id, data, extended):
    id_text = f"{can_id:08X}" if (extended or can_id > 0x7FF) else f"{can_id:03X}"
    return f"{id_text}#{data.hex().upper()}"


# ------------------------------------------------------------------- app ----
class CanBenchApp:
    def __init__(self, root):
        self.root = root
        self.root.title("LinSight CAN 台架控制台")
        self.root.geometry("980x820")
        self.root.minsize(840, 680)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        self.q = queue.Queue()
        self._closing = False
        self._polling = False
        self._connecting = False
        self._gen_stopping = False
        self._mon_stopping = False
        self._gen_proc = None
        self._mon_proc = None
        self._mon_total = 0
        self._prev_stats = None
        self._last_usb_state = None
        self._last_busid = None
        self._last_can_present = False
        self._can_fail_count = 0

        self._load_config()
        self._build_style()
        self._build_ui()

        self.log("info", "就绪：点『一键连接』把 PCAN-USB 接入 WSL2")
        self.root.after(80, self._pump)
        self.root.after(600, self._poll_loop)

    # ------------------------------------------------------------- config --
    def _load_config(self):
        cfg = {}
        try:
            cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception:
            pass
        self.distro_var = tk.StringVar(value=cfg.get("distro", "Ubuntu-22.04"))
        self.iface_var = tk.StringVar(value=cfg.get("iface", "can0"))
        self.hwid_var = tk.StringVar(value=cfg.get("hwid", DEFAULT_HWID))
        self.bitrate_var = tk.StringVar(value=str(cfg.get("bitrate", 500000)))
        self.canid_var = tk.StringVar(value=cfg.get("canid", "123"))
        self.ext_var = tk.BooleanVar(value=bool(cfg.get("extended", False)))
        self.dlc_var = tk.IntVar(value=int(cfg.get("dlc", 8)))
        self.data_var = tk.StringVar(value=cfg.get("data", "11 22 33 44 55 66 77 88"))
        self.gap_var = tk.StringVar(value=str(cfg.get("gap", 20)))
        self.count_var = tk.StringVar(value=str(cfg.get("count", "500")))
        self.datamode_var = tk.StringVar(value=cfg.get("datamode", "递增"))
        self.data_var.trace_add("write", self._sync_dlc_from_data)

    def _save_config(self):
        cfg = {
            "distro": self.distro_var.get(),
            "iface": self.iface_var.get(),
            "hwid": self.hwid_var.get(),
            "bitrate": self.bitrate_var.get(),
            "canid": self.canid_var.get(),
            "extended": bool(self.ext_var.get()),
            "dlc": int(self.dlc_var.get()),
            "data": self.data_var.get(),
            "gap": self.gap_var.get(),
            "count": self.count_var.get(),
            "datamode": self.datamode_var.get(),
        }
        try:
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            CONFIG_PATH.write_text(json.dumps(cfg, ensure_ascii=False, indent=2),
                                   encoding="utf-8")
        except Exception:
            pass

    # -------------------------------------------------------------- style --
    def _build_style(self):
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        try:
            self.root.tk.call("tk", "scaling",
                              max(1.0, self.root.winfo_fpixels("1i") / 72.0))
        except tk.TclError:
            pass
        base = ("Microsoft YaHei UI", 10)
        style.configure(".", font=base)
        style.configure("Title.TLabel", font=("Microsoft YaHei UI", 17, "bold"))
        style.configure("Sub.TLabel", foreground="#5f6368")
        style.configure("Chip.TLabel", padding=(10, 6), anchor="center")
        style.configure("Stat.TLabel", foreground="#3c4043")
        style.configure("TLabelframe.Label", font=("Microsoft YaHei UI", 10, "bold"))
        style.configure("Treeview", font=("Consolas", 9), rowheight=24)
        style.configure("Treeview.Heading", font=("Microsoft YaHei UI", 9, "bold"))
        style.configure("Accent.TButton", font=("Microsoft YaHei UI", 10, "bold"),
                        padding=(14, 6))
        style.configure("Stop.TButton", font=("Microsoft YaHei UI", 10, "bold"),
                        padding=(12, 6), foreground="#c5221f")

    # ---------------------------------------------------------------- ui ---
    def _build_ui(self):
        outer = ttk.Frame(self.root, padding=12)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(5, weight=2)
        outer.rowconfigure(6, weight=1)

        head = ttk.Frame(outer)
        head.grid(row=0, column=0, sticky="ew")
        ttk.Label(head, text="LinSight CAN 台架控制台", style="Title.TLabel").pack(side="left")
        ttk.Label(head, text="PCAN-USB → WSL2 SocketCAN", style="Sub.TLabel").pack(
            side="left", padx=(12, 0), pady=(8, 0))

        chips = ttk.Frame(outer)
        chips.grid(row=1, column=0, sticky="ew", pady=(8, 8))
        for i in range(3):
            chips.columnconfigure(i, weight=1, uniform="chip")
        self.chip_usb = tk.Label(chips, text="USB · 未检测", **self._chip_style("idle"))
        self.chip_wsl = tk.Label(chips, text="WSL · 未连接", **self._chip_style("idle"))
        self.chip_can = tk.Label(chips, text="CAN · 未连接", **self._chip_style("idle"))
        self.chip_usb.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.chip_wsl.grid(row=0, column=1, sticky="ew", padx=4)
        self.chip_can.grid(row=0, column=2, sticky="ew", padx=(4, 0))

        # ---- 连接 ----
        conn = ttk.LabelFrame(outer, text=" 连接 ", padding=10)
        conn.grid(row=2, column=0, sticky="ew", pady=(0, 8))
        for i in range(6):
            conn.columnconfigure(i, weight=0)
        conn.columnconfigure(1, weight=1)

        ttk.Label(conn, text="发行版").grid(row=0, column=0, sticky="w")
        self.distro_entry = ttk.Entry(conn, textvariable=self.distro_var, width=18)
        self.distro_entry.grid(row=0, column=1, sticky="w", padx=(6, 16))

        ttk.Label(conn, text="比特率").grid(row=0, column=2, sticky="w")
        self.bitrate_box = ttk.Combobox(
            conn, textvariable=self.bitrate_var, width=10,
            values=("125000", "250000", "500000", "800000", "1000000"))
        self.bitrate_box.grid(row=0, column=3, sticky="w", padx=(6, 16))

        ttk.Label(conn, text="接口").grid(row=0, column=4, sticky="w")
        ttk.Entry(conn, textvariable=self.iface_var, width=8).grid(
            row=0, column=5, sticky="w", padx=(6, 0))

        btns = ttk.Frame(conn)
        btns.grid(row=1, column=0, columnspan=6, sticky="w", pady=(10, 0))
        self.connect_btn = ttk.Button(btns, text="一键连接", style="Accent.TButton",
                                      command=lambda: self.do_connect(False))
        self.apply_btn = ttk.Button(btns, text="应用速率", command=self.do_apply_bitrate)
        self.reattach_btn = ttk.Button(btns, text="强制重挂",
                                       command=lambda: self.do_connect(True))
        self.disconnect_btn = ttk.Button(btns, text="断开", command=self.do_disconnect)
        self.restart_wsl_btn = ttk.Button(btns, text="重启 WSL", command=self.do_restart_wsl)
        self.connect_btn.pack(side="left")
        self.apply_btn.pack(side="left", padx=(8, 0))
        self.reattach_btn.pack(side="left", padx=(8, 0))
        self.disconnect_btn.pack(side="left", padx=(8, 0))
        self.restart_wsl_btn.pack(side="left", padx=(8, 0))
        ttk.Label(btns, text="首次/换 USB 口会弹一次 UAC（bind）", style="Sub.TLabel").pack(
            side="left", padx=(16, 0))

        ttk.Label(conn, text="VID:PID").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(conn, textvariable=self.hwid_var, width=14).grid(
            row=2, column=1, sticky="w", padx=(6, 16), pady=(8, 0))

        # ---- 发送 ----
        send = ttk.LabelFrame(outer, text=" 发送测试帧 ", padding=10)
        send.grid(row=3, column=0, sticky="ew", pady=(0, 8))
        send.columnconfigure(4, weight=1)

        ttk.Label(send, text="ID").grid(row=0, column=0, sticky="w")
        ttk.Entry(send, textvariable=self.canid_var, width=12).grid(
            row=0, column=1, sticky="w", padx=(6, 10))
        ttk.Checkbutton(send, text="扩展帧", variable=self.ext_var).grid(
            row=0, column=2, sticky="w", padx=(0, 14))
        ttk.Label(send, text="数据").grid(row=0, column=3, sticky="w")
        ttk.Entry(send, textvariable=self.data_var).grid(
            row=0, column=4, sticky="ew", padx=(6, 10))
        self.send_btn = ttk.Button(send, text="发送单帧", command=self.do_send_single)
        self.send_btn.grid(row=0, column=5, sticky="e")

        ttk.Label(send, text="连续发送").grid(row=1, column=0, sticky="w", pady=(10, 0))
        gen_row = ttk.Frame(send)
        gen_row.grid(row=1, column=1, columnspan=5, sticky="w", pady=(10, 0))
        ttk.Label(gen_row, text="间隔(ms)").pack(side="left")
        ttk.Spinbox(gen_row, textvariable=self.gap_var, from_=0, to=10000,
                    width=6).pack(side="left", padx=(5, 12))
        ttk.Label(gen_row, text="帧数(-1=无限)").pack(side="left")
        ttk.Spinbox(gen_row, textvariable=self.count_var, from_=-1, to=10000000,
                    width=8).pack(side="left", padx=(5, 12))
        ttk.Label(gen_row, text="数据").pack(side="left")
        ttk.Combobox(gen_row, textvariable=self.datamode_var, width=6,
                     values=("递增", "固定"), state="readonly").pack(
            side="left", padx=(5, 12))
        self.gen_start_btn = ttk.Button(gen_row, text="开始连续发送",
                                        command=self.do_start_gen)
        self.gen_start_btn.pack(side="left")
        self.gen_stop_btn = ttk.Button(gen_row, text="停止", style="Stop.TButton",
                                       command=self.do_stop_gen, state="disabled")
        self.gen_stop_btn.pack(side="left", padx=(8, 0))
        self.gen_state_var = tk.StringVar(value="")
        ttk.Label(gen_row, textvariable=self.gen_state_var, style="Sub.TLabel").pack(
            side="left", padx=(12, 0))

        ttk.Label(send, text="单帧数据长度自动作为 DLC；连续发送按上方 DLC / 数据设置。",
                  style="Sub.TLabel").grid(row=2, column=0, columnspan=6, sticky="w",
                                           pady=(8, 0))

        # ---- 统计 ----
        stats = ttk.Frame(outer)
        stats.grid(row=4, column=0, sticky="ew", pady=(0, 8))
        self.tx_total_var = tk.StringVar(value="TX 0")
        self.tx_rate_var = tk.StringVar(value="0 帧/s")
        self.rx_total_var = tk.StringVar(value="RX 0")
        self.rx_rate_var = tk.StringVar(value="0 帧/s")
        self.err_var = tk.StringVar(value="错误 TX 0 / RX 0")
        self.bus_var = tk.StringVar(value="总线 —")
        for var in (self.tx_total_var, self.tx_rate_var, self.rx_total_var,
                    self.rx_rate_var, self.err_var, self.bus_var):
            ttk.Label(stats, textvariable=var, style="Stat.TLabel").pack(
                side="left", padx=(0, 22))

        # ---- 监听 ----
        mon = ttk.LabelFrame(outer, text=" 总线监听 ", padding=10)
        mon.grid(row=5, column=0, sticky="nsew", pady=(0, 8))
        mon.columnconfigure(0, weight=1)
        mon.rowconfigure(1, weight=1)

        mon_bar = ttk.Frame(mon)
        mon_bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        self.mon_start_btn = ttk.Button(mon_bar, text="开始监听",
                                        command=self.do_start_monitor)
        self.mon_stop_btn = ttk.Button(mon_bar, text="停止", command=self.do_stop_monitor,
                                       state="disabled")
        self.mon_clear_btn = ttk.Button(mon_bar, text="清空", command=self._clear_monitor)
        self.mon_start_btn.pack(side="left")
        self.mon_stop_btn.pack(side="left", padx=(8, 0))
        self.mon_clear_btn.pack(side="left", padx=(8, 0))
        self.mon_count_var = tk.StringVar(value="已接收 0 帧")
        ttk.Label(mon_bar, textvariable=self.mon_count_var, style="Sub.TLabel").pack(
            side="left", padx=(14, 0))

        tree_wrap = ttk.Frame(mon)
        tree_wrap.grid(row=1, column=0, sticky="nsew")
        tree_wrap.columnconfigure(0, weight=1)
        tree_wrap.rowconfigure(0, weight=1)
        self.mon_tree = ttk.Treeview(tree_wrap, columns=("time", "id", "dlc", "data"),
                                     show="headings", height=8)
        self.mon_tree.heading("time", text="时间")
        self.mon_tree.heading("id", text="CAN ID")
        self.mon_tree.heading("dlc", text="DLC")
        self.mon_tree.heading("data", text="数据")
        self.mon_tree.column("time", width=110, anchor="center")
        self.mon_tree.column("id", width=110, anchor="center")
        self.mon_tree.column("dlc", width=50, anchor="center")
        self.mon_tree.column("data", width=420, anchor="w")
        self.mon_tree.grid(row=0, column=0, sticky="nsew")
        yscroll = ttk.Scrollbar(tree_wrap, orient="vertical", command=self.mon_tree.yview)
        yscroll.grid(row=0, column=1, sticky="ns")
        self.mon_tree.configure(yscrollcommand=yscroll.set)

        # ---- 日志 ----
        logf = ttk.LabelFrame(outer, text=" 日志 ", padding=6)
        logf.grid(row=6, column=0, sticky="nsew")
        logf.columnconfigure(0, weight=1)
        logf.rowconfigure(0, weight=1)
        self.log_text = ScrolledText(logf, height=7, wrap="word", state="disabled",
                                     font=("Consolas", 9), relief="flat")
        self.log_text.grid(row=0, column=0, sticky="nsew")
        self.log_text.tag_configure("info", foreground="#3c4043")
        self.log_text.tag_configure("ok", foreground="#137333")
        self.log_text.tag_configure("warn", foreground="#b06000")
        self.log_text.tag_configure("err", foreground="#c5221f")

        ttk.Label(outer, text="提示：固件默认 LISTEN_ONLY（不 ACK）；双节点台架测试请在 "
                              "Settings/menuconfig 切 NORMAL。",
                  style="Sub.TLabel").grid(row=7, column=0, sticky="w", pady=(8, 0))

    @staticmethod
    def _chip_style(level):
        bg, fg = COLORS[level]
        return {"bg": bg, "fg": fg, "font": ("Microsoft YaHei UI", 10, "bold"),
                "padx": 10, "pady": 6}

    def _set_chip(self, chip, text, level):
        bg, fg = COLORS[level]
        chip.configure(text=text, bg=bg, fg=fg)

    def _sync_dlc_from_data(self, *_args):
        try:
            data = parse_payload(self.data_var.get())
            self.dlc_var.set(len(data))
        except ValueError:
            pass

    # ------------------------------------------------------------- queue ---
    def post(self, kind, payload=None):
        self.q.put((kind, payload))

    def log(self, level, message):
        self.post("log", (level, message))

    def _pump(self):
        if self._closing:
            return
        try:
            for _ in range(300):
                kind, payload = self.q.get_nowait()
                self._handle(kind, payload)
        except queue.Empty:
            pass
        self.root.after(80, self._pump)

    def _handle(self, kind, payload):
        if kind == "log":
            level, message = payload
            stamp = time.strftime("%H:%M:%S")
            self.log_text.configure(state="normal")
            self.log_text.insert("end", f"[{stamp}] {message}\n", level)
            try:  # 限制日志长度，长跑不无限增长
                if int(self.log_text.index("end-1c").split(".")[0]) > 2000:
                    self.log_text.delete("1.0", "500.0")
            except Exception:
                pass
            self.log_text.see("end")
            self.log_text.configure(state="disabled")
        elif kind == "usb":
            self._apply_usb_state(payload)
        elif kind == "can":
            self._apply_can_stats(payload)
        elif kind == "frames":
            rows, total = payload
            for row in rows:
                self.mon_tree.insert("", "end", values=row)
            children = self.mon_tree.get_children()
            for item in children[:-300]:
                self.mon_tree.delete(item)
            if children:
                self.mon_tree.yview_moveto(1)
            self.mon_count_var.set(f"已接收 {total} 帧")
        elif kind == "mon_started":
            self.mon_start_btn.configure(state="disabled")
            self.mon_stop_btn.configure(state="normal")
        elif kind == "mon_stopped":
            self.mon_start_btn.configure(state="normal")
            self.mon_stop_btn.configure(state="disabled")
            if payload not in (None, 0) and not self._mon_stopping \
                    and not self._closing:
                self.log("warn", f"candump 已退出（code={payload}）")
        elif kind == "gen_started":
            self.gen_start_btn.configure(state="disabled")
            self.gen_stop_btn.configure(state="normal")
            self.gen_state_var.set("发送中…")
        elif kind == "gen_stopped":
            self.gen_start_btn.configure(state="normal")
            self.gen_stop_btn.configure(state="disabled")
            self.gen_state_var.set("")
            if payload not in (None, 0) and not self._gen_stopping and not self._closing:
                self.log("warn", f"cangen 已退出（code={payload}）")
        elif kind == "conn_done":
            self._connecting = False
            self._set_conn_enabled(True)
        elif kind == "alert":
            title, message = payload
            messagebox.showerror(title, message, parent=self.root)
        elif kind == "poll_now":
            pass

    def _set_conn_enabled(self, enabled):
        state = "normal" if enabled else "disabled"
        for btn in (self.connect_btn, self.apply_btn, self.reattach_btn,
                    self.disconnect_btn, self.restart_wsl_btn):
            btn.configure(state=state)

    # -------------------------------------------------------------- state --
    def _wsl_root(self, script, timeout=25):
        return run_cmd(["wsl", "-d", self.distro_var.get().strip(), "-u", "root",
                        "--exec", "bash", "-c", script], timeout)

    def _find_device(self):
        usbipd = find_usbipd()
        if not usbipd:
            return None
        result = run_cmd([usbipd, "list"], 20)
        hwid = self.hwid_var.get().strip().lower()
        for dev in parse_usbipd_devices(result.stdout):
            if dev["vidpid"] == hwid:
                return dev
        return None

    def _can_stats(self):
        iface = self.iface_var.get().strip() or "can0"
        for attempt in (1, 2):
            try:
                result = self._wsl_root(f"ip -j -d -s link show {iface}", 15)
            except Exception:
                result = None
            if result is not None and result.returncode == 0 \
                    and result.stdout.strip():
                try:
                    return self._parse_can_stats(json.loads(result.stdout)[0])
                except Exception:
                    pass
            if attempt == 1:
                time.sleep(0.25)  # WSL 单次抖动，快速重试一次
        return None

    @staticmethod
    def _parse_can_stats(data):
        info = (data.get("linkinfo") or {}).get("info_data") or {}
        timings = info.get("bittiming") or {}
        xstats = (data.get("linkinfo") or {}).get("info_xstats") or {}
        stats = data.get("stats64") or data.get("stats") or {}
        rx = stats.get("rx") or {}
        tx = stats.get("tx") or {}
        return {
            "up": data.get("operstate") == "UP",
            "operstate": data.get("operstate", "?"),
            "state": info.get("state", "?"),
            "bitrate": timings.get("bitrate"),
            "tx_packets": tx.get("packets", 0),
            "rx_packets": rx.get("packets", 0),
            "tx_errors": tx.get("errors", 0),
            "rx_errors": rx.get("errors", 0),
            "restarts": xstats.get("restarts", 0),
            "bus_off": xstats.get("bus_off", 0),
        }

    def _apply_usb_state(self, dev):
        state = dev["state"] if dev else None
        busid = dev["busid"] if dev else None
        if state != self._last_usb_state or busid != self._last_busid:
            if dev:
                self.log("info", f"USB 状态：{dev['name']} {dev['busid']} → {state}")
            self._last_usb_state = state
            self._last_busid = busid
        if not dev:
            self._set_chip(self.chip_usb, "USB · 未检测到 PCAN", "bad")
            self._set_chip(self.chip_wsl, "WSL · 未连接", "idle")
            return
        if state == "Attached":
            self._set_chip(self.chip_usb, f"USB · {dev['busid']} 已挂载", "ok")
            self._set_chip(self.chip_wsl, f"WSL · {self.distro_var.get()}", "ok")
        elif state == "Shared":
            self._set_chip(self.chip_usb, f"USB · {dev['busid']} 已共享", "warn")
            self._set_chip(self.chip_wsl, "WSL · 未挂载", "idle")
        else:
            self._set_chip(self.chip_usb, f"USB · {dev['busid']} 未共享", "idle")
            self._set_chip(self.chip_wsl, "WSL · 未连接", "idle")

    def _apply_can_stats(self, stats):
        iface = self.iface_var.get().strip() or "can0"
        present = stats is not None
        if present != self._last_can_present:
            self._last_can_present = present
            if present:
                self.log("ok", f"检测到 {iface}，接口已就绪")
            elif self._last_usb_state == "Attached":
                self.log("warn", f"{iface} 消失：USB 设备可能被重插，"
                                 "或 WSL 探测连续失败")
            # 设备本来未挂载时不再重复提示，状态灯已显示
        if not stats:
            self._set_chip(self.chip_can, "CAN · 未连接", "idle")
            self.tx_total_var.set("TX 0")
            self.tx_rate_var.set("0 帧/s")
            self.rx_total_var.set("RX 0")
            self.rx_rate_var.set("0 帧/s")
            self.err_var.set("错误 TX 0 / RX 0")
            self.bus_var.set("总线 —")
            self._prev_stats = None
            return

        rate = stats["bitrate"]
        rate_text = f"{rate // 1000}k" if rate else "?"
        if stats["up"]:
            self._set_chip(self.chip_can, f"CAN · {iface} {rate_text} 已启动", "ok")
        else:
            self._set_chip(self.chip_can, f"CAN · {iface} 未启动", "warn")

        now = time.monotonic()
        tx_rate = rx_rate = 0
        if self._prev_stats:
            prev_t, prev_tx, prev_rx = self._prev_stats
            dt = now - prev_t
            if dt > 0.2:
                tx_rate = max(0, int((stats["tx_packets"] - prev_tx) / dt))
                rx_rate = max(0, int((stats["rx_packets"] - prev_rx) / dt))
        self._prev_stats = (now, stats["tx_packets"], stats["rx_packets"])

        self.tx_total_var.set(f"TX {stats['tx_packets']:,}")
        self.tx_rate_var.set(f"{tx_rate:,} 帧/s")
        self.rx_total_var.set(f"RX {stats['rx_packets']:,}")
        self.rx_rate_var.set(f"{rx_rate:,} 帧/s")
        self.err_var.set(f"错误 TX {stats['tx_errors']} / RX {stats['rx_errors']}")
        self.bus_var.set(
            f"总线 {stats['state']} · 重启 {stats['restarts']} · BusOff {stats['bus_off']}")

    # -------------------------------------------------------------- poll ---
    def _poll_loop(self):
        if self._closing:
            return
        if not self._polling:
            self._polling = True
            threading.Thread(target=self._poll_worker, daemon=True).start()
        self.root.after(2000, self._poll_loop)

    def _poll_worker(self):
        try:
            dev = self._find_device()
            self.post("usb", dev)
            if dev and dev["state"] == "Attached":
                stats = self._can_stats()
                if stats is not None:
                    self._can_fail_count = 0
                    self.post("can", stats)
                else:
                    # 单次探测失败可能只是 WSL 抖动，连续失败才判定接口消失
                    self._can_fail_count += 1
                    if self._can_fail_count >= 2:
                        self.post("can", None)
            else:
                self._can_fail_count = 0
                self.post("can", None)
        finally:
            self._polling = False

    # ----------------------------------------------------------- connect ---
    def do_connect(self, force=False):
        if self._connecting:
            return
        self._connecting = True
        self._set_conn_enabled(False)
        threading.Thread(target=self._connect_worker, args=(force,), daemon=True).start()

    def _connect_worker(self, force):
        try:
            self.log("info", "开始连接 PCAN-USB → WSL2 ...")
            usbipd = find_usbipd()
            if not usbipd:
                raise RuntimeError("未找到 usbipd.exe，请先安装：winget install -e "
                                   "--id dorssel.usbipd-win")
            dev = self._find_device()
            if not dev:
                raise RuntimeError(
                    f"未找到 USB-CAN（VID:PID={self.hwid_var.get()}）。"
                    "请检查 USB 线和设备管理器。")
            self.log("info", f"设备 {dev['name']} BUSID={dev['busid']} 状态={dev['state']}")

            distro = self.distro_var.get().strip()
            if dev["state"] == "Not shared":
                self._bind_elevated(usbipd, dev["busid"])
                dev = self._find_device() or dev

            self.log("info", f"启动 WSL 发行版 {distro} ...")
            self._ensure_wsl(distro)
            self.log("ok", "WSL 已运行")

            if dev["state"] == "Attached" and not force:
                self.log("ok", "设备已处于 Attached，跳过 attach")
            else:
                if dev["state"] == "Attached":
                    self.log("info", "清除旧挂载 ...")
                    run_cmd([usbipd, "detach", "--busid", dev["busid"]], 30)
                    time.sleep(0.5)
                result = run_cmd([usbipd, "attach", "--wsl", "--busid", dev["busid"]], 90)
                if result.returncode != 0:
                    dev2 = self._find_device()
                    if dev2 and dev2["state"] == "Attached":
                        self.log("warn", "attach 返回非零，但设备状态已是 Attached，继续")
                    else:
                        self.log("warn", "attach 失败（" +
                                 (output_of(result) or f"exit={result.returncode}") +
                                 "），重试 ...")
                        time.sleep(1.2)
                        result = run_cmd(
                            [usbipd, "attach", "--wsl", "--busid", dev["busid"]], 90)
                        if result.returncode != 0:
                            dev2 = self._find_device()
                            if not (dev2 and dev2["state"] == "Attached"):
                                raise RuntimeError("attach 失败：" +
                                                   (output_of(result) or
                                                    f"exit={result.returncode}"))
                self.log("ok", "USB-CAN 已挂载到 WSL")

            self._apply_bitrate_worker()
            self.log("ok", "台架链路就绪")
            self.post("poll_now", None)
        except Exception as exc:
            self.log("err", str(exc))
            self.post("alert", ("连接失败", str(exc)))
        finally:
            self._connecting = False
            self.post("conn_done", None)

    def _bind_elevated(self, usbipd, busid):
        self.log("warn", "设备未共享，需要管理员 bind（请确认 UAC）...")
        script = (
            f"$p = Start-Process -FilePath '{usbipd}' "
            f"-ArgumentList @('bind','--busid','{busid}') "
            "-Verb RunAs -WindowStyle Hidden -Wait -PassThru; exit $p.ExitCode"
        )
        result = run_cmd(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                          "-Command", script], 180)
        if result.returncode != 0:
            detail = output_of(result) or "UAC 被取消或无权限"
            raise RuntimeError("bind 失败：" + detail)
        self.log("ok", "设备已共享")

    def _ensure_wsl(self, distro):
        """启动并确认 WSL 可用；对切换瞬间的空输出失败做重试。"""
        last_rc, last_detail = "?", ""
        for attempt in range(1, 4):
            result = run_cmd(
                ["wsl", "-d", distro, "-u", "root", "--exec", "/bin/sh", "-c",
                 "echo __LINSIGHT_WSL_OK__"], 45)
            text = output_of(result)
            if "__LINSIGHT_WSL_OK__" in text:
                return
            last_rc = result.returncode
            last_detail = text or "无输出"
            if attempt < 3:
                self.log("warn", f"启动 WSL 第 {attempt} 次失败"
                                 f"（exit={last_rc}），重试 ...")
                time.sleep(1.5)

        state = self._wsl_distro_state(distro)
        if state.lower().startswith("run") or "运行" in state:
            # 探测命令偶发失败但发行版实际在运行（root 命令随后可用），继续
            self.log("warn", f"WSL 探测失败（exit={last_rc}），"
                             f"但发行版状态为 {state}，继续连接 ...")
            return
        raise RuntimeError(
            f"无法启动 WSL 发行版 '{distro}'（exit={last_rc}）：{last_detail}\n"
            f"当前状态：{state or '未知'}\n"
            "可点击『重启 WSL』（会终止 WSL 内所有进程），"
            "或在终端执行 wsl --shutdown 后重试。")

    def _wsl_distro_state(self, distro):
        result = run_cmd(["wsl", "-l", "-v"], 20)
        for line in output_of(result).splitlines():
            parts = line.replace("*", " ").split()
            if len(parts) >= 3 and parts[0] == distro:
                return " ".join(parts[1:-1])
        return ""

    # ----------------------------------------------------------- bitrate ---
    def do_apply_bitrate(self):
        if self._connecting:
            return
        threading.Thread(target=self._apply_bitrate_guarded, daemon=True).start()

    def _apply_bitrate_guarded(self):
        try:
            if self._gen_proc and self._gen_proc.poll() is None:
                self.log("info", "先停止连续发送，再切换速率")
                self._stop_gen_sync()
            self._apply_bitrate_worker()
            self.post("poll_now", None)
        except Exception as exc:
            self.log("err", str(exc))
            self.post("alert", ("配置失败", str(exc)))

    def _apply_bitrate_worker(self):
        try:
            bitrate = int(str(self.bitrate_var.get()).replace("_", "").strip())
        except ValueError:
            raise RuntimeError("比特率必须是整数")
        if not 1000 <= bitrate <= 1000000:
            raise RuntimeError("比特率超出 1k–1M 范围")
        iface = self.iface_var.get().strip() or "can0"
        # attach 后 can0 可能晚几百毫秒出现，先单独等接口节点就绪（最多 10 s）
        self._wsl_root(
            f"for i in $(seq 1 50); do [ -e /sys/class/net/{iface} ] && break; "
            "sleep 0.2; done", 15)
        script = (
            f"ip link set {iface} down 2>/dev/null; "
            f"ip link set {iface} type can bitrate {bitrate} restart-ms 100 && "
            f"ip link set {iface} txqueuelen 1000 && "
            f"ip link set {iface} up"
        )
        result = None
        for attempt in (1, 2, 3):
            result = self._wsl_root(script, 20)
            if result.returncode == 0:
                break
            if attempt < 3:
                # WSL 状态切换瞬间可能空输出失败，配置幂等，短暂重试
                time.sleep(0.6 * attempt)
        if result is None or result.returncode != 0:
            detail = (output_of(result) if result else "") or \
                f"exit={result.returncode if result else '?'}"
            raise RuntimeError(
                f"配置 {iface} 失败：{detail}（先确认已连接并出现 can0）")
        self.log("ok", f"{iface} 已启动 @ {bitrate} bps")

    # ---------------------------------------------------------- disconnect -
    def do_disconnect(self):
        if self._connecting:
            return
        threading.Thread(target=self._disconnect_worker, daemon=True).start()

    def _disconnect_worker(self):
        try:
            self.log("info", "断开连接 ...")
            self._stop_gen_sync(silent=True)
            self._stop_monitor_sync(silent=True)
            iface = self.iface_var.get().strip() or "can0"
            self._wsl_root(f"ip link set {iface} down 2>/dev/null || true", 15)
            dev = self._find_device()
            usbipd = find_usbipd()
            if dev and usbipd:
                result = run_cmd([usbipd, "detach", "--busid", dev["busid"]], 30)
                if result.returncode == 0:
                    self.log("ok", "设备已从 WSL 断开（保持 Shared）")
                else:
                    self.log("warn", "detach 失败：" + output_of(result))
            else:
                self.log("info", "设备当前不在 Windows 侧，无需 detach")
            self.post("poll_now", None)
        except Exception as exc:
            self.log("err", str(exc))

    def do_restart_wsl(self):
        if not messagebox.askyesno(
                "重启 WSL",
                "将执行 wsl --shutdown，终止所有 WSL 发行版中的进程"
                "（包括 linsightd）。\n确定继续？",
                parent=self.root):
            return
        threading.Thread(target=self._restart_wsl_worker, daemon=True).start()

    def _restart_wsl_worker(self):
        self.log("warn", "正在执行 wsl --shutdown ...")
        result = run_cmd(["wsl", "--shutdown"], 60)
        if result.returncode != 0:
            self.log("err", "wsl --shutdown 失败：" +
                     (output_of(result) or f"exit={result.returncode}"))
            return
        time.sleep(1.5)
        self.log("ok", "WSL 已关闭，可重新点『一键连接』")
        self.post("poll_now", None)

    # --------------------------------------------------------------- send --
    def _frame_inputs(self):
        can_id = parse_can_id(self.canid_var.get())
        data = parse_payload(self.data_var.get())
        extended = bool(self.ext_var.get()) or can_id > 0x7FF
        return can_id, data, extended

    def do_send_single(self):
        try:
            can_id, data, extended = self._frame_inputs()
        except ValueError as exc:
            messagebox.showerror("参数错误", str(exc), parent=self.root)
            return
        iface = self.iface_var.get().strip() or "can0"
        frame = cansend_frame(can_id, data, extended)
        threading.Thread(target=self._send_single_worker, args=(iface, frame),
                         daemon=True).start()

    def _send_single_worker(self, iface, frame):
        try:
            result = self._wsl_root(f"cansend {iface} {frame}", 15)
            if result.returncode != 0:
                self.log("err", f"发送失败 {frame}：{output_of(result)}")
            else:
                self.log("ok", f"已发送 {frame}")
        except Exception as exc:
            self.log("err", f"发送异常：{exc}")

    def do_start_gen(self):
        if self._gen_proc and self._gen_proc.poll() is None:
            return
        try:
            can_id, data, extended = self._frame_inputs()
            gap = int(self.gap_var.get())
            count = int(self.count_var.get())
            dlc = int(self.dlc_var.get())
        except ValueError as exc:
            messagebox.showerror("参数错误", str(exc), parent=self.root)
            return
        if gap < 0 or gap > 60000:
            messagebox.showerror("参数错误", "间隔需在 0–60000 ms", parent=self.root)
            return
        if not 0 <= dlc <= 8:
            messagebox.showerror("参数错误", "DLC 需在 0–8", parent=self.root)
            return
        mode = self.datamode_var.get()
        if mode == "固定" and dlc > 0 and len(data) != dlc:
            messagebox.showerror("参数错误",
                                 f"固定数据长度 {len(data)} 与 DLC {dlc} 不一致",
                                 parent=self.root)
            return
        iface = self.iface_var.get().strip() or "can0"
        cmd = f"exec cangen {iface} -I {can_id:X} -L {dlc} -g {gap} -p 10"
        if count > 0:
            cmd += f" -n {count}"
        if extended:
            cmd += " -e"
        if dlc > 0:
            cmd += " -D i" if mode == "递增" else f" -D {data.hex().upper()}"
        self.gen_state_var.set("启动中…")
        threading.Thread(target=self._gen_worker, args=(cmd,), daemon=True).start()

    def _gen_worker(self, cmd):
        distro = self.distro_var.get().strip()
        try:
            proc = subprocess.Popen(
                ["wsl", "-d", distro, "-u", "root", "--exec", "bash", "-c", cmd],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace", bufsize=1,
                creationflags=CREATE_NO_WINDOW)
        except Exception as exc:
            self.log("err", f"启动 cangen 失败：{exc}")
            self.post("gen_stopped", -1)
            return
        self._gen_stopping = False
        self._gen_proc = proc
        self.log("ok", f"连续发送已启动：{cmd.replace('exec ', '', 1)}")
        self.post("gen_started", None)
        for line in proc.stdout:
            if line.strip():
                self.log("info", line.rstrip())
        code = proc.wait()
        self.post("gen_stopped", code)

    def do_stop_gen(self):
        self.gen_state_var.set("停止中…")
        threading.Thread(target=self._stop_gen_worker, daemon=True).start()

    def _stop_gen_worker(self):
        self._gen_stopping = True
        self._stop_gen_sync()
        self.post("gen_stopped", 0)

    def _stop_gen_sync(self, silent=False):
        # 保持 True 直到下一次启动，避免 pkill 造成的退出码被误报为异常
        self._gen_stopping = True
        proc = self._gen_proc
        self._gen_proc = None
        try:
            self._wsl_root("pkill -x cangen 2>/dev/null || true", 8)
        except Exception:
            pass
        if proc and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
        if not silent:
            self.log("info", "连续发送已停止")

    # ------------------------------------------------------------ monitor --
    def do_start_monitor(self):
        if self._mon_proc and self._mon_proc.poll() is None:
            return
        self._mon_total = 0
        threading.Thread(target=self._monitor_worker, daemon=True).start()

    def _monitor_worker(self):
        distro = self.distro_var.get().strip()
        iface = self.iface_var.get().strip() or "can0"
        self._mon_stopping = False
        try:
            proc = subprocess.Popen(
                ["wsl", "-d", distro, "-u", "root", "--exec", "candump", iface],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace", bufsize=1,
                creationflags=CREATE_NO_WINDOW)
        except Exception as exc:
            self.log("err", f"启动 candump 失败：{exc}")
            self.post("mon_stopped", -1)
            return
        self._mon_proc = proc
        self.log("info", f"开始监听 {iface} ...")
        self.post("mon_started", None)
        batch = []
        last_flush = time.monotonic()
        try:
            for line in proc.stdout:
                line = line.rstrip()
                if not line:
                    continue
                parsed = parse_candump_line(line)
                if not parsed:
                    self.log("err", line)
                    continue
                self._mon_total += 1
                now = time.time()
                stamp = time.strftime("%H:%M:%S", time.localtime(now))
                stamp += f".{int(now * 1000) % 1000:03d}"
                batch.append((stamp, f"0x{parsed['canid']:X}",
                              parsed["dlc"], parsed["data"]))
                # 高帧率下只按时间节流，避免 UI 队列被刷爆
                if time.monotonic() - last_flush >= 0.2:
                    self.post("frames", (batch[-25:], self._mon_total))
                    batch = []
                    last_flush = time.monotonic()
        finally:
            if batch:
                self.post("frames", (batch, self._mon_total))
            self.post("mon_stopped", proc.poll())

    def do_stop_monitor(self):
        threading.Thread(target=self._stop_monitor_sync, daemon=True).start()

    def _stop_monitor_sync(self, silent=False):
        self._mon_stopping = True
        proc = self._mon_proc
        self._mon_proc = None
        try:
            self._wsl_root("pkill -x candump 2>/dev/null || true", 8)
        except Exception:
            pass
        if proc and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
        if not silent:
            self.log("info", "监听已停止")

    def _clear_monitor(self):
        for item in self.mon_tree.get_children():
            self.mon_tree.delete(item)
        self._mon_total = 0
        self.mon_count_var.set("已接收 0 帧")

    # -------------------------------------------------------------- close --
    def _on_close(self):
        self._closing = True
        self._save_config()
        proc = self._mon_proc
        self._mon_proc = None
        gen = self._gen_proc
        self._gen_proc = None
        for p in (proc, gen):
            if p and p.poll() is None:
                try:
                    p.terminate()
                except Exception:
                    pass
        try:
            subprocess.Popen(
                ["wsl", "-d", self.distro_var.get().strip(), "-u", "root",
                 "--exec", "bash", "-c", "pkill -x candump; pkill -x cangen"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=CREATE_NO_WINDOW)
        except Exception:
            pass
        self.root.destroy()


def main():
    if os.name != "nt":
        print("此工具运行在 Windows 侧（需要 usbipd 控制 WSL2）。")
        return 1
    root = tk.Tk()
    CanBenchApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
