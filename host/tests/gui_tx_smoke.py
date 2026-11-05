"""Hardware-free smoke tests for the bench GUI's TX safety paths.

Covers:
  1. a continuous virtual-bus stream survives transient bus-off (reset+backoff);
  2. the PCAN single-frame probe reports a missing echo with device context and
     latches the adapter;
  3. after a latch, the next burst is retried with one probe frame before any
     of the 20 burst frames are sent.

Run: py -3.10 host/tests/gui_tx_smoke.py   (no hardware needed)
"""
import importlib.util
import pathlib
import queue
import sys
import threading
import time
import types

ROOT = pathlib.Path(__file__).resolve().parents[2]
GUI_PATH = ROOT / "host" / "gui" / "linsight_gui.py"


class FakeMsg:
    def __init__(self, **kw):
        self.__dict__.update(kw)
        self.is_rx = kw.get("is_rx", False)


class CanError(Exception):
    pass


class FakeWidget:
    """Minimal tkinter widget stand-in: only .config(**kw) is used."""

    def __init__(self):
        self.opts = {}

    def config(self, **kw):
        self.opts.update(kw)


# Fake python-can BEFORE importing the GUI (it imports `can`).
fake_can = types.SimpleNamespace()
fake_can.CanError = CanError
fake_can.Message = FakeMsg
fake_can.interface = types.SimpleNamespace(Bus=lambda **kw: None)
sys.modules["can"] = fake_can

spec = importlib.util.spec_from_file_location("gui", GUI_PATH)
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)

TX_CFG = {"iface": "pcan", "channel": "PCAN_USBBUS1", "bitrate": 500000,
          "pattern": "single", "id": 0x123, "ext": False,
          "rate": 2000, "duration": 0}

DEVICE_STATS = {"mode": "NORMAL", "state": "RUNNING", "bitrate": 500000,
                "total": 1000, "bus_err": 0, "tec": 0, "rxq_full": 0,
                "missed": 0}


class EchoBus:
    """Accepts sends and returns every frame as a PCAN TX echo."""

    def __init__(self):
        self.n = 0
        self.sent = 0
        self.status_v = 0

    def send(self, msg, timeout=None):
        self.n += 1
        self.sent += 1

    def recv(self, timeout=None):
        if self.n == 0:
            return None
        return FakeMsg(is_rx=False, arbitration_id=0x123)

    def status(self):
        return self.status_v

    def shutdown(self):
        pass


class DeadBus(EchoBus):
    """Accepts sends but never receives an echo (wedged adapter)."""

    def recv(self, timeout=None):
        return None


class HealingBus:
    """Fails every 100th send with bus-off; reset() heals it."""

    def __init__(self):
        self.n = 0
        self.resets = 0
        self.status_v = 0
        self.sent = 0

    def send(self, msg, timeout=None):
        self.n += 1
        if self.n % 100 == 0:
            self.status_v = 0x10  # BUSOFF
            raise CanError("transient bus-off")
        self.sent += 1

    def status(self):
        return self.status_v

    def reset(self):
        self.resets += 1
        self.status_v = 0

    def shutdown(self):
        # A real reopen creates a fresh adapter; emulate that here.
        self.resets += 1
        self.status_v = 0


def new_app():
    app = object.__new__(gui.App)  # no __init__ -> no Tk
    app.q = queue.Queue()
    app._tx_stop = threading.Event()
    app._tx_running = False
    app._tx_thread = None
    app._latest_stats = dict(DEVICE_STATS)
    app._stats_at = time.monotonic()
    app._tx_wedged = False
    app._device_online = True
    app.logs = []
    app._log = lambda text, level="dim": app.logs.append((level, text))
    app.start_btn = FakeWidget()
    app.stop_btn = FakeWidget()
    app.tx_badge = FakeWidget()
    app.tx_detail = FakeWidget()
    return app


def bump_stats_later(app, delta, delay):
    """Emulate the 1 Hz daemon STATS poll arriving after `delay` seconds."""

    def run():
        time.sleep(delay)
        app._latest_stats = dict(app._latest_stats,
                                 total=app._latest_stats["total"] + delta)
        app._stats_at = time.monotonic()

    threading.Thread(target=run, daemon=True).start()


def wait_tx_done(app, timeout=6.0):
    events = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            item = app.q.get(timeout=0.05)
        except queue.Empty:
            continue
        events.append(item)
        if item[0] == "tx_done":
            return events
    raise AssertionError(f"burst did not finish in {timeout:.1f}s")


def test_probe_echo_ok():
    app = new_app()
    bus = EchoBus()
    ok, detail = gui.App._probe_bus(app, bus, TX_CFG)
    assert ok, detail
    assert "回显正常" in detail, detail
    assert app._tx_wedged is False


def test_probe_missing_echo_latches():
    app = new_app()
    bus = DeadBus()
    bump_stats_later(app, 1, delay=0.5)  # after the 0.35 s echo window
    ok, detail = gui.App._probe_bus(app, bus, TX_CFG)
    assert not ok, detail
    assert "设备收到 +1 帧" in detail, detail
    assert "total=1000" in detail or "total=1001" in detail, detail
    assert app._tx_wedged is True
    assert bus.sent == 1, f"probe must send exactly one frame, sent={bus.sent}"


def test_burst_aborts_before_frames_when_probe_fails():
    app = new_app()
    bus = DeadBus()
    app._collect_tx = lambda: dict(TX_CFG)
    app._open_bus = lambda cfg: bus
    bump_stats_later(app, 1, delay=0.5)

    gui.App._tx_burst(app)
    events = wait_tx_done(app)

    assert bus.sent == 1, f"burst sent frames despite failed probe: {bus.sent}"
    assert app._tx_wedged is True
    assert any(e[0] == "log" and e[2] == "err" for e in events), events
    assert any(e[0] == "tx" and e[1].get("error") for e in events), events


def test_burst_retry_probes_then_sends():
    app = new_app()
    app._tx_wedged = True  # latched by the previous failure
    bus = EchoBus()
    app._collect_tx = lambda: dict(TX_CFG)
    app._open_bus = lambda cfg: bus

    gui.App._tx_burst(app)
    events = wait_tx_done(app)

    assert bus.sent == 21, f"expected 1 probe + 20 burst frames, got {bus.sent}"
    assert app._tx_wedged is False
    assert any(e[0] == "tx" and e[1].get("sent") == 20 for e in events), events


def test_continuous_survives_busoff():
    app = new_app()
    cfg = {"iface": "virtual", "channel": "vcan0", "bitrate": 500000,
           "pattern": "single", "id": 0x123, "ext": False,
           "rate": 2000, "duration": 0}
    bus = HealingBus()
    app._open_bus = lambda c: bus

    t = threading.Thread(target=gui.App._tx_worker, args=(app, cfg), daemon=True)
    t.start()
    time.sleep(4)
    app._tx_stop.set()
    t.join(timeout=10)
    assert not t.is_alive(), "worker did not stop"

    print(f"sent={bus.sent} resets={bus.resets}")
    assert bus.sent > 50, f"stream died early: {bus.sent} frames"
    assert bus.resets >= 3, f"reset path never fired: {bus.resets}"


def main():
    tests = [
        test_probe_echo_ok,
        test_probe_missing_echo_latches,
        test_burst_aborts_before_frames_when_probe_fails,
        test_burst_retry_probes_then_sends,
        test_continuous_survives_busoff,
    ]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print("GUI-TX SMOKE PASS")


if __name__ == "__main__":
    main()
