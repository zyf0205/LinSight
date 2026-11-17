#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LinSight CAN 台架控制台（Windows 侧专业级 GUI 工具）

功能特性:
  - 一键把 PCAN-USB 透传进 WSL2 并自动配置 can0 链路（支持自动拉起 WSL、按需 UAC 提权 bind）
  - 动态发现已安装的 WSL 发行版与连接的 USB-CAN 硬件设备
  - 自由切换 CAN 比特率（20k ~ 1000k）及自动重启参数
  - 单帧发送（支持标准帧/扩展帧、远程帧、输入格式化辅助、Enter 快捷键发送）
  - 常用预设报文管理（一键发送、载入编辑、添加与删除预设、持久化保存）
  - 连续/周期报文生成器（cangen 接口，支持固定/递增/随机数据模式与周期设置）
  - 实时总线报文监视器（支持实时 ID/数据过滤搜索、时间差 Δt 周期计算、暂停显示、冻结查看、导出 CSV）
  - 右键上下文快捷菜单（一键复制 ID/数据/整行、直接载入到发送区或存为预设）
  - 总线健康与统计诊断看板（TX/RX 帧率与总数、TEC/REC 错误计数、Bus-Off 与驱动重启状态、清零统计）
  - 现代化科技风格 UI（完美支持深色 Dark / 浅色 Light 模式一键切换、分栏可拖动调节、高 DPI 缩放适配）

运行方式:
  python tools\\can_gui.py
