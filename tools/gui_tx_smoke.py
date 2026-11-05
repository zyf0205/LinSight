"""Smoke test: transient bus-off is healed by reset+backoff, stream survives.

Run: py -3.10 tools/gui_tx_smoke.py   (no hardware needed)
"""
import importlib.util
import queue
import sys
import threading
import time
import types

# Fake the python-can module BEFORE importing the GUI (it imports `can`).
class FakeMsg:
    def __init__(self, **kw):
        self.__dict__.update(kw)

class CanError(Exception):
    pass

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
        pass

fake_can = types.SimpleNamespace()
fake_can.CanError = CanError
fake_can.Message = FakeMsg
fake_can.interface = types.SimpleNamespace(Bus=lambda **kw: None)
sys.modules["can"] = fake_can

spec = importlib.util.spec_from_file_location("gui", "host/gui/linsight_gui.py")
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)

app = object.__new__(gui.App)  # no __init__ -> no Tk
app.q = queue.Queue()
app._tx_stop = threading.Event()

cfg = {"iface": "virtual", "channel": "vcan0", "bitrate": 500000,
       "pattern": "single", "id": 0x123, "ext": False, "rate": 2000, "duration": 0}

bus = HealingBus()
gui.App._open_bus = lambda self, c: bus

t = threading.Thread(target=gui.App._tx_worker, args=(app, cfg), daemon=True)
t.start()
time.sleep(4)
app._tx_stop.set()
t.join(timeout=10)
assert not t.is_alive(), "worker did not stop"

print(f"sent={bus.sent} resets={bus.resets}")
assert bus.sent > 50, f"stream died early: {bus.sent} frames"
assert bus.resets >= 5, f"reset path never fired: {bus.resets}"
print("RECOVERY-TEST PASS: bus-off reset + backoff keeps the stream alive")
