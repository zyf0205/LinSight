"""Bench CAN traffic generator and receiver (python-can; PCAN by default).

The generator is deliberately conservative around PCAN-USB: echo frames prove
a frame completed on the bus, and a 1.5 s silence stops the run before a missing
ACK drives the adapter into bus-off (which sometimes needs a USB re-plug).

Payload: the first 4 bytes are a little-endian 32-bit send counter, followed by
11 22 33 44. `--record` writes one CSV row per accepted frame so a capture can
be compared frame by frame.

Examples:
    py -3.10 tools/bench_can.py tx --rate 1000 --duration 10
    py -3.10 tools/bench_can.py tx --rate 3000 --duration 60 --record tx.csv
    py -3.10 tools/bench_can.py rx --duration 5
    py -3.10 tools/bench_can.py rx --duration 0 --quiet
"""
import argparse
import csv
import os
import sys
import time

import can

TX_MAX_CONSEC_ERRORS = 200
TX_TICK_S = 0.005
TX_MAX_BATCH = 1000


def add_common(p):
    p.add_argument("--interface", default="pcan")
    p.add_argument("--channel", default="PCAN_USBBUS1")
    p.add_argument("--bitrate", type=int, default=500000)
    p.add_argument("--verbose", action="store_true")


def _open_bus(args, *, listen_only=False, receive_own=False, auto_reset=False):
    kwargs = {"interface": args.interface, "channel": args.channel}
    if args.interface != "virtual":
        kwargs["bitrate"] = args.bitrate
    if args.interface == "pcan" and listen_only:
        kwargs["listen_only"] = True
    if args.interface == "pcan" and receive_own:
        kwargs["receive_own_messages"] = True
    if args.interface == "pcan" and auto_reset:
        kwargs["auto_reset"] = True
    return can.interface.Bus(**kwargs)


def _windows_timer_1ms():
    """Raise the Windows timer resolution to 1 ms; returns a restore callback."""
    if os.name != "nt":
        return lambda: None
    try:
        import ctypes

        winmm = ctypes.WinDLL("winmm")
        winmm.timeBeginPeriod(1)
    except Exception:
        return lambda: None
    return lambda: winmm.timeEndPeriod(1)


def _sleep_until(deadline):
    """Sleep most of the way, then spin the last millisecond for accuracy."""
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            return
        if remaining > 0.002:
            time.sleep(remaining - 0.001)
        # else: busy-wait the final <1 ms


def _status(bus):
    """Adapter status word, or '?' when the interface has no status()."""
    try:
        return f"0x{int(bus.status()):X}"
    except Exception:
        return "?"


def _payload(i):
    return i.to_bytes(4, "little") + bytes((0x11, 0x22, 0x33, 0x44))