"""

import csv
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk
from tkinter.scrolledtext import ScrolledText

# Windows 高 DPI 缩放支持
try:
    from ctypes import windll
    try:
        windll.shcore.SetProcessDpiAwareness(2)  # Per-Monitor DPI Aware
    except Exception:
        windll.user32.SetProcessDPIAware()
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

# 默认常用预设报文
DEFAULT_PRESETS = [
    {
        "name": "心跳测试",
        "id": "123",
        "ext": False,
        "dlc": 8,
        "data": "11 22 33 44 55 66 77 88",
    },
    {
        "name": "扩展帧测试",
        "id": "18FF50E5",
        "ext": True,
        "dlc": 8,
        "data": "01 02 03 04 05 06 07 08",
    },
    {
        "name": "OBD-II 诊断查询",
        "id": "7DF",
        "ext": False,
        "dlc": 8,
        "data": "02 01 00 00 00 00 00 00",
    },
    {
        "name": "全零复位帧",
        "id": "100",
        "ext": False,
        "dlc": 8,
        "data": "00 00 00 00 00 00 00 00",
    },
]

# 现代化深色 / 浅色调色板
THEMES = {
    "dark": {
        "name": "dark",
        "bg_app": "#131316",
        "bg_card": "#1d1d23",
        "bg_card_inner": "#17171c",
        "bg_input": "#141418",
        "border": "#2e2e38",
        "border_light": "#3b3b48",
        "text_main": "#f4f4f6",
        "text_sub": "#9da3ae",
        "text_accent": "#60a5fa",
        "accent": "#2563eb",
        "accent_hover": "#1d4ed8",
        "accent_fg": "#ffffff",
        "danger": "#dc2626",
        "danger_hover": "#b91c1c",
        "danger_fg": "#ffffff",
        "btn_bg": "#2b2b35",
        "btn_fg": "#f4f4f6",
        "btn_active": "#393946",
        "tree_bg": "#16161b",
        "tree_fg": "#f4f4f6",
        "tree_even": "#18181e",
        "tree_odd": "#1f1f26",
        "tree_sel": "#1d4ed8",
        "tree_sel_fg": "#ffffff",
        "head_bg": "#262631",
        "head_fg": "#e4e4e7",
        "log_bg": "#121216",
        "log_fg": "#d1d5db",
        "status_ok_bg": "#064e3b",
        "status_ok_fg": "#34d399",
        "status_warn_bg": "#78350f",
        "status_warn_fg": "#fbbf24",
        "status_bad_bg": "#7f1d1d",
        "status_bad_fg": "#f87171",
        "status_idle_bg": "#27272a",
        "status_idle_fg": "#9ca3af",
    },
    "light": {
        "name": "light",
        "bg_app": "#f1f5f9",
        "bg_card": "#ffffff",
        "bg_card_inner": "#f8fafc",
        "bg_input": "#ffffff",
        "border": "#cbd5e1",
        "border_light": "#e2e8f0",
        "text_main": "#0f172a",
        "text_sub": "#64748b",
        "text_accent": "#2563eb",
        "accent": "#2563eb",
        "accent_hover": "#1d4ed8",
        "accent_fg": "#ffffff",
        "danger": "#dc2626",
        "danger_hover": "#b91c1c",
        "danger_fg": "#ffffff",
        "btn_bg": "#e2e8f0",
        "btn_fg": "#0f172a",
        "btn_active": "#cbd5e1",
        "tree_bg": "#ffffff",
        "tree_fg": "#0f172a",
        "tree_even": "#ffffff",
        "tree_odd": "#f8fafc",
        "tree_sel": "#bfdbfe",
        "tree_sel_fg": "#1e3a8a",
        "head_bg": "#e2e8f0",
        "head_fg": "#1e293b",
        "log_bg": "#ffffff",
        "log_fg": "#1e293b",
        "status_ok_bg": "#dcfce7",
        "status_ok_fg": "#16a34a",
        "status_warn_bg": "#fef3c7",
        "status_warn_fg": "#d97706",
        "status_bad_bg": "#fee2e2",
        "status_bad_fg": "#dc2626",
        "status_idle_bg": "#f1f5f9",
        "status_idle_fg": "#64748b",
    },
}


# ---------------------------------------------------------------- helpers ---
def decode_bytes(raw):
    """统一解码 WSL 宿主与进程输出（兼容 UTF-16LE / UTF-8 / MBCS）。"""
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
            args, -1, "", f"命令执行超时（{timeout}s）：{args[0]}"
        )
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


def list_wsl_distros():
    """获取 Windows 宿主机上已安装的所有 WSL 发行版列表。"""
    try:
        res = run_cmd(["wsl", "-l", "-q"], timeout=6)
        if res.returncode == 0 and res.stdout:
            lines = [line.strip().replace("\x00", "") for line in res.stdout.splitlines()]
            distros = [line for line in lines if line]
            if distros:
                return distros
    except Exception:
        pass
    return ["Ubuntu-22.04", "Ubuntu", "Debian"]


def parse_usbipd_devices(text):
    devices = []
    for line in text.splitlines():
        m = USBIPD_LINE.match(line)
        if m:
            devices.append(
                {
                    "busid": m.group("busid"),
                    "vidpid": m.group("vidpid").lower(),
                    "name": m.group("name").strip(),
                    "state": m.group("state"),
                }
            )
    return devices


def parse_candump_line(line):
    m = CANDUMP_LINE.match(line)
    if not m:
        return None
    return {
        "canid": int(m.group("canid"), 16),
        "raw_canid_str": m.group("canid"),
        "dlc": int(m.group("dlc")),
        "data": " ".join(m.group("data").split()),
    }


def parse_can_id(text):
    raw = text.strip().lower().replace("0x", "")
    if not raw or len(raw) > 8:
        raise ValueError("CAN ID 需为 1–8 位十六进制，如 123 或 18FF50E5")
    try:
        value = int(raw, 16)
    except ValueError:
        raise ValueError("CAN ID 含非法非十六进制字符")
    if value > 0x1FFFFFFF:
        raise ValueError("CAN ID 超出 29 位范围（最大 0x1FFFFFFF）")
    return value


def parse_payload(text):
    raw = re.sub(r"[\s,;:_-]+", "", text.strip())
    if not raw:
        return b""
    if len(raw) % 2:
        raise ValueError("数据载荷需为偶数个十六进制字符")
    if len(raw) > 16:
        raise ValueError("标准 CAN 帧最多支持 8 字节（16 个十六进制字符）")
    try:
        return bytes.fromhex(raw)
    except ValueError:
        raise ValueError("数据载荷包含非法非十六进制字符")


def format_payload_spaced(raw_bytes):
    return " ".join(f"{b:02X}" for b in raw_bytes)


def bytes_to_ascii(data_bytes):
    return "".join(chr(b) if 32 <= b <= 126 else "." for b in data_bytes)


def cansend_frame(can_id, data, extended, is_rtr=False, dlc=None):
    id_text = f"{can_id:08X}" if (extended or can_id > 0x7FF) else f"{can_id:03X}"
    if is_rtr:
        rtr_dlc = len(data) if dlc is None else max(0, min(8, int(dlc)))
        return f"{id_text}#R{rtr_dlc}" if rtr_dlc else f"{id_text}#R"
    return f"{id_text}#{data.hex().upper()}"


# ------------------------------------------------------------------- app ----
class CanBenchApp:
    def __init__(self, root):
        self.root = root
        self.root.title("LinSight CAN 台架控制台")
        self.root.geometry("1180x840")
        self.root.minsize(980, 680)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

        # 核心通信与任务队列
        self.q = queue.Queue()
        self._closing = False
        self._polling = False
        self._connecting = False
        self._gen_stopping = False
        self._mon_stopping = False
        self._gen_proc = None
        self._mon_proc = None

        # 报文监视与过滤状态
        self._mon_total = 0
        self._raw_frames = []  # 缓存所有接收到的帧 (最多保持 1500 条)
        self._last_id_time = {}  # 记录各 ID 上次到达时刻 (用于计算 Δt 周期)
        self._paused = False
        self._auto_scroll = True

        # 统计与设备状态
        self._prev_stats = None
        self._last_usb_state = None
        self._last_busid = None
        self._last_can_present = False
        self._can_fail_count = 0
        self._usb_poll_skip = 0
        self._base_tx_packets = 0
        self._base_rx_packets = 0

        self._load_config()
        self._init_styles()
        self._build_ui()
        self.apply_theme(self.theme_name)

        self.log("info", "系统就绪：请点击『一键连接』将 PCAN-USB 接入 WSL2")
        self.root.after(80, self._pump)
        self.root.after(600, self._poll_loop)
        self.root.after(1000, self._refresh_distros_and_devices)

    # ------------------------------------------------------------- config --
    def _load_config(self):
        cfg = {}
        try:
            if CONFIG_PATH.exists():
                cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except Exception:
            pass

        self.theme_name = cfg.get("theme", "dark")
        self.distro_var = tk.StringVar(value=cfg.get("distro", "Ubuntu-22.04"))
        self.iface_var = tk.StringVar(value=cfg.get("iface", "can0"))
        self.hwid_var = tk.StringVar(value=cfg.get("hwid", DEFAULT_HWID))
        self.bitrate_var = tk.StringVar(value=str(cfg.get("bitrate", 500000)))

        self.canid_var = tk.StringVar(value=cfg.get("canid", "123"))
        self.ext_var = tk.BooleanVar(value=bool(cfg.get("extended", False)))
        self.rtr_var = tk.BooleanVar(value=bool(cfg.get("rtr", False)))
        self.dlc_var = tk.IntVar(value=int(cfg.get("dlc", 8)))
        self.data_var = tk.StringVar(value=cfg.get("data", "11 22 33 44 55 66 77 88"))

        self.gap_var = tk.StringVar(value=str(cfg.get("gap", 20)))
        self.count_var = tk.StringVar(value=str(cfg.get("count", "500")))
        self.datamode_var = tk.StringVar(value=cfg.get("datamode", "递增"))

        self.presets = cfg.get("presets", list(DEFAULT_PRESETS))
        self._auto_scroll = cfg.get("autoscroll", True)

        self.data_var.trace_add("write", self._sync_dlc_from_data)
        self.rtr_var.trace_add("write", self._sync_dlc_from_data)

    def _save_config(self):
        cfg = {
            "theme": self.theme_name,
            "distro": self.distro_var.get(),
            "iface": self.iface_var.get(),
            "hwid": self.hwid_var.get(),
            "bitrate": self.bitrate_var.get(),
            "canid": self.canid_var.get(),
            "extended": bool(self.ext_var.get()),
            "rtr": bool(self.rtr_var.get()),
            "dlc": int(self.dlc_var.get()),
            "data": self.data_var.get(),
            "gap": self.gap_var.get(),
            "count": self.count_var.get(),
            "datamode": self.datamode_var.get(),
            "presets": self.presets,
            "autoscroll": self._auto_scroll,
        }
        try:
            CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
            CONFIG_PATH.write_text(
                json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8"
            )
        except Exception:
            pass

    # -------------------------------------------------------------- style --
    def _init_styles(self):
        self.style = ttk.Style(self.root)
        try:
            self.style.theme_use("clam")
        except tk.TclError:
            pass

        try:
            scaling = max(1.0, self.root.winfo_fpixels("1i") / 72.0)
            self.root.tk.call("tk", "scaling", scaling)
        except tk.TclError:
            pass

    def apply_theme(self, name):
        if name not in THEMES:
            name = "dark"
        self.theme_name = name
        th = THEMES[name]

        self.root.configure(bg=th["bg_app"])

        # 全局与通用组件样式
        self.style.configure(".", font=("Microsoft YaHei UI", 9), background=th["bg_app"])
        self.style.configure("TFrame", background=th["bg_app"])
        self.style.configure("Card.TFrame", background=th["bg_card"])

        # 标题与文本标签
        self.style.configure("Title.TLabel", font=("Microsoft YaHei UI", 14, "bold"),
                             foreground=th["text_main"], background=th["bg_app"])
        self.style.configure("Sub.TLabel", font=("Microsoft YaHei UI", 9),
                             foreground=th["text_sub"], background=th["bg_app"])
        self.style.configure("TLabel", foreground=th["text_main"], background=th["bg_card"])

        # 卡片容器 (LabelFrame)
        self.style.configure(
            "TLabelframe",
            background=th["bg_card"],
            bordercolor=th["border"],
            relief="solid",
            borderwidth=1,
        )
        self.style.configure(
            "TLabelframe.Label",
            font=("Microsoft YaHei UI", 9, "bold"),
            foreground=th["text_accent"],
            background=th["bg_card"],
        )

        # 按钮样式配置
        self.style.configure(
            "TButton",
            font=("Microsoft YaHei UI", 9),
            padding=(8, 4),
            background=th["btn_bg"],
            foreground=th["btn_fg"],
            bordercolor=th["border_light"],
            focuscolor="none",
        )
        self.style.map(
            "TButton",
            background=[("active", th["btn_active"]), ("disabled", th["bg_app"])],
            foreground=[("disabled", th["text_sub"])],
        )

        # 主要重点按钮 (Accent)
        self.style.configure(
            "Accent.TButton",
            font=("Microsoft YaHei UI", 9, "bold"),
            padding=(10, 4),
            background=th["accent"],
            foreground=th["accent_fg"],
            bordercolor=th["accent"],
        )
        self.style.map(
            "Accent.TButton",
            background=[("active", th["accent_hover"]), ("disabled", th["btn_bg"])],
            foreground=[("disabled", th["text_sub"])],
        )

        # 危险/停止按钮 (Danger)
        self.style.configure(
            "Danger.TButton",
            font=("Microsoft YaHei UI", 9, "bold"),
            padding=(8, 4),
            background=th["danger"],
            foreground=th["danger_fg"],
            bordercolor=th["danger"],
        )
        self.style.map(
            "Danger.TButton",
            background=[("active", th["danger_hover"]), ("disabled", th["btn_bg"])],
            foreground=[("disabled", th["text_sub"])],
        )

        # 输入框与下拉选择框
        self.style.configure(
            "TEntry",
            fieldbackground=th["bg_input"],
            foreground=th["text_main"],
            bordercolor=th["border"],
            insertcolor=th["text_main"],
            padding=4,
        )
        self.style.configure(
            "TCombobox",
            fieldbackground=th["bg_input"],
            foreground=th["text_main"],
            bordercolor=th["border"],
            padding=4,
        )
        self.style.configure(
            "TSpinbox",
            fieldbackground=th["bg_input"],
            foreground=th["text_main"],
            bordercolor=th["border"],
            padding=4,
        )

        # 选项卡 Notebook
        self.style.configure(
            "TNotebook",
            background=th["bg_card"],
            borderwidth=0,
        )
        self.style.configure(
            "TNotebook.Tab",
            font=("Microsoft YaHei UI", 9),
            background=th["btn_bg"],
            foreground=th["text_sub"],
            padding=(12, 5),
            bordercolor=th["border"],
        )
        self.style.map(
            "TNotebook.Tab",
            background=[("selected", th["bg_card"]), ("active", th["btn_active"])],
            foreground=[("selected", th["text_accent"]), ("active", th["text_main"])],
        )

        # 报文监视列表 Treeview
        self.style.configure(
            "Treeview",
            background=th["tree_bg"],
            foreground=th["tree_fg"],
            fieldbackground=th["tree_bg"],
            rowheight=24,
            font=("Consolas", 9),
            borderwidth=0,
        )
        self.style.map(
            "Treeview",
            background=[("selected", th["tree_sel"])],
            foreground=[("selected", th["tree_sel_fg"])],
        )
        self.style.configure(
            "Treeview.Heading",
            background=th["head_bg"],
            foreground=th["head_fg"],
            font=("Microsoft YaHei UI", 9, "bold"),
            relief="flat",
            padding=(4, 5),
        )
        self.style.map(
            "Treeview.Heading",
            background=[("active", th["btn_active"])],
        )

        # 细分网格与分割线
        self.style.configure("TSeparator", background=th["border"])
        self.style.configure("TPanedwindow", background=th["bg_app"])

        # 更新 Treeview 斑马线颜色
        if hasattr(self, "mon_tree"):
            self.mon_tree.tag_configure("even", background=th["tree_even"])
            self.mon_tree.tag_configure("odd", background=th["tree_odd"])
            self.mon_tree.tag_configure("ext", foreground=th["text_accent"])
            self.mon_tree.tag_configure("rtr", foreground=th["status_warn_fg"])
        if hasattr(self, "preset_tree"):
            self.preset_tree.tag_configure("even", background=th["tree_even"])
            self.preset_tree.tag_configure("odd", background=th["tree_odd"])

        # 更新系统日志文本框
        if hasattr(self, "log_text"):
            self.log_text.configure(
                bg=th["log_bg"],
                fg=th["log_fg"],
                insertbackground=th["text_main"],
            )
            self.log_text.tag_configure("info", foreground=th["text_sub"])
            self.log_text.tag_configure("ok", foreground=th["status_ok_fg"])
            self.log_text.tag_configure("warn", foreground=th["status_warn_fg"])
            self.log_text.tag_configure("err", foreground=th["status_bad_fg"])

        # 更新状态胶囊与统计看板卡片
        self._refresh_chips_appearance()
        self._refresh_stat_tiles_appearance()

        # 更新主题切换按钮文字
        if hasattr(self, "theme_btn"):
            btn_text = "☀️ 切换浅色" if name == "dark" else "🌙 切换深色"
            self.theme_btn.configure(text=btn_text)

    def toggle_theme(self):
        new_theme = "light" if self.theme_name == "dark" else "dark"
        self.apply_theme(new_theme)

    # ---------------------------------------------------------------- ui ---
    def _build_ui(self):
        # 顶层总容器
        self.outer = ttk.Frame(self.root, padding=10)
        self.outer.pack(fill="both", expand=True)

        # 1. 顶部 Header 状态总览
        self._build_header(self.outer)

        # 2. 中间主体：水平双栏分屏 (PanedWindow 可拖动调整宽度)
        self.paned_h = ttk.PanedWindow(self.outer, orient="horizontal")
        self.paned_h.pack(fill="both", expand=True, pady=(8, 6))

        # 左栏：配置与发送控制台 (Control Deck)
        left_container = ttk.Frame(self.paned_h, width=430)
        self.paned_h.add(left_container, weight=0)
        self._build_left_deck(left_container)

        # 右栏：报文监视与系统日志 (Traffic & Logs)
        right_container = ttk.Frame(self.paned_h)
        self.paned_h.add(right_container, weight=1)
        self._build_right_deck(right_container)

        # 3. 底部快捷提示栏
        self._build_footer(self.outer)

    def _build_header(self, parent):
        head = ttk.Frame(parent)
        head.pack(fill="x", pady=(0, 4))

        # 左侧标题与简要描述
        title_box = ttk.Frame(head)
        title_box.pack(side="left")
        ttk.Label(title_box, text="LinSight CAN 台架控制台", style="Title.TLabel").pack(
            side="left"
        )
        ttk.Label(
            title_box,
            text="PCAN-USB ⇌ WSL2 SocketCAN",
            style="Sub.TLabel",
        ).pack(side="left", padx=(10, 0), pady=(3, 0))

        # 右侧操作区：主题切换 & 设备刷新
        action_box = ttk.Frame(head)
        action_box.pack(side="right")
        self.theme_btn = ttk.Button(action_box, text="☀️ 切换浅色", command=self.toggle_theme)
        self.theme_btn.pack(side="right", padx=(6, 0))
        self.refresh_btn = ttk.Button(
            action_box, text="🔄 刷新设备", command=self._refresh_distros_and_devices
        )
        self.refresh_btn.pack(side="right")

        # 中间 4 个状态胶囊标签 (USB, WSL, CAN, Bus State)
        self.chips_frame = ttk.Frame(parent)
        self.chips_frame.pack(fill="x", pady=(4, 4))
        for i in range(4):
            self.chips_frame.columnconfigure(i, weight=1, uniform="chip")

        self.chip_usb = tk.Label(self.chips_frame, text="● USB · 未检测", anchor="center")
        self.chip_wsl = tk.Label(self.chips_frame, text="● WSL · 未连接", anchor="center")
        self.chip_can = tk.Label(self.chips_frame, text="● CAN · 未启动", anchor="center")
        self.chip_bus = tk.Label(self.chips_frame, text="● 总线 · 待机", anchor="center")

        self.chip_usb.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.chip_wsl.grid(row=0, column=1, sticky="ew", padx=4)
        self.chip_can.grid(row=0, column=2, sticky="ew", padx=4)
        self.chip_bus.grid(row=0, column=3, sticky="ew", padx=(4, 0))

    def _build_left_deck(self, parent):
        # 允许左栏垂直滚动以适配笔记本小屏幕
        canvas = tk.Canvas(parent, borderwidth=0, highlightthickness=0)
        vbar = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
        scroll_content = ttk.Frame(canvas)

        scroll_content.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all")),
        )
        win_id = canvas.create_window((0, 0), window=scroll_content, anchor="nw")
        canvas.configure(yscrollcommand=vbar.set)

        def _on_canvas_configure(event):
            canvas.itemconfig(win_id, width=event.width)

        canvas.bind("<Configure>", _on_canvas_configure)

        def _on_mousewheel(event):
            w = event.widget
            while w is not None:
                if w is parent:
                    if scroll_content.winfo_reqheight() > canvas.winfo_height():
                        canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
                    break
                w = getattr(w, "master", None)

        canvas.bind_all("<MouseWheel>", _on_mousewheel, add="+")

        canvas.pack(side="left", fill="both", expand=True)
        vbar.pack(side="right", fill="y")

        # ---- 卡片 1: 硬件与连接设置 ----
        conn = ttk.LabelFrame(scroll_content, text=" 硬件与链路配置 ", padding=10)
        conn.pack(fill="x", pady=(0, 8), padx=2)
        conn.columnconfigure(1, weight=1)
        conn.columnconfigure(3, weight=1)

        ttk.Label(conn, text="WSL 发行版:").grid(row=0, column=0, sticky="w", pady=3)
        self.distro_box = ttk.Combobox(
            conn, textvariable=self.distro_var, values=list_wsl_distros()
        )
        self.distro_box.grid(row=0, column=1, sticky="ew", padx=(6, 10), pady=3)

        ttk.Label(conn, text="CAN 接口:").grid(row=0, column=2, sticky="w", pady=3)
        self.iface_box = ttk.Combobox(
            conn, textvariable=self.iface_var, width=8, values=("can0", "can1", "vcan0")
        )
        self.iface_box.grid(row=0, column=3, sticky="ew", padx=(6, 0), pady=3)

        ttk.Label(conn, text="总线比特率:").grid(row=1, column=0, sticky="w", pady=3)
        self.bitrate_box = ttk.Combobox(
            conn,
            textvariable=self.bitrate_var,
            values=("1000000", "800000", "500000", "250000", "125000", "50000", "20000"),
        )
        self.bitrate_box.grid(row=1, column=1, sticky="ew", padx=(6, 10), pady=3)

        ttk.Label(conn, text="硬件 VID:PID:").grid(row=1, column=2, sticky="w", pady=3)
        self.hwid_entry = ttk.Entry(conn, textvariable=self.hwid_var, width=11)
        self.hwid_entry.grid(row=1, column=3, sticky="ew", padx=(6, 0), pady=3)

        # 操作按钮网格
        btns_grid = ttk.Frame(conn)
        btns_grid.grid(row=2, column=0, columnspan=4, sticky="ew", pady=(10, 2))
        for col in range(4):
            btns_grid.columnconfigure(col, weight=1)

        self.connect_btn = ttk.Button(
            btns_grid,
            text="⚡ 一键连接",
            style="Accent.TButton",
            command=lambda: self.do_connect(False),
        )
        self.connect_btn.grid(row=0, column=0, columnspan=2, sticky="ew", padx=(0, 4), pady=2)

        self.apply_btn = ttk.Button(
            btns_grid, text="🎯 应用速率", command=self.do_apply_bitrate
        )
        self.apply_btn.grid(row=0, column=2, sticky="ew", padx=2, pady=2)

        self.disconnect_btn = ttk.Button(
            btns_grid, text="🔌 断开", command=self.do_disconnect
        )
        self.disconnect_btn.grid(row=0, column=3, sticky="ew", padx=(4, 0), pady=2)

        self.reattach_btn = ttk.Button(
            btns_grid, text="🔄 强制重挂", command=lambda: self.do_connect(True)
        )
        self.reattach_btn.grid(row=1, column=0, columnspan=2, sticky="ew", padx=(0, 4), pady=2)

        self.restart_wsl_btn = ttk.Button(
            btns_grid, text="🛑 重启 WSL", command=self.do_restart_wsl
        )
        self.restart_wsl_btn.grid(row=1, column=2, columnspan=2, sticky="ew", padx=(2, 0), pady=2)

        ttk.Label(
            conn,
            text="* 首次或更换 USB 口连接时会弹出一次 UAC 管理员提权进行 bind",
            style="Sub.TLabel",
        ).grid(row=3, column=0, columnspan=4, sticky="w", pady=(6, 0))

        # ---- 卡片 2: 发送控制台 (Notebook 选项卡) ----
        send_frame = ttk.LabelFrame(scroll_content, text=" 报文发送控制台 ", padding=8)
        send_frame.pack(fill="x", pady=(0, 8), padx=2)

        self.send_notebook = ttk.Notebook(send_frame)
        self.send_notebook.pack(fill="x", expand=True)

        # Tab 1: 单帧发送与预设
        tab_single = ttk.Frame(self.send_notebook, padding=8)
        self.send_notebook.add(tab_single, text="  单帧发送与预设  ")
        self._build_single_send_tab(tab_single)

        # Tab 2: 连续/周期生成器
        tab_cangen = ttk.Frame(self.send_notebook, padding=8)
        self.send_notebook.add(tab_cangen, text="  连续生成器 (cangen)  ")
        self._build_cangen_tab(tab_cangen)

        # ---- 卡片 3: 总线状态与统计看板 ----
        stat_card = ttk.LabelFrame(scroll_content, text=" 总线健康与统计看板 ", padding=10)
        stat_card.pack(fill="x", pady=(0, 8), padx=2)
        self._build_stat_card(stat_card)

    def _build_single_send_tab(self, parent):
        parent.columnconfigure(1, weight=1)

        # CAN ID 与类型
        id_row = ttk.Frame(parent)
        id_row.pack(fill="x", pady=(0, 6))
        ttk.Label(id_row, text="CAN ID:").pack(side="left")
        self.id_entry = ttk.Entry(id_row, textvariable=self.canid_var, width=12)
        self.id_entry.pack(side="left", padx=(6, 12))
        self.id_entry.bind("<Return>", lambda e: self.do_send_single())

        ttk.Checkbutton(id_row, text="扩展帧 (29-bit)", variable=self.ext_var).pack(
            side="left", padx=(0, 8)
        )
        ttk.Checkbutton(id_row, text="远程帧 (RTR)", variable=self.rtr_var).pack(
            side="left"
        )

        # 数据载荷
        data_row = ttk.Frame(parent)
        data_row.pack(fill="x", pady=(0, 6))
        ttk.Label(data_row, text="数据 (Hex):").pack(side="left")
        self.data_entry = ttk.Entry(data_row, textvariable=self.data_var)
        self.data_entry.pack(side="left", fill="x", expand=True, padx=(6, 6))
        self.data_entry.bind("<Return>", lambda e: self.do_send_single())

        ttk.Button(data_row, text="格式化", width=6, command=self._format_data_input).pack(
            side="left"
        )

        # 发送单帧操作行
        ctrl_row = ttk.Frame(parent)
        ctrl_row.pack(fill="x", pady=(2, 8))
        self.dlc_label_var = tk.StringVar(value="DLC: 8 字节")
        ttk.Label(ctrl_row, textvariable=self.dlc_label_var, style="Sub.TLabel").pack(
            side="left"
        )

        self.send_btn = ttk.Button(
            ctrl_row,
            text="🚀 发送单帧 (Enter)",
            style="Accent.TButton",
            command=self.do_send_single,
        )
        self.send_btn.pack(side="right")

        # 常用预设列表 (Presets)
        ttk.Separator(parent, orient="horizontal").pack(fill="x", pady=(4, 6))
        preset_title_row = ttk.Frame(parent)
        preset_title_row.pack(fill="x", pady=(0, 4))
        ttk.Label(preset_title_row, text="常用预设报文:", font=("Microsoft YaHei UI", 9, "bold")).pack(
            side="left"
        )

        # 预设表格
        p_frame = ttk.Frame(parent)
        p_frame.pack(fill="x", pady=(0, 6))
        self.preset_tree = ttk.Treeview(
            p_frame,
            columns=("name", "id", "ext", "data"),
            show="headings",
            height=4,
        )
        self.preset_tree.heading("name", text="名称")
        self.preset_tree.heading("id", text="ID")
        self.preset_tree.heading("ext", text="类型")
        self.preset_tree.heading("data", text="数据载荷")

        self.preset_tree.column("name", width=95, anchor="w")
        self.preset_tree.column("id", width=65, anchor="center")
        self.preset_tree.column("ext", width=55, anchor="center")
        self.preset_tree.column("data", width=160, anchor="w")
        self.preset_tree.pack(side="left", fill="x", expand=True)

        p_scroll = ttk.Scrollbar(
            p_frame, orient="vertical", command=self.preset_tree.yview
        )
        p_scroll.pack(side="right", fill="y")
        self.preset_tree.configure(yscrollcommand=p_scroll.set)
        self.preset_tree.bind("<Double-1>", lambda e: self._load_selected_preset())

        # 预设管理按钮
        p_btns = ttk.Frame(parent)
        p_btns.pack(fill="x")
        ttk.Button(p_btns, text="🚀 发送选中", command=self._send_selected_preset).pack(
            side="left", padx=(0, 4)
        )
        ttk.Button(p_btns, text="📥 载入", command=self._load_selected_preset).pack(
            side="left", padx=4
        )
        ttk.Button(p_btns, text="➕ 保存当前", command=self._add_current_as_preset).pack(
            side="left", padx=4
        )
        ttk.Button(p_btns, text="🗑 删除", command=self._delete_selected_preset).pack(
            side="right"
        )

        self._refresh_presets_table()

    def _build_cangen_tab(self, parent):
        grid = ttk.Frame(parent)
        grid.pack(fill="x", pady=(0, 8))
        grid.columnconfigure(1, weight=1)
        grid.columnconfigure(3, weight=1)

        ttk.Label(grid, text="发送间隔 (ms):").grid(row=0, column=0, sticky="w", pady=4)
        ttk.Spinbox(grid, textvariable=self.gap_var, from_=1, to=60000, width=8).grid(
            row=0, column=1, sticky="w", padx=(6, 12), pady=4
        )

        ttk.Label(grid, text="发送帧数:").grid(row=0, column=2, sticky="w", pady=4)
        ttk.Spinbox(
            grid, textvariable=self.count_var, from_=-1, to=10000000, width=9
        ).grid(row=0, column=3, sticky="w", padx=(6, 0), pady=4)

        ttk.Label(grid, text="数据模式:").grid(row=1, column=0, sticky="w", pady=4)
        ttk.Combobox(
            grid,
            textvariable=self.datamode_var,
            width=8,
            values=("递增", "固定", "随机"),
            state="readonly",
        ).grid(row=1, column=1, sticky="w", padx=(6, 12), pady=4)

        ttk.Label(
            grid, text="* -1 表示无限循环持续发送", style="Sub.TLabel"
        ).grid(row=1, column=2, columnspan=2, sticky="w", pady=4)

        gen_ctrl = ttk.Frame(parent)
        gen_ctrl.pack(fill="x", pady=(8, 4))
        self.gen_start_btn = ttk.Button(
            gen_ctrl,
            text="▶ 开始连续发送",
            style="Accent.TButton",
            command=self.do_start_gen,
        )
        self.gen_start_btn.pack(side="left")

        self.gen_stop_btn = ttk.Button(
            gen_ctrl,
            text="⏹ 停止发送",
            style="Danger.TButton",
            command=self.do_stop_gen,
            state="disabled",
        )
        self.gen_stop_btn.pack(side="left", padx=(8, 0))

        self.gen_state_var = tk.StringVar(value="")
        ttk.Label(gen_ctrl, textvariable=self.gen_state_var, style="Sub.TLabel").pack(
            side="left", padx=(12, 0)
        )

    def _build_stat_card(self, parent):
        # 统计看板卡片网格 (2x2 指标看板)
        tiles = ttk.Frame(parent)
        tiles.pack(fill="x", pady=(0, 8))
        for col in range(2):
            tiles.columnconfigure(col, weight=1, uniform="stat")

        # TX 指标瓦片
        self.tx_tile = tk.Frame(tiles, padx=10, pady=8, relief="solid", borderwidth=1)
        self.tx_tile.grid(row=0, column=0, sticky="nsew", padx=(0, 4), pady=2)
        tk.Label(
            self.tx_tile,
            text="发送统计 (TX)",
            font=("Microsoft YaHei UI", 8),
            name="tx_title",
        ).pack(anchor="w")
        self.tx_total_lbl = tk.Label(
            self.tx_tile,
            text="0",
            font=("Segoe UI", 15, "bold"),
            name="tx_val",
        )
        self.tx_total_lbl.pack(anchor="w")
        self.tx_rate_lbl = tk.Label(
            self.tx_tile,
            text="0 帧/s",
            font=("Consolas", 9),
            name="tx_rate",
        )
        self.tx_rate_lbl.pack(anchor="w")

        # RX 指标瓦片
        self.rx_tile = tk.Frame(tiles, padx=10, pady=8, relief="solid", borderwidth=1)
        self.rx_tile.grid(row=0, column=1, sticky="nsew", padx=(4, 0), pady=2)
        tk.Label(
            self.rx_tile,
            text="接收统计 (RX)",
            font=("Microsoft YaHei UI", 8),
            name="rx_title",
        ).pack(anchor="w")
        self.rx_total_lbl = tk.Label(
            self.rx_tile,
            text="0",
            font=("Segoe UI", 15, "bold"),
            name="rx_val",
        )
        self.rx_total_lbl.pack(anchor="w")
        self.rx_rate_lbl = tk.Label(
            self.rx_tile,
            text="0 帧/s",
            font=("Consolas", 9),
            name="rx_rate",
        )
        self.rx_rate_lbl.pack(anchor="w")

        # 详细错误与重启状态行
        info_row = ttk.Frame(parent)
        info_row.pack(fill="x", pady=(4, 0))
        self.err_var = tk.StringVar(value="错误: TX 0 / RX 0  |  Bus-Off: 0  |  重启: 0")
        ttk.Label(info_row, textvariable=self.err_var, style="Sub.TLabel").pack(side="left")

        ttk.Button(info_row, text="↺ 清零统计", command=self._reset_stats).pack(
            side="right"
        )

    def _build_right_deck(self, parent):
        # 垂直分割：上方总线报文监视器，下方系统日志控制台
        paned_v = ttk.PanedWindow(parent, orient="vertical")
        paned_v.pack(fill="both", expand=True)

        # ---- 上半部分: 总线监听器 ----
        mon_box = ttk.LabelFrame(paned_v, text=" 实时总线报文监视器 ", padding=8)
        paned_v.add(mon_box, weight=3)
        mon_box.rowconfigure(1, weight=1)
        mon_box.columnconfigure(0, weight=1)

        # 监视器控制工具栏
        mon_bar = ttk.Frame(mon_box)
        mon_bar.grid(row=0, column=0, sticky="ew", pady=(0, 6))

        self.mon_start_btn = ttk.Button(
            mon_bar,
            text="▶ 开始监听",
            style="Accent.TButton",
            command=self.do_start_monitor,
        )
        self.mon_start_btn.pack(side="left")

        self.mon_stop_btn = ttk.Button(
            mon_bar,
            text="⏹ 停止",
            style="Danger.TButton",
            command=self.do_stop_monitor,
            state="disabled",
        )
        self.mon_stop_btn.pack(side="left", padx=(6, 0))

        self.mon_pause_btn = ttk.Button(
            mon_bar, text="⏸ 暂停滚屏", command=self._toggle_pause
        )
        self.mon_pause_btn.pack(side="left", padx=(6, 0))

        self.mon_clear_btn = ttk.Button(
            mon_bar, text="🗑 清空", command=self._clear_monitor
        )
        self.mon_clear_btn.pack(side="left", padx=(6, 12))

        # 实时过滤器与搜索框
        ttk.Label(mon_bar, text="🔍 过滤 ID/数据:").pack(side="left")
        self.filter_var = tk.StringVar()
        self.filter_entry = ttk.Entry(mon_bar, textvariable=self.filter_var, width=14)
        self.filter_entry.pack(side="left", padx=(4, 4))
        self.filter_var.trace_add("write", self._on_filter_changed)

        ttk.Button(mon_bar, text="✖", width=3, command=lambda: self.filter_var.set("")).pack(
            side="left"
        )

        # 自动滚屏勾选框
        self.autoscroll_var = tk.BooleanVar(value=self._auto_scroll)
        ttk.Checkbutton(
            mon_bar,
            text="自动滚屏",
            variable=self.autoscroll_var,
            command=self._on_autoscroll_toggled,
        ).pack(side="left", padx=(10, 0))

        # 导出报文按钮
        self.export_btn = ttk.Button(mon_bar, text="💾 导出 CSV", command=self._export_traffic_csv)
        self.export_btn.pack(side="right")

        self.mon_count_var = tk.StringVar(value="已捕获 0 帧")
        ttk.Label(mon_bar, textvariable=self.mon_count_var, style="Sub.TLabel").pack(
            side="right", padx=(0, 10)
        )

        # 报文展示树状列表 Treeview
        tree_wrap = ttk.Frame(mon_box)
        tree_wrap.grid(row=1, column=0, sticky="nsew")
        tree_wrap.columnconfigure(0, weight=1)
        tree_wrap.rowconfigure(0, weight=1)

        self.mon_tree = ttk.Treeview(
            tree_wrap,
            columns=("idx", "time", "delta", "canid", "type", "dlc", "data", "ascii"),
            show="headings",
            selectmode="extended",
        )
        self.mon_tree.heading("idx", text="#")
        self.mon_tree.heading("time", text="时间戳")
        self.mon_tree.heading("delta", text="Δt (ms)")
        self.mon_tree.heading("canid", text="CAN ID")
        self.mon_tree.heading("type", text="类型")
        self.mon_tree.heading("dlc", text="DLC")
        self.mon_tree.heading("data", text="数据载荷 (Hex)")
        self.mon_tree.heading("ascii", text="ASCII")

        self.mon_tree.column("idx", width=50, minwidth=40, anchor="center")
        self.mon_tree.column("time", width=105, minwidth=90, anchor="center")
        self.mon_tree.column("delta", width=75, minwidth=60, anchor="center")
        self.mon_tree.column("canid", width=95, minwidth=80, anchor="center")
        self.mon_tree.column("type", width=65, minwidth=50, anchor="center")
        self.mon_tree.column("dlc", width=45, minwidth=35, anchor="center")
        self.mon_tree.column("data", width=220, minwidth=140, anchor="w")
        self.mon_tree.column("ascii", width=90, minwidth=60, anchor="w")

        self.mon_tree.grid(row=0, column=0, sticky="nsew")

        yscroll = ttk.Scrollbar(tree_wrap, orient="vertical", command=self.mon_tree.yview)
        yscroll.grid(row=0, column=1, sticky="ns")
        xscroll = ttk.Scrollbar(tree_wrap, orient="horizontal", command=self.mon_tree.xview)
        xscroll.grid(row=1, column=0, sticky="ew")
        self.mon_tree.configure(yscrollcommand=yscroll.set, xscrollcommand=xscroll.set)

        # 报文列表右键快捷菜单
        self._build_tree_context_menu()

        # ---- 下半部分: 系统运行日志 ----
        log_box = ttk.LabelFrame(paned_v, text=" 系统运行日志 ", padding=6)
        paned_v.add(log_box, weight=1)
        log_box.rowconfigure(1, weight=1)
        log_box.columnconfigure(0, weight=1)

        log_head = ttk.Frame(log_box)
        log_head.grid(row=0, column=0, sticky="ew", pady=(0, 4))
        ttk.Label(log_head, text="Console Output:", style="Sub.TLabel").pack(side="left")

        ttk.Button(log_head, text="📋 复制全部", command=self._copy_log).pack(
            side="right"
        )
        ttk.Button(log_head, text="🗑 清空日志", command=self._clear_log).pack(
            side="right", padx=(0, 4)
        )

        self.log_text = ScrolledText(
            log_box,
            height=6,
            wrap="word",
            state="disabled",
            font=("Consolas", 9),
            relief="flat",
        )
        self.log_text.grid(row=1, column=0, sticky="nsew")

    def _build_footer(self, parent):
        foot = ttk.Frame(parent)
        foot.pack(fill="x", pady=(4, 0))
        ttk.Label(
            foot,
            text="💡 提示：LinSight 固件默认处于 LISTEN_ONLY 模式（不应答 ACK）；"
                 "双节点收发联调请在设备 SETUP 页面（或 menuconfig）切换为 NORMAL 模式。",
            style="Sub.TLabel",
        ).pack(side="left")

    def _build_tree_context_menu(self):
        self.tree_menu = tk.Menu(self.root, tearoff=0)
        self.tree_menu.add_command(label="📋 复制 CAN ID", command=self._copy_selected_id)
        self.tree_menu.add_command(
            label="📋 复制数据 (Hex)", command=self._copy_selected_data
        )
        self.tree_menu.add_command(
            label="📋 复制整行文本", command=self._copy_selected_row
        )
        self.tree_menu.add_separator()
        self.tree_menu.add_command(
            label="📥 填入单帧发送区", command=self._load_selected_into_sender
        )
        self.tree_menu.add_command(
            label="⭐ 添加为快捷预设", command=self._save_selected_as_preset
        )

        self.mon_tree.bind("<Button-3>", self._on_tree_right_click)

    def _on_tree_right_click(self, event):
        item = self.mon_tree.identify_row(event.y)
        if item:
            self.mon_tree.selection_set(item)
            self.tree_menu.post(event.x_root, event.y_root)

    # -------------------------------------------------------- chips & tiles -
    def _refresh_chips_appearance(self):
        th = THEMES[self.theme_name]
        for chip in (self.chip_usb, self.chip_wsl, self.chip_can, self.chip_bus):
            chip.configure(
                font=("Microsoft YaHei UI", 9, "bold"),
                padx=8,
                pady=5,
                relief="flat",
            )
        # 根据最后记录的状态恢复配色
        self._apply_usb_state(getattr(self, "_last_dev_obj", None))

    def _refresh_stat_tiles_appearance(self):
        th = THEMES[self.theme_name]
        for tile in (self.tx_tile, self.rx_tile):
            tile.configure(bg=th["bg_card_inner"], highlightbackground=th["border"])
            for child in tile.winfo_children():
                name = child.winfo_name()
                if "title" in name:
                    child.configure(bg=th["bg_card_inner"], fg=th["text_sub"])
                elif "val" in name:
                    child.configure(bg=th["bg_card_inner"], fg=th["text_main"])
                elif "rate" in name:
                    child.configure(bg=th["bg_card_inner"], fg=th["text_accent"])

    def _set_chip(self, chip, text, level):
        th = THEMES[self.theme_name]
        colors = {
            "ok": (th["status_ok_bg"], th["status_ok_fg"]),
            "warn": (th["status_warn_bg"], th["status_warn_fg"]),
            "bad": (th["status_bad_bg"], th["status_bad_fg"]),
            "idle": (th["status_idle_bg"], th["status_idle_fg"]),
        }
        bg, fg = colors.get(level, colors["idle"])
        chip.configure(text=text, bg=bg, fg=fg)

    # ------------------------------------------------------------- presets --
    def _refresh_presets_table(self):
        for item in self.preset_tree.get_children():
            self.preset_tree.delete(item)
        for i, p in enumerate(self.presets):
            tag = "even" if i % 2 == 0 else "odd"
            if p.get("rtr"):
                type_str = "远程"
            else:
                type_str = "扩展" if p.get("ext") else "标准"
            data_disp = f"RTR (DLC={p.get('dlc', 0)})" if p.get("rtr") else p.get("data", "")
            self.preset_tree.insert(
                "",
                "end",
                values=(p.get("name", "预设"), p.get("id", "123"), type_str, data_disp),
                tags=(tag,),
            )

    def _load_selected_preset(self):
        sel = self.preset_tree.selection()
        if not sel:
            return False
        idx = self.preset_tree.index(sel[0])
        if 0 <= idx < len(self.presets):
            p = self.presets[idx]
            self.canid_var.set(p.get("id", "123"))
            self.ext_var.set(bool(p.get("ext", False)))
            is_rtr = bool(p.get("rtr", False))
            self.rtr_var.set(is_rtr)
            if is_rtr:
                self.dlc_var.set(int(p.get("dlc", 0)))
                self.data_var.set("")
            else:
                self.data_var.set(p.get("data", ""))
            self.log("info", f"已载入预设报文 [{p.get('name')}]")
            return True
        return False

    def _send_selected_preset(self):
        if self._load_selected_preset():
            self.do_send_single()
        else:
            self.log("warn", "请先在预设列表中选中一条报文")

    def _add_current_as_preset(self):
        try:
            can_id, data, extended, is_rtr = self._frame_inputs()
        except ValueError as exc:
            messagebox.showerror("参数错误", str(exc), parent=self.root)
            return

        name = simpledialog.askstring(
            "保存预设", "请输入预设报文名称:", initialvalue="自定义测试帧", parent=self.root
        )
        if not name:
            return
        id_str = f"{can_id:08X}" if extended else f"{can_id:03X}"
        dlc = int(self.dlc_var.get()) if is_rtr else len(data)
        self.presets.append(
            {
                "name": name.strip(),
                "id": id_str,
                "ext": extended,
                "rtr": is_rtr,
                "dlc": dlc,
                "data": "" if is_rtr else format_payload_spaced(data),
            }
        )
        self._refresh_presets_table()
        self._save_config()
        self.log("ok", f"已新增预设 [{name}] (ID: 0x{id_str})")

    def _delete_selected_preset(self):
        sel = self.preset_tree.selection()
        if not sel:
            return
        idx = self.preset_tree.index(sel[0])
        if 0 <= idx < len(self.presets):
            deleted = self.presets.pop(idx)
            self._refresh_presets_table()
            self._save_config()
            self.log("info", f"已删除预设 [{deleted.get('name')}]")

    # --------------------------------------------------------- format & ui --
    def _sync_dlc_from_data(self, *_args):
        if self.rtr_var.get():
            try:
                data = parse_payload(self.data_var.get())
                dlc = len(data) if data else int(self.dlc_var.get())
            except ValueError:
                dlc = int(self.dlc_var.get())
            dlc = max(0, min(8, dlc))
            self.dlc_var.set(dlc)
            self.dlc_label_var.set(f"DLC: {dlc} 字节 (RTR 远程帧)")
            return
        try:
            data = parse_payload(self.data_var.get())
            self.dlc_var.set(len(data))
            self.dlc_label_var.set(f"DLC: {len(data)} 字节 ({len(data) * 2} 字符)")
        except ValueError:
            self.dlc_label_var.set("DLC: 格式错误")

    def _format_data_input(self):
        try:
            data = parse_payload(self.data_var.get())
            self.data_var.set(format_payload_spaced(data))
        except ValueError as exc:
            messagebox.showwarning("格式化提示", str(exc), parent=self.root)

    def _on_autoscroll_toggled(self):
        self._auto_scroll = self.autoscroll_var.get()
        self._save_config()

    # ------------------------------------------------------------- queue ---
    def post(self, kind, payload=None):
        self.q.put((kind, payload))

    def log(self, level, message):
        self.post("log", (level, message))

    def _pump(self):
        if self._closing:
            return
        try:
            for _ in range(400):
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
            try:
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
            new_frames, total = payload
            self._raw_frames.extend(new_frames)
            if len(self._raw_frames) > 1500:
                self._raw_frames = self._raw_frames[-1200:]

            if not self._paused:
                filt = self.filter_var.get().strip().lower()
                for f in new_frames:
                    if self._matches_filter(f, filt):
                        self._insert_frame_row(f)

                # 保留 Treeview 中最多 400 条渲染条目以维持极致性能
                children = self.mon_tree.get_children()
                if len(children) > 400:
                    for item in children[:-400]:
                        self.mon_tree.delete(item)

                if self._auto_scroll and children:
                    self.mon_tree.yview_moveto(1.0)

            self.mon_count_var.set(f"已捕获 {total} 帧")

        elif kind == "mon_started":
            self.mon_start_btn.configure(state="disabled")
            self.mon_stop_btn.configure(state="normal")

        elif kind == "mon_stopped":
            self.mon_start_btn.configure(state="normal")
            self.mon_stop_btn.configure(state="disabled")
            if payload not in (None, 0) and not self._mon_stopping and not self._closing:
                self.log("warn", f"candump 已退出（code={payload}）")

        elif kind == "gen_started":
            self.gen_start_btn.configure(state="disabled")
            self.gen_stop_btn.configure(state="normal")
            self.gen_state_var.set("● 正在持续发送...")

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

        elif kind == "distros_updated":
            distros = payload
            current = self.distro_var.get()
            self.distro_box["values"] = distros
            if current not in distros and distros:
                self.distro_var.set(distros[0])

        elif kind == "poll_now":
            self._trigger_poll(force_usb=True)

    def _set_conn_enabled(self, enabled):
        state = "normal" if enabled else "disabled"
        for btn in (
            self.connect_btn,
            self.apply_btn,
            self.reattach_btn,
            self.disconnect_btn,
            self.restart_wsl_btn,
            self.refresh_btn,
        ):
            btn.configure(state=state)

    # ------------------------------------------------------- state & poll ---
    def _wsl_exec(self, argv, timeout=15):
        """直接执行 WSL 二进制程序（跳过 bash 启动开销）。"""
        return run_cmd(
            ["wsl", "-d", self.distro_var.get().strip(), "-u", "root", "--exec", *argv],
            timeout,
        )

    def _wsl_root(self, script, timeout=25):
        return self._wsl_exec(["bash", "-c", script], timeout)

    def _find_device(self):
        usbipd = find_usbipd()
        if not usbipd:
            return None
        result = run_cmd([usbipd, "list"], 20)
        hwid = self.hwid_var.get().strip().lower()
        devs = parse_usbipd_devices(result.stdout)
        for dev in devs:
            if dev["vidpid"] == hwid:
                return dev
        for dev in devs:
            if "pcan" in dev["name"].lower() or "can" in dev["name"].lower():
                return dev
        return None

    def _refresh_distros_and_devices(self):
        threading.Thread(target=self._refresh_worker, daemon=True).start()

    def _refresh_worker(self):
        distros = list_wsl_distros()
        self.post("distros_updated", distros)
        dev = self._find_device()
        self.post("usb", dev)
        if dev:
            self.log("ok", f"检测到设备: {dev['name']} (BUSID={dev['busid']}) [{dev['state']}]")

    def _can_stats(self):
        iface = self.iface_var.get().strip() or "can0"
        try:
            result = self._wsl_exec(["ip", "-j", "-d", "-s", "link", "show", iface], 10)
        except Exception:
            return None
        if result and result.returncode == 0 and result.stdout.strip():
            try:
                return self._parse_can_stats(json.loads(result.stdout)[0])
            except Exception:
                pass
        return None

    @staticmethod
    def _parse_can_stats(data):
        info = (data.get("linkinfo") or {}).get("info_data") or {}
        timings = info.get("bittiming") or {}
        xstats = (data.get("linkinfo") or {}).get("info_xstats") or {}
        stats = data.get("stats64") or data.get("stats") or {}
        rx = stats.get("rx") or {}
        tx = stats.get("tx") or {}
        berr = info.get("berr_counter") or {}
        return {
            "up": data.get("operstate") == "UP",
            "operstate": data.get("operstate", "?"),
            "state": info.get("state", "UNKNOWN"),
            "bitrate": timings.get("bitrate"),
            "sample_point": timings.get("sample_point"),
            "tx_packets": tx.get("packets", 0),
            "rx_packets": rx.get("packets", 0),
            "tx_errors": tx.get("errors", 0),
            "rx_errors": rx.get("errors", 0),
            "tec": berr.get("tx", 0),
            "rec": berr.get("rx", 0),
            "restarts": xstats.get("restarts", 0),
            "bus_off": xstats.get("bus_off", 0),
        }

    def _apply_usb_state(self, dev):
        self._last_dev_obj = dev
        state = dev["state"] if dev else None
        busid = dev["busid"] if dev else None
        if state != self._last_usb_state or busid != self._last_busid:
            if dev:
                self.log("info", f"USB 状态更新：{dev['name']} {dev['busid']} → {state}")
            self._last_usb_state = state
            self._last_busid = busid

        distro = self.distro_var.get()
        if not dev:
            self._set_chip(self.chip_usb, "● USB · 未检测到 PCAN", "bad")
            self._set_chip(self.chip_wsl, f"● WSL · {distro} (待机)", "idle")
            return

        if state == "Attached":
            self._set_chip(self.chip_usb, f"● USB · {dev['busid']} 已挂载", "ok")
            self._set_chip(self.chip_wsl, f"● WSL · {distro} (在线)", "ok")
        elif state == "Shared":
            self._set_chip(self.chip_usb, f"● USB · {dev['busid']} 已共享", "warn")
            self._set_chip(self.chip_wsl, f"● WSL · {distro} (未挂载)", "idle")
        else:
            self._set_chip(self.chip_usb, f"● USB · {dev['busid']} 未共享", "idle")
            self._set_chip(self.chip_wsl, f"● WSL · {distro} (未连接)", "idle")

    def _apply_can_stats(self, stats):
        iface = self.iface_var.get().strip() or "can0"
        present = stats is not None
        if present != self._last_can_present:
            self._last_can_present = present
            if present:
                self.log("ok", f"检测到 {iface} 接口已成功启动")
            elif self._last_usb_state == "Attached":
                self.log("warn", f"{iface} 接口离线：USB 可能断开或被重新插拔")

        if not stats:
            self._set_chip(self.chip_can, f"● CAN · {iface} 未启动", "idle")
            self._set_chip(self.chip_bus, "● 总线 · 离线", "idle")
            self.tx_total_lbl.configure(text="0")
            self.tx_rate_lbl.configure(text="0 帧/s")
            self.rx_total_lbl.configure(text="0")
            self.rx_rate_lbl.configure(text="0 帧/s")
            self.err_var.set("错误: TX 0 / RX 0  |  Bus-Off: 0  |  重启: 0")
            self._prev_stats = None
            return

        rate = stats["bitrate"]
        rate_text = f"{rate // 1000}k" if rate else "?"
        if stats["up"]:
            self._set_chip(self.chip_can, f"● CAN · {iface} {rate_text} UP", "ok")
        else:
            self._set_chip(self.chip_can, f"● CAN · {iface} DOWN", "warn")

        bus_state = stats["state"]
        if bus_state == "ERROR-ACTIVE":
            self._set_chip(self.chip_bus, "● 总线 · 正常 (ACTIVE)", "ok")
        elif bus_state == "ERROR-PASSIVE":
            self._set_chip(self.chip_bus, "● 总线 · 被动错误 (PASSIVE)", "warn")
        elif bus_state == "BUS-OFF":
            self._set_chip(self.chip_bus, "● 总线 · 关闭 (BUS-OFF)", "bad")
        else:
            self._set_chip(self.chip_bus, f"● 总线 · {bus_state}", "idle")

        now = time.monotonic()
        tx_rate = rx_rate = 0
        if self._prev_stats:
            prev_t, prev_tx, prev_rx = self._prev_stats
            dt = now - prev_t
            if dt > 0.2:
                tx_rate = max(0, int((stats["tx_packets"] - prev_tx) / dt))
                rx_rate = max(0, int((stats["rx_packets"] - prev_rx) / dt))
        self._prev_stats = (now, stats["tx_packets"], stats["rx_packets"])

        # 计算相对清零基准的显示值
        disp_tx = max(0, stats["tx_packets"] - self._base_tx_packets)
        disp_rx = max(0, stats["rx_packets"] - self._base_rx_packets)

        self.tx_total_lbl.configure(text=f"{disp_tx:,}")
        self.tx_rate_lbl.configure(text=f"{tx_rate:,} 帧/s")
        self.rx_total_lbl.configure(text=f"{disp_rx:,}")
        self.rx_rate_lbl.configure(text=f"{rx_rate:,} 帧/s")

        err_tx = stats["tx_errors"]
        err_rx = stats["rx_errors"]
        tec = stats.get("tec", 0)
        rec = stats.get("rec", 0)
        self.err_var.set(
            f"错误: TX {err_tx} (TEC:{tec}) / RX {err_rx} (REC:{rec})  |  "
            f"Bus-Off: {stats['bus_off']}  |  驱动重启: {stats['restarts']}"
        )

    def _reset_stats(self):
        if self._prev_stats:
            _, tx, rx = self._prev_stats
            self._base_tx_packets = tx
            self._base_rx_packets = rx
            self.tx_total_lbl.configure(text="0")
            self.rx_total_lbl.configure(text="0")
            self.log("info", "已重置软件统计发包计数基准")

    def _trigger_poll(self, force_usb=False):
        if self._closing or self._polling:
            return
        self._polling = True
        threading.Thread(target=self._poll_worker, args=(force_usb,), daemon=True).start()

    def _poll_loop(self):
        if self._closing:
            return
        self._trigger_poll()
        interval = 2000 if self._last_can_present else 3000
        self.root.after(interval, self._poll_loop)

    def _poll_worker(self, force_usb=False):
        try:
            # 当 can0 已在线且正常工作时，跳过大部分 usbipd list 子进程以降低系统开销
            if (
                not force_usb
                and self._last_usb_state == "Attached"
                and self._last_can_present
                and self._usb_poll_skip < 4
            ):
                stats = self._can_stats()
                if stats is not None:
                    self._usb_poll_skip += 1
                    self._can_fail_count = 0
                    self.post("can", stats)
                    return

            self._usb_poll_skip = 0
            dev = self._find_device()
            self.post("usb", dev)
            if dev and dev["state"] == "Attached":
                stats = self._can_stats()
                if stats is not None:
                    self._can_fail_count = 0
                    self.post("can", stats)
                else:
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
            self.log("info", "开始建立 PCAN-USB ⇌ WSL2 连接链路 ...")
            usbipd = find_usbipd()
            if not usbipd:
                raise RuntimeError(
                    "未在系统中检测到 usbipd.exe，请先通过终端安装：\n"
                    "winget install -e --id dorssel.usbipd-win"
                )
            dev = self._find_device()
            if not dev:
                raise RuntimeError(
                    f"未找到匹配的 USB-CAN 适配器（VID:PID={self.hwid_var.get()}）。\n"
                    "请确认硬件已插入 USB 口并在设备管理器中识别正常。"
                )
            self.log("info", f"锁定设备：{dev['name']} (BUSID={dev['busid']}, 状态={dev['state']})")

            distro = self.distro_var.get().strip()
            if dev["state"] == "Not shared":
                self._bind_elevated(usbipd, dev["busid"])
                dev = self._find_device() or dev

            self.log("info", f"正在确保 WSL 发行版 [{distro}] 就绪 ...")
            self._ensure_wsl(distro)
            self.log("ok", f"WSL 发行版 [{distro}] 已正常启动")

            if dev["state"] == "Attached" and not force:
                self.log("ok", "硬件设备已处于 Attached 挂载状态，跳过重复挂载")
            else:
                if dev["state"] == "Attached":
                    self.log("info", "清除旧挂载 (detach) ...")
                    run_cmd([usbipd, "detach", "--busid", dev["busid"]], 30)
                    time.sleep(0.5)

                result = run_cmd([usbipd, "attach", "--wsl", "--busid", dev["busid"]], 90)
                if result.returncode != 0:
                    dev2 = self._find_device()
                    if dev2 and dev2["state"] == "Attached":
                        self.log("warn", "attach 返回警告，但设备状态已成功更新为 Attached")
                    else:
                        self.log("warn", f"attach 初次尝试失败，进行第二次重试 ...")
                        time.sleep(1.2)
                        result = run_cmd(
                            [usbipd, "attach", "--wsl", "--busid", dev["busid"]], 90
                        )
                        if result.returncode != 0:
                            dev2 = self._find_device()
                            if not (dev2 and dev2["state"] == "Attached"):
                                raise RuntimeError("挂载设备进 WSL 失败：" + output_of(result))
                self.log("ok", f"USB-CAN 设备已成功挂载入 WSL [{distro}]")

            self._apply_bitrate_worker()
            self.log("ok", "🌟 CAN 台架整条链路已全部就绪！")
            self.post("poll_now", None)
        except Exception as exc:
            self.log("err", str(exc))
            self.post("alert", ("连接失败", str(exc)))
        finally:
            self._connecting = False
            self.post("conn_done", None)

    def _bind_elevated(self, usbipd, busid):
        self.log("warn", "设备处于未共享状态，需要管理员权限执行 bind（请在弹出的 UAC 窗口点击允许）...")
        script = (
            f"$p = Start-Process -FilePath '{usbipd}' "
            f"-ArgumentList @('bind','--busid','{busid}') "
            "-Verb RunAs -WindowStyle Hidden -Wait -PassThru; exit $p.ExitCode"
        )
        result = run_cmd(
            ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
            180,
        )
        if result.returncode != 0:
            detail = output_of(result) or "用户取消了 UAC 提权或无管理员权限"
            raise RuntimeError("USB 绑定失败：" + detail)
        self.log("ok", "USB 设备已成功共享")

    def _ensure_wsl(self, distro):
        last_rc, last_detail = "?", ""
        for attempt in range(1, 4):
            result = run_cmd(
                [
                    "wsl",
                    "-d",
                    distro,
                    "-u",
                    "root",
                    "--exec",
                    "/bin/sh",
                    "-c",
                    "echo __LINSIGHT_WSL_OK__",
                ],
                45,
            )
            text = output_of(result)
            if "__LINSIGHT_WSL_OK__" in text:
                return
            last_rc = result.returncode
            last_detail = text or "无回显"
            if attempt < 3:
                self.log("warn", f"启动 WSL 第 {attempt} 次失败（code={last_rc}），正在重试 ...")
                time.sleep(1.2)

        state = self._wsl_distro_state(distro)
        if state.lower().startswith("run") or "运行" in state:
            self.log("warn", f"WSL 检测命令微抖，但当前状态为 {state}，继续建立连接 ...")
            return
        raise RuntimeError(
            f"无法启动 WSL 发行版 '{distro}'（exit={last_rc}）：{last_detail}\n"
            f"当前状态：{state or '未知'}\n"
            "可尝试点击界面的『重启 WSL』或手动在终端执行 wsl --shutdown。"
        )

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
                self.log("info", "检测到正在连续发包，先自动停止再切换比特率")
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
            raise RuntimeError("比特率必须是有效数字（如 500000）")
        if not 1000 <= bitrate <= 1000000:
            raise RuntimeError("CAN 比特率需在 1k–1000k 范围内")

        iface = self.iface_var.get().strip() or "can0"
        self._wsl_root(
            f"for i in $(seq 1 50); do [ -e /sys/class/net/{iface} ] && break; sleep 0.2; done",
            15,
        )
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
                time.sleep(0.5 * attempt)

        if result is None or result.returncode != 0:
            detail = (output_of(result) if result else "") or f"exit={result.returncode if result else '?'}"
            raise RuntimeError(f"配置网络接口 {iface} 失败：{detail}")
        self.log("ok", f"接口 {iface} 已成功上线 @ {bitrate} bps (自动恢复 restart-ms 100)")

    # ---------------------------------------------------------- disconnect -
    def do_disconnect(self):
        if self._connecting:
            return
        threading.Thread(target=self._disconnect_worker, daemon=True).start()

    def _disconnect_worker(self):
        try:
            self.log("info", "正在断开 CAN 台架链路 ...")
            self._stop_gen_sync(silent=True)
            self._stop_monitor_sync(silent=True)
            iface = self.iface_var.get().strip() or "can0"
            self._wsl_root(f"ip link set {iface} down 2>/dev/null || true", 15)
            dev = self._find_device()
            usbipd = find_usbipd()
            if dev and usbipd:
                res = run_cmd([usbipd, "detach", "--busid", dev["busid"]], 30)
                if res.returncode == 0:
                    self.log("ok", "硬件设备已从 WSL 安全解除挂载（保留系统 Shared 共享态）")
                else:
                    self.log("warn", "detach 告警：" + output_of(res))
            else:
                self.log("info", "设备当前未挂载在 WSL 中")
            self.post("poll_now", None)
        except Exception as exc:
            self.log("err", str(exc))

    def do_restart_wsl(self):
        if not messagebox.askyesno(
            "重启 WSL",
            "将执行 wsl --shutdown，会彻底终止 WSL 中所有发行版的后台进程。\n确定继续？",
            parent=self.root,
        ):
            return
        threading.Thread(target=self._restart_wsl_worker, daemon=True).start()

    def _restart_wsl_worker(self):
        self.log("warn", "正在执行 wsl --shutdown ...")
        res = run_cmd(["wsl", "--shutdown"], 60)
        if res.returncode != 0:
            self.log("err", "wsl --shutdown 失败：" + output_of(res))
            return
        time.sleep(1.5)
        self.log("ok", "WSL 所有发行版已彻底关闭，可随时点击『一键连接』重新启动")
        self.post("poll_now", None)

    # --------------------------------------------------------------- send --
    def _frame_inputs(self):
        can_id = parse_can_id(self.canid_var.get())
        is_rtr = bool(self.rtr_var.get())
        data = b"" if (is_rtr and not self.data_var.get().strip()) else parse_payload(self.data_var.get())
        extended = bool(self.ext_var.get()) or can_id > 0x7FF
        return can_id, data, extended, is_rtr

    def do_send_single(self):
        try:
            can_id, data, extended, is_rtr = self._frame_inputs()
        except ValueError as exc:
            messagebox.showerror("参数错误", str(exc), parent=self.root)
            return

        iface = self.iface_var.get().strip() or "can0"
        dlc = int(self.dlc_var.get()) if is_rtr else None
        frame = cansend_frame(can_id, data, extended, is_rtr, dlc=dlc)
        threading.Thread(
            target=self._send_single_worker, args=(iface, frame), daemon=True
        ).start()

    def _send_single_worker(self, iface, frame):
        try:
            result = self._wsl_exec(["cansend", iface, frame], 10)
            if result.returncode != 0:
                self.log("err", f"发送失败 {frame}：{output_of(result)}")
            else:
                self.log("ok", f"已成功发送: {frame}")
        except Exception as exc:
            self.log("err", f"发送异常：{exc}")

    def do_start_gen(self):
        if self._gen_proc and self._gen_proc.poll() is None:
            return
        try:
            can_id, data, extended, _ = self._frame_inputs()
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
            messagebox.showerror(
                "参数错误",
                f"固定模式下数据长度 ({len(data)} 字节) 与 DLC ({dlc}) 不一致",
                parent=self.root,
            )
            return

        iface = self.iface_var.get().strip() or "can0"
        cmd = f"exec cangen {iface} -I {can_id:X} -L {dlc} -g {gap} -p 10"
        if count > 0:
            cmd += f" -n {count}"
        if extended:
            cmd += " -e"
        if dlc > 0:
            if mode == "递增":
                cmd += " -D i"
            elif mode == "固定":
                cmd += f" -D {data.hex().upper()}"
            # 随机模式无需 -D 参数，cangen 默认随机生成

        self.gen_state_var.set("● 启动中...")
        threading.Thread(target=self._gen_worker, args=(cmd,), daemon=True).start()

    def _gen_worker(self, cmd):
        distro = self.distro_var.get().strip()
        try:
            proc = subprocess.Popen(
                ["wsl", "-d", distro, "-u", "root", "--exec", "bash", "-c", cmd],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
        except Exception as exc:
            self.log("err", f"启动连续生成器失败：{exc}")
            self.post("gen_stopped", -1)
            return

        self._gen_stopping = False
        self._gen_proc = proc
        clean_cmd = cmd.replace("exec ", "", 1)
        self.log("ok", f"连续发包已启动：{clean_cmd}")
        self.post("gen_started", None)

        for line in proc.stdout:
            if line.strip():
                self.log("info", line.rstrip())
        code = proc.wait()
        self.post("gen_stopped", code)

    def do_stop_gen(self):
        self.gen_state_var.set("● 停止中...")
        threading.Thread(target=self._stop_gen_worker, daemon=True).start()

    def _stop_gen_worker(self):
        self._gen_stopping = True
        self._stop_gen_sync()
        self.post("gen_stopped", 0)

    def _stop_gen_sync(self, silent=False):
        self._gen_stopping = True
        proc = self._gen_proc
        self._gen_proc = None
        try:
            self._wsl_exec(["pkill", "-x", "cangen"], 8)
        except Exception:
            pass
        if proc and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
        if not silent:
            self.log("info", "连续发包已停止")

    # ------------------------------------------------------------ monitor --
    def do_start_monitor(self):
        if self._mon_proc and self._mon_proc.poll() is None:
            return
        self._mon_total = 0
        self._last_id_time.clear()
        threading.Thread(target=self._monitor_worker, daemon=True).start()

    def _monitor_worker(self):
        distro = self.distro_var.get().strip()
        iface = self.iface_var.get().strip() or "can0"
        self._mon_stopping = False
        try:
            proc = subprocess.Popen(
                ["wsl", "-d", distro, "-u", "root", "--exec", "candump", iface],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=CREATE_NO_WINDOW,
            )
        except Exception as exc:
            self.log("err", f"启动 candump 失败：{exc}")
            self.post("mon_stopped", -1)
            return

        self._mon_proc = proc
        self.log("ok", f"已启动 {iface} 报文监听通道")
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
                    self.log("err", f"candump: {line}")
                    continue

                self._mon_total += 1
                now = time.time()
                now_mono = time.monotonic()
                canid = parsed["canid"]

                # 周期与时间差 (Δt ms) 精确计算
                delta_str = "—"
                if canid in self._last_id_time:
                    dt = (now_mono - self._last_id_time[canid]) * 1000.0
                    delta_str = f"{dt:.1f}" if dt < 9999 else f"{dt/1000:.2f}s"
                self._last_id_time[canid] = now_mono

                stamp = time.strftime("%H:%M:%S", time.localtime(now))
                stamp += f".{int(now * 1000) % 1000:03d}"

                is_ext = canid > 0x7FF or len(parsed["raw_canid_str"]) > 3
                is_rtr = "remote request" in parsed["data"].lower()
                if is_rtr:
                    can_type = "远程"
                    data_str = "RTR"
                else:
                    can_type = "扩展" if is_ext else "标准"
                    data_str = parsed["data"]

                canid_str = f"0x{canid:08X}" if is_ext else f"0x{canid:03X}"

                # ASCII 解析
                raw_bytes = b""
                if not is_rtr and data_str:
                    try:
                        raw_bytes = bytes.fromhex(data_str.replace(" ", ""))
                    except Exception:
                        pass
                ascii_repr = bytes_to_ascii(raw_bytes) if raw_bytes else ""

                frame_item = {
                    "idx": self._mon_total,
                    "time": stamp,
                    "delta": delta_str,
                    "canid": canid,
                    "canid_str": canid_str,
                    "type": can_type,
                    "dlc": parsed["dlc"],
                    "data": data_str,
                    "ascii": ascii_repr,
                }
                batch.append(frame_item)

                # 节流合并刷新，最高 10fps 刷新率避免 UI 卡顿
                if time.monotonic() - last_flush >= 0.1:
                    self.post("frames", (batch, self._mon_total))
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
            self._wsl_exec(["pkill", "-x", "candump"], 8)
        except Exception:
            pass
        if proc and proc.poll() is None:
            try:
                proc.terminate()
            except Exception:
                pass
        if not silent:
            self.log("info", "总线监听已停止")

    def _toggle_pause(self):
        self._paused = not self._paused
        if self._paused:
            self.mon_pause_btn.configure(text="▶ 继续滚屏")
            self.log("info", "已暂停报文视图滚动（后台持续记录中）")
        else:
            self.mon_pause_btn.configure(text="⏸ 暂停滚屏")
            self.log("info", "恢复实时报文视图更新")
            self._rebuild_tree_from_buffer()

    def _clear_monitor(self):
        for item in self.mon_tree.get_children():
            self.mon_tree.delete(item)
        self._raw_frames.clear()
        self._last_id_time.clear()
        self._mon_total = 0
        self.mon_count_var.set("已捕获 0 帧")

    def _matches_filter(self, f, filt):
        if not filt:
            return True
        clean_filt = filt.replace("0x", "")
        return (
            clean_filt in f["canid_str"].lower()
            or clean_filt in f["data"].lower().replace(" ", "")
            or filt in f["ascii"].lower()
            or filt in f["type"].lower()
        )

    def _insert_frame_row(self, f):
        tags = ["even" if f["idx"] % 2 == 0 else "odd"]
        if f["type"] == "扩展":
            tags.append("ext")
        elif f["type"] == "远程":
            tags.append("rtr")
        self.mon_tree.insert(
            "",
            "end",
            values=(
                f["idx"],
                f["time"],
                f["delta"],
                f["canid_str"],
                f["type"],
                f["dlc"],
                f["data"],
                f["ascii"],
            ),
            tags=tuple(tags),
        )

    def _on_filter_changed(self, *_args):
        self._rebuild_tree_from_buffer()

    def _rebuild_tree_from_buffer(self):
        for item in self.mon_tree.get_children():
            self.mon_tree.delete(item)
        filt = self.filter_var.get().strip().lower()

        # 显示最后匹配的 400 条
        matched = [f for f in self._raw_frames if self._matches_filter(f, filt)][-400:]
        for f in matched:
            self._insert_frame_row(f)
        if self._auto_scroll and matched:
            self.mon_tree.yview_moveto(1.0)

    # ------------------------------------------------------------- export ---
    def _export_traffic_csv(self):
        if not self._raw_frames:
            messagebox.showinfo("提示", "当前无任何捕获的报文记录可供导出", parent=self.root)
            return
        default_name = f"can_dump_{time.strftime('%Y%m%d_%H%M%S')}.csv"
        path = filedialog.asksaveasfilename(
            parent=self.root,
            title="导出 CAN 报文记录为 CSV",
            initialfile=default_name,
            defaultextension=".csv",
            filetypes=[("CSV 表格文件", "*.csv"), ("所有文件", "*.*")],
        )
        if not path:
            return

        try:
            with open(path, "w", newline="", encoding="utf-8-sig") as f:
                writer = csv.writer(f)
                writer.writerow(
                    ["序号", "时间戳", "Δt周期(ms)", "CAN_ID", "帧类型", "DLC", "数据载荷(Hex)", "ASCII"]
                )
                for item in self._raw_frames:
                    writer.writerow(
                        [
                            item["idx"],
                            item["time"],
                            item["delta"],
                            item["canid_str"],
                            item["type"],
                            item["dlc"],
                            item["data"],
                            item["ascii"],
                        ]
                    )
            self.log("ok", f"已成功导出 {len(self._raw_frames)} 帧报文至: {path}")
            messagebox.showinfo("导出成功", f"文件已保存至：\n{path}", parent=self.root)
        except Exception as exc:
            self.log("err", f"导出失败: {exc}")
            messagebox.showerror("导出错误", str(exc), parent=self.root)

    # ------------------------------------------------------- context menu ---
    def _copy_selected_id(self):
        sel = self.mon_tree.selection()
        if sel:
            vals = self.mon_tree.item(sel[0], "values")
            if vals and len(vals) >= 4:
                self.root.clipboard_clear()
                self.root.clipboard_append(vals[3])

    def _copy_selected_data(self):
        sel = self.mon_tree.selection()
        if sel:
            vals = self.mon_tree.item(sel[0], "values")
            if vals and len(vals) >= 7:
                self.root.clipboard_clear()
                self.root.clipboard_append(vals[6])

    def _copy_selected_row(self):
        sel = self.mon_tree.selection()
        if sel:
            lines = []
            for item in sel:
                vals = self.mon_tree.item(item, "values")
                lines.append("\t".join(str(v) for v in vals))
            self.root.clipboard_clear()
            self.root.clipboard_append("\n".join(lines))

    def _load_selected_into_sender(self):
        sel = self.mon_tree.selection()
        if sel:
            vals = self.mon_tree.item(sel[0], "values")
            if vals and len(vals) >= 7:
                can_id_str = vals[3].replace("0x", "")
                is_rtr = vals[4] == "远程"
                is_ext = vals[4] == "扩展" or len(can_id_str) > 3
                self.canid_var.set(can_id_str)
                self.ext_var.set(is_ext)
                self.rtr_var.set(is_rtr)
                if is_rtr:
                    self.dlc_var.set(int(vals[5]))
                    self.data_var.set("")
                else:
                    self.data_var.set(vals[6])
                self.send_notebook.select(0)
                self.log("info", f"已载入报文 [ID: 0x{can_id_str}] 到单帧发送区")

    def _save_selected_as_preset(self):
        sel = self.mon_tree.selection()
        if not sel:
            return
        vals = self.mon_tree.item(sel[0], "values")
        if vals and len(vals) >= 7:
            can_id_str = vals[3].replace("0x", "")
            is_rtr = vals[4] == "远程"
            is_ext = vals[4] == "扩展" or len(can_id_str) > 3
            data_str = "" if is_rtr else vals[6]
            name = simpledialog.askstring(
                "添加到预设", "请输入预设报文名称:", initialvalue=f"报文_0x{can_id_str}", parent=self.root
            )
            if not name:
                return
            self.presets.append(
                {
                    "name": name.strip(),
                    "id": can_id_str,
                    "ext": is_ext,
                    "rtr": is_rtr,
                    "dlc": int(vals[5]),
                    "data": data_str,
                }
            )
            self._refresh_presets_table()
            self._save_config()
            self.log("ok", f"已保存预设 [{name}] (ID: 0x{can_id_str})")

    # ----------------------------------------------------------- log utils --
    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def _copy_log(self):
        content = self.log_text.get("1.0", "end-1c")
        if content:
            self.root.clipboard_clear()
            self.root.clipboard_append(content)
            self.log("info", "已复制全部系统日志到剪贴板")

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
            distro = self.distro_var.get().strip()
            subprocess.Popen(
                [
                    "wsl",
                    "-d",
                    distro,
                    "-u",
                    "root",
                    "--exec",
                    "bash",
                    "-c",
                    "pkill -x candump; pkill -x cangen",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=CREATE_NO_WINDOW,
            )
        except Exception:
            pass

        self.root.destroy()


def main():
    if os.name != "nt":
        print("此控制台工具运行在 Windows 宿主机侧（用于通过 usbipd 与 WSL2 进行交互控制）。")
        return 1
    root = tk.Tk()
    CanBenchApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
