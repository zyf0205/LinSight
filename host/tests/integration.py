#!/usr/bin/env python3
"""End-to-end integration test: fake device -> linsightd -> .lsc -> linsight-cli.

Usage: integration.py <linsightd-path> <linsight-cli-path>
Exits 0 on success, 1 on failure.
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

REC = struct.Struct("<HIIBB8s")  # seq u16 | ts_ms u32 | id u32 | flags u8 | dlc u8 | data[8]
HEADER = struct.Struct("<BBBBB")  # AA 55 type len_lo len_hi


def frame(ftype, payload):
    return HEADER.pack(0xAA, 0x55, ftype, len(payload) & 0xFF, len(payload) >> 8) + payload


def batch_frame(records):
    body = struct.pack("<H", len(records))
    for seq, ts, ident, flags, data in records:
        body += REC.pack(seq, ts, ident, flags, len(data),
                         bytes(data).ljust(8, b"\0"))
    return frame(2, body)  # CAN_BATCH


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    daemon, cli = sys.argv[1], sys.argv[2]

    tmp = tempfile.mkdtemp(prefix="linsight-test-")
    ctl = os.path.join(tmp, "ctl.sock")
    log_dir = os.path.join(tmp, "logs")
    port = _free_port()

    proc = subprocess.Popen(
        [daemon, "--port", str(port), "--ctl", ctl, "--log-dir", log_dir],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        _run_test(port, ctl, cli, log_dir, proc)
    except BaseException:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        err = proc.stderr.read().decode(errors="replace")
        print("--- daemon stderr ---")
        print(err[-3000:])
        raise
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    print("integration: all passed")


def _run_test(port, ctl, cli, log_dir, proc):
    dev = _wait_port(port, proc)
    dev.settimeout(5)

    # HELLO, then 10 records in two batches, then STATS.
    dev.sendall(frame(1, b'{"proto":32,"fw":"2.0.0","device":"fake"}'))
    records = [
        (i, 1000 + i * 10, (0x123 if i % 2 else 0x1ABCDEF),
         (0x01 if i % 2 else 0x00), [i, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88][:5])
        for i in range(10)
    ]
    dev.sendall(batch_frame(records[:6]))
    dev.sendall(batch_frame(records[6:]))
    dev.sendall(frame(3, b'{"fps":4330,"total":10}'))
    time.sleep(0.3)

    # stats over the control socket.
    out = _ctl_roundtrip(ctl, b"stats\n")
    st = json.loads(out.strip())
    if st.get("records") != 10:
        fail(f"stats records: {st}")
    if st.get("written") != 10:
        fail(f"stats written: {st}")
    if not st.get("log"):
        fail(f"stats missing log path: {st}")
    log_path = st["log"]
    if not os.path.exists(log_path):
        fail(f"log file missing: {log_path}")

    # pause -> daemon forwards CONTROL; fake device answers ACK; CLI echoes it.
    # The CLI blocks until the ACK, so the command runs while the device side
    # receives the CONTROL and replies.
    with ThreadPoolExecutor(max_workers=1) as ex:
        fut = ex.submit(_ctl_roundtrip, ctl, b"pause\n", False)
        ftype, payload = _recv_frame(dev)
        if ftype != 7 or b"pause" not in payload:
            fail(f"expected CONTROL pause, got type={ftype} {payload!r}")
        _reply_ack(dev, payload)
        ack = fut.result(timeout=5)
    if '"code":0' not in ack:
        fail(f"expected ACK echo, got: {ack!r}")

    # resume the same way.
    with ThreadPoolExecutor(max_workers=1) as ex:
        fut = ex.submit(_ctl_roundtrip, ctl, b"resume\n", False)
        ftype, payload = _recv_frame(dev)
        if ftype != 7 or b"resume" not in payload:
            fail(f"expected CONTROL resume, got type={ftype} {payload!r}")
        _reply_ack(dev, payload)
        ack = fut.result(timeout=5)
    if '"code":0' not in ack:
        fail(f"expected ACK echo, got: {ack!r}")

    # .lsc content check: header + 10 raw records.
    with open(log_path, "rb") as f:
        blob = f.read()
    if blob[:4] != b"LSC1":
        fail(f"bad .lsc magic: {blob[:4]!r}")
    if len(blob) != 32 + 10 * REC.size:
        fail(f".lsc size {len(blob)} != {32 + 10 * REC.size}")
    seqs = [REC.unpack_from(blob, 32 + i * REC.size)[0] for i in range(10)]
    if seqs != list(range(10)):
        fail(f"record seqs wrong: {seqs}")

    # Export via the CLI and verify shape.
    out_txt = os.path.join(log_dir, "export.txt")
    r = subprocess.run([cli, "export", "candump", log_path, "-o", out_txt],
                       capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        fail(f"cli export rc={r.returncode}: {r.stderr}")
    with open(out_txt) as f:
        lines = f.read().splitlines()
    if len(lines) != 10:
        fail(f"export lines: {len(lines)}")
    # Timestamp = start_wall + ts_ms/1000, so record 0 (ts 1000 ms) ends .000000.
    if "can0" not in lines[0] or ".000000)" not in lines[0]:
        fail(f"export line 0 malformed: {lines[0]!r}")
    if "01ABCDEF" not in lines[0]:  # even index -> extended id, no pad
        fail(f"export line 0 missing id: {lines[0]!r}")
    if "00000123" not in lines[1]:  # odd index -> standard id, 8-digit pad
        fail(f"export line 1 missing id: {lines[1]!r}")

    # Device disconnect closes the log gracefully.
    dev.close()
    time.sleep(0.3)


def _wait_port(port, proc, timeout=5.0):
    """Connect once the daemon's listener is up (avoids a bind race)."""
    deadline = time.time() + timeout
    last_err = None
    while time.time() < deadline:
        if proc.poll() is not None:
            fail(f"daemon exited early (rc={proc.returncode})")
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.settimeout(5)
            return s
        except OSError as e:
            last_err = e
            time.sleep(0.05)
    fail(f"daemon never listened on {port}: {last_err}")