def cmd_tx(args):
    if args.rate <= 0:
        sys.exit("--rate must be > 0")

    echo_check = args.interface == "pcan" and not args.no_echo_check
    if echo_check:
        # Echo frames prove a frame completed on the bus (was ACKed). They
        # also let us stop before a no-ACK bus drives the PCAN into bus-off.
        print("echo check on: stop before bus-off if the bus stops ACKing")
    elif args.interface == "pcan":
        print("WARNING: echo check disabled; a bus without ACK can wedge the "
              "PCAN (USB re-plug needed)", file=sys.stderr)

    bus = _open_bus(
        args,
        receive_own=echo_check,
        auto_reset=args.interface == "pcan" and not args.no_auto_reset,
    )
    if args.interface == "pcan" and args.no_auto_reset:
        # python-can never clears the parameter, and PCAN-Basic keeps it per
        # channel, so an earlier auto_reset=True can leak into this process.
        try:
            from can.interfaces.pcan.basic import (PCAN_BUSOFF_AUTORESET,
                                                   PCAN_PARAMETER_OFF)
            bus.m_objPCANBasic.SetValue(bus.m_PcanHandle, PCAN_BUSOFF_AUTORESET,
                                        PCAN_PARAMETER_OFF)
        except Exception as exc:
            print(f"could not clear PCAN auto-reset: {exc}", file=sys.stderr)
    if args.reset_before:
        try:
            print(f"controller reset: {bus.reset()}")
        except Exception as exc:
            print(f"controller reset failed: {exc}", file=sys.stderr)
    print(f"tx {args.interface}/{args.channel} {args.bitrate} bps, "
          f"rate={args.rate} fps, id=0x{args.id:X}, ext={args.ext}")
    if args.interface == "pcan":
        limit = max(1, args.bitrate // 250)  # 500k -> 2000 fps
        if args.rate > limit:
            print(f"WARNING: {args.rate:.0f} fps exceeds the recommended "
                  f"{limit} fps at {args.bitrate // 1000}k; the PCAN TX queue "
                  "may overflow and trigger bus-off", file=sys.stderr)

    record = None
    record_writer = None
    if args.record:
        record = open(args.record, "w", newline="", encoding="utf-8")
        record_writer = csv.writer(record)
        record_writer.writerow(
            ("send_index", "monotonic_ns", "id", "ext", "dlc", "data"))

    sent = errors = echoes = consec_errors = 0
    start = time.perf_counter()
    tick = TX_TICK_S if args.rate >= 200 else 1.0 / args.rate
    next_tick = start
    next_report = start
    last_echo = start
    echo_timeout = max(1.5, 3.0 / args.rate)
    budget = 0.0
    aborted = False

    restore_timer = _windows_timer_1ms()
    try:
        while args.duration <= 0 or time.perf_counter() - start < args.duration:
            budget += args.rate * tick
            batch = min(int(budget), TX_MAX_BATCH)
            budget -= batch
            budget = min(budget, args.rate)  # drop runaway catch-up backlog

            for _ in range(batch):
                i = sent + errors
                msg = can.Message(arbitration_id=args.id,
                                  is_extended_id=args.ext,
                                  data=_payload(i))
                try:
                    bus.send(msg)
                    sent += 1
                    consec_errors = 0
                    if record_writer is not None:
                        record_writer.writerow(
                            (i, time.perf_counter_ns(), args.id,
                             int(args.ext), len(msg.data), msg.data.hex(" ")))
                except can.CanError as exc:
                    errors += 1
                    consec_errors += 1
                    if consec_errors == 1 or args.verbose:
                        print(f"send error #{errors}: {exc} "
                              f"(status={_status(bus)})")
                    if consec_errors >= TX_MAX_CONSEC_ERRORS:
                        aborted = True
                        print(f"too many consecutive errors "
                              f"({consec_errors}); aborting")
                        break

            if echo_check:
                while True:
                    msg = bus.recv(0)
                    if msg is None:
                        break
                    if not msg.is_rx:
                        echoes += 1
                        last_echo = time.perf_counter()

            now = time.perf_counter()
            if now - next_report >= 1.0:
                fps = sent / max(now - start, 1e-6)
                print(f"sent={sent} fps={fps:.0f} echoes={echoes} "
                      f"errors={errors} status={_status(bus)}")
                next_report = now
                if record is not None:
                    record.flush()

            if aborted:
                break
            if echo_check and sent > 0 and now - last_echo > echo_timeout:
                print(f"no self-echo for {echo_timeout:.1f}s: the bus is not "
                      "ACKing (or the PCAN is already wedged). Stopped before "
                      "driving it into bus-off. Check the peer is in NORMAL "
                      "mode, the bitrate, wiring and termination; a wedged "
                      "PCAN needs a USB re-plug.")
                break
            if args.duration > 0 and now - start >= args.duration:
                break

            next_tick += tick
            if next_tick < now:  # fell behind: resynchronise, don't burst
                next_tick = now + tick
            _sleep_until(next_tick)
    except KeyboardInterrupt:
        pass
    finally:
        elapsed = time.perf_counter() - start
        fps = sent / max(elapsed, 1e-6)
        print(f"done sent={sent} fps={fps:.0f} echoes={echoes} "
              f"errors={errors} elapsed={elapsed:.2f}s "
              f"status={_status(bus)}")
        if record is not None:
            record.close()
        restore_timer()
        bus.shutdown()


def cmd_rx(args):
    bus = _open_bus(args, listen_only=args.listen_only)
    print(f"rx {args.interface}/{args.channel} {args.bitrate} bps"
          f"{' (listen-only)' if args.listen_only else ''}")

    count = 0
    start = time.perf_counter()
    next_report = start
    try:
        while args.duration <= 0 or time.perf_counter() - start < args.duration:
            remaining = (args.duration - (time.perf_counter() - start)
                         if args.duration > 0 else 0.5)
            msg = bus.recv(timeout=max(0.01, min(0.5, remaining)))
            if msg is None:
                continue
            count += 1
            if not args.quiet:
                print(f"{msg.timestamp:.3f} {msg.arbitration_id:08X} "
                      f"dlc={msg.dlc} {msg.data.hex(' ')}")
            now = time.perf_counter()
            if args.verbose and now - next_report >= 1.0:
                print(f"received={count} fps={count / (now - start):.0f}")
                next_report = now
    except KeyboardInterrupt:
        pass
    finally:
        elapsed = time.perf_counter() - start
        fps = count / max(elapsed, 1e-6)
        print(f"done received={count} fps={fps:.0f} "
              f"elapsed={elapsed:.2f}s status={_status(bus)}")
        bus.shutdown()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    tx = sub.add_parser("tx", help="generate CAN traffic")
    add_common(tx)
    tx.add_argument("--rate", type=float, default=1000,
                    help="frames per second (suggest <=2000 at 500k)")
    tx.add_argument("--duration", type=float, default=5.0,
                    help="seconds; 0 = until Ctrl-C")
    tx.add_argument("--id", type=lambda s: int(s, 0), default=0x123)
    tx.add_argument("--ext", action="store_true", help="extended 29-bit ID")
    tx.add_argument("--no-auto-reset", action="store_true",
                    help="leave PCAN in bus-off (diagnose missing ACK)")
    tx.add_argument("--reset-before", action="store_true",
                    help="reset the PCAN controller before sending")
    tx.add_argument("--no-echo-check", action="store_true",
                    help="keep sending without ACK (can wedge the PCAN)")
    tx.add_argument("--record", metavar="FILE",
                    help="write accepted sends to CSV for frame comparison")
    tx.set_defaults(func=cmd_tx)

    rx = sub.add_parser("rx", help="receive and print CAN traffic")
    add_common(rx)
    rx.add_argument("--duration", type=float, default=5.0,
                    help="seconds; 0 = until Ctrl-C")
    rx.add_argument("--listen-only", action="store_true")
    rx.add_argument("--quiet", action="store_true", help="summary only")
    rx.set_defaults(func=cmd_rx)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
