#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial>=3.5"]
# ///
"""Drive an AWTRIX NG panel over USB serial.

    python tools/serialctl/serialctl.py ports
    python tools/serialctl/serialctl.py text "Hello" --color FF0000
    python tools/serialctl/serialctl.py raw cmd/settings '{"brightness":40}'
    python tools/serialctl/serialctl.py bench --seconds 10

Reconnecting is off by default and on with --reconnect: a script that grabs the port back in a
loop is exactly what makes `esptool` fail later, so it has to be asked for.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import sys
import time

# This file is meant to be symlinked onto PATH, and sys.path[0] is then whichever directory the
# symlink sits in rather than the one holding link.py. Resolve it explicitly.
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))

from link import BAUD, HEIGHT, WIDTH, Link, LinkGone, encode_full, find_ports  # noqa: E402

STOP = False


def _stop(*_args) -> None:
    global STOP
    STOP = True


def log_line(line: str) -> None:
    print(f"  · {line}", file=sys.stderr)


def open_link(args) -> Link:
    return Link(port=args.port, baud=args.baud, timeout=args.timeout,
                on_log=log_line if args.verbose else None).open()


# ---- commands ------------------------------------------------------------------------


def cmd_ports(_args) -> int:
    found = find_ports()
    if not found:
        print("no USB serial port found", file=sys.stderr)
        return 1
    for p in found:
        print(p)
    return 0


def cmd_raw(args) -> int:
    with open_link(args) as link:
        reply = link.command(args.topic, args.body or "")
        print(json.dumps(reply.payload, ensure_ascii=False))
        return 0 if reply.ok else 1


def cmd_ping(args) -> int:
    with open_link(args) as link:
        start = time.monotonic()
        # An unknown topic is the only request that provably changes nothing, so that is the ping.
        reply = link.command("cmd/")
        rtt = (time.monotonic() - start) * 1000
        answered = reply.payload.get("error", {}).get("code") == "notFound"
        print(f"{link.port}  {rtt:.0f} ms  {'ok' if answered else json.dumps(reply.payload)}")
        return 0 if answered else 1


def cmd_text(args) -> int:
    body = {"text": args.text, "stack": False}
    if args.color:
        body["textColor"] = "#" + args.color.lstrip("#")
    if args.hold:
        body["hold"] = True
    with open_link(args) as link:
        reply = link.command("cmd/notify", json.dumps(body, ensure_ascii=False))
        if not reply.ok:
            print(json.dumps(reply.payload, ensure_ascii=False), file=sys.stderr)
            return 1
        if any(ord(c) > 0x24F for c in args.text):
            print("note: the panel font has no glyph for some of those characters and will show "
                  "'?' - render them yourself and stream pixels instead, see "
                  "docs/reference/serial.md", file=sys.stderr)
        return 0


def cmd_clear(args) -> int:
    with open_link(args) as link:
        reply = link.command("cmd/notify/dismiss")
        return 0 if reply.ok else 1


def cmd_log(args) -> int:
    with open_link(args) as link:
        link.on_log = lambda line: print(line, flush=True)
        while not STOP:
            for reply in link.drain():
                print(f"<< {reply}", flush=True)
            time.sleep(0.02)
    return 0


def cmd_bench(args) -> int:
    """Streams generated frames flat out and reports what the device says it managed."""
    frames = []
    for phase in range(16):
        frame = []
        for y in range(HEIGHT):
            for x in range(WIDTH):
                v = (x * 8 + phase * 16) % 256
                frame.append((v, (v * 2) % 256, 255 - v))
        frames.append(frame)

    with open_link(args) as link:
        sent = 0
        start = time.monotonic()
        deadline = start + args.seconds
        seq = 0
        while not STOP and time.monotonic() < deadline:
            # Full frames on purpose: this measures the worst case, 768 bytes every frame.
            link.frame(encode_full(frames[sent % len(frames)]), seq)
            sent += 1
            seq += 1
            for reply in link.drain():
                print(f"  {reply}", file=sys.stderr)
        elapsed = time.monotonic() - start
        print(f"host sent {sent} frames in {elapsed:.1f}s = {sent / elapsed:.1f} fps "
              f"({sent * 768 / elapsed / 1024:.1f} KiB/s payload)")
        # Let the last stats line and the hold window land before the port closes.
        time.sleep(0.3)
        for reply in link.drain():
            print(f"  {reply}", file=sys.stderr)
    return 0


# ---- wiring --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="ulanzi", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="serial device; autodetected when there is exactly one")
    p.add_argument("--baud", type=int, default=BAUD,
                   help="must match the firmware's AWTRIX_SERIAL_BAUD (default 115200)")
    p.add_argument("--timeout", type=float, default=1.0, help="reply timeout in seconds")
    p.add_argument("--reconnect", action="store_true",
                   help="retry with backoff when the cable goes away (off by default so the port "
                        "is free for esptool)")
    p.add_argument("-v", "--verbose", action="store_true", help="echo the device log")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="list candidate serial ports").set_defaults(fn=cmd_ports)
    sub.add_parser("ping", help="check the link").set_defaults(fn=cmd_ping)
    sub.add_parser("clear", help="dismiss the notification on screen").set_defaults(fn=cmd_clear)
    sub.add_parser("log", help="follow the device log").set_defaults(fn=cmd_log)

    t = sub.add_parser("text", help="show text using the panel's own font")
    t.add_argument("text")
    t.add_argument("--color", help="RRGGBB")
    t.add_argument("--hold", action="store_true", help="keep it on screen until dismissed")
    t.set_defaults(fn=cmd_text)

    r = sub.add_parser("raw", help="send any cmd/* topic")
    r.add_argument("topic")
    r.add_argument("body", nargs="?")
    r.set_defaults(fn=cmd_raw)

    b = sub.add_parser("bench", help="measure streaming throughput")
    b.add_argument("--seconds", type=float, default=10.0)
    b.set_defaults(fn=cmd_bench)

    return p


def main(argv: list[str] | None = None) -> int:
    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)
    args = build_parser().parse_args(argv)

    delay = 0.5
    while True:
        try:
            return args.fn(args)
        except LinkGone as exc:
            print(f"link lost: {exc}", file=sys.stderr)
            if not args.reconnect or STOP:
                return 2
            time.sleep(delay)
            delay = min(delay * 2, 8.0)


if __name__ == "__main__":
    raise SystemExit(main())