def _free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _ctl_roundtrip(ctl_path, msg, expect_ack=False):
    """Send one control command, read the reply, return it as text."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(ctl_path)
    s.sendall(msg)
    if expect_ack:
        data = s.recv(4096)
    else:
        # pause/resume replies arrive when the device ACKs; poll briefly.
        data = b""
        deadline = time.time() + 3
        while time.time() < deadline:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if b"code" in data:
                break
    s.close()
    return data.decode(errors="replace")


def _recv_frame(sock):
    """Read one protocol frame (header + payload), tolerating TCP splits."""
    buf = b""
    while len(buf) < 5:
        chunk = sock.recv(5 - len(buf))
        if not chunk:
            fail("device socket closed while reading header")
        buf += chunk
    magic, magic2, ftype, lo, hi = buf
    if magic != 0xAA or magic2 != 0x55:
        fail(f"bad header from daemon: {buf!r}")
    plen = lo | (hi << 8)
    payload = b""
    while len(payload) < plen:
        chunk = sock.recv(plen - len(payload))
        if not chunk:
            fail("device socket closed while reading payload")
        payload += chunk
    return ftype, payload


def _reply_ack(dev_sock, ctrl_payload):
    """Parse the CONTROL id out of the daemon's JSON and answer with an ACK frame."""
    ctrl = json.loads(ctrl_payload.decode(errors="replace"))
    # Compact JSON: ACK payload is capped at 16 bytes by the protocol.
    ack = json.dumps({"id": ctrl.get("id", 0), "code": 0},
                     separators=(",", ":")).encode()
    dev_sock.sendall(frame(8, ack))


if __name__ == "__main__":
    main()
