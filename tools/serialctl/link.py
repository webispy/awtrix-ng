"""Serial link to an AWTRIX NG panel.

The wire format lives in docs/reference/serial.md. Everything awkward about this link is
about the cable, not the protocol: opening the port resets the device unless DTR/RTS are lowered
first, the log shares the channel with replies, and the cable can vanish mid-write.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable, Iterator

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    # Soft, because half of this module is pure: the frame encoders and the header format need no
    # port and no library, and a conformance test for the wire should not have to install one to
    # read them. Anything that actually touches the cable asks for it below.
    serial = list_ports = None


def _need_pyserial() -> None:
    if serial is None:
        raise SystemExit("pyserial is required (pip install pyserial)")

REPLY_PREFIX = "<<"
BAUD = 115200
# The panel this link speaks to. Geometry is a property of the device, so it lives beside the
# transport rather than beside whatever happens to be drawing.
WIDTH, HEIGHT = 32, 8
# Ranked, not merged. The first tier is what a USB-serial bridge is called - a TC001 shows up as
# usbserial or wchusbserial. The second is where a board with native USB lands, but so does every
# iPhone plugged into the machine, so it only counts when the first tier finds nothing.
PORT_TIERS = (
    ("usbserial", "wchusbserial", "SLAB_USBtoUART", "ttyUSB"),
    ("usbmodem", "ttyACM"),
)


class LinkGone(Exception):
    """The cable went away. Distinct from a protocol error, which arrives as a reply."""


@dataclass
class Reply:
    ok: bool
    payload: dict
    seq: int | None = None


def rank_ports(devices: list[str]) -> list[str]:
    """Candidates from a list of device names, best tier first.

    A lower tier is only reported when no better one matched. Separate from asking the system what
    exists so the ranking can be pinned by a test - it is the part with a decision in it.
    """
    for tier in PORT_TIERS:
        hit = [d for d in devices if any(h in d for h in tier)]
        if hit:
            return hit
    return []


def find_ports() -> list[str]:
    """What is plugged in, ranked."""
    _need_pyserial()
    return rank_ports([p.device for p in list_ports.comports()])


def pick_port(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    found = find_ports()
    if not found:
        raise LinkGone("no USB serial port found - check the cable is a data cable")
    if len(found) > 1:
        raise LinkGone(f"several ports match, pass --port: {', '.join(found)}")
    return found[0]


@dataclass
class Link:
    port: str | None = None
    baud: int = BAUD
    timeout: float = 1.0
    # Generous: it has to cover a boot on the bridges that do reset on open.
    ready_timeout: float = 5.0
    on_log: Callable[[str], None] | None = None
    _ser: serial.Serial | None = field(default=None, init=False, repr=False)
    _buf: bytes = field(default=b"", init=False, repr=False)
    _seq: int = field(default=0, init=False, repr=False)

    # ---- lifecycle -------------------------------------------------------------------

    def open(self) -> "Link":
        device = pick_port(self.port)
        _need_pyserial()
        ser = serial.Serial()
        ser.port = device
        ser.baudrate = self.baud
        ser.timeout = self.timeout
        ser.write_timeout = 5.0
        # DTR and RTS are left exactly as the driver leaves them. The usual advice is to lower
        # both before open() so the auto-reset circuit does not fire; on a TC001 that is the one
        # thing not to do. DTR is active low on the reset line, so lowering it holds the board in
        # reset for as long as the port is open - the panel goes silent rather than rebooting, and
        # only boots on the way out when closing the port releases the line. Untouched, the port
        # opens silently and the panel answers in 20-30 ms.
        try:
            ser.open()
        except (OSError, serial.SerialException) as exc:
            raise LinkGone(f"{device}: {exc}") from exc
        self.port = device
        self._ser = ser
        self._buf = b""
        # A newline terminates whatever fragment the last session left mid-line; the device ignores
        # blank lines, so this is free.
        self._write(b"\n")
        ser.reset_input_buffer()
        self._wait_ready()
        return self

    def _wait_ready(self) -> None:
        """Waits until the panel answers, so a first command is never fired into a booting device.

        Some bridges do reset the board on open however carefully the port is configured; the boot
        that follows swallows roughly half a second of input. An unknown topic is the probe because
        it is the one request that provably changes nothing.
        """
        deadline = time.monotonic() + self.ready_timeout
        while time.monotonic() < deadline:
            self._write(b"cmd/\n")
            spin = time.monotonic() + 0.4
            while time.monotonic() < spin:
                if self.drain():
                    return
                time.sleep(0.01)
        raise LinkGone(
            f"{self.port} opened but the panel did not answer within "
            f"{self.ready_timeout:g}s - wrong baud, or firmware without the serial channel?"
        )

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except (OSError, serial.SerialException):
                pass
            self._ser = None

    def __enter__(self) -> "Link":
        return self.open()

    def __exit__(self, *_exc) -> None:
        self.close()

    # ---- plumbing --------------------------------------------------------------------

    def _write(self, data: bytes) -> None:
        if self._ser is None:
            raise LinkGone("port is not open")
        try:
            self._ser.write(data)
        except (OSError, serial.SerialException) as exc:
            self.close()
            raise LinkGone(f"write failed: {exc}") from exc

    def _read_available(self) -> None:
        if self._ser is None:
            raise LinkGone("port is not open")
        try:
            waiting = self._ser.in_waiting
            if waiting:
                self._buf += self._ser.read(waiting)
        except (OSError, serial.SerialException) as exc:
            self.close()
            raise LinkGone(f"read failed: {exc}") from exc

    def _take_lines(self) -> Iterator[str]:
        lines, self._buf = split_lines(self._buf)
        yield from lines

    def drain(self) -> list[str]:
        """Reads what is waiting, routes log lines to on_log, returns the reply lines."""
        self._read_available()
        replies = []
        for line in self._take_lines():
            payload = reply_payload(line)
            if payload is not None:
                replies.append(payload)
            elif line and self.on_log is not None:
                self.on_log(line)
        return replies

    # ---- commands --------------------------------------------------------------------

    def command(self, topic: str, body: str = "", wait: bool = True) -> Reply | None:
        self._seq = (self._seq + 1) % 10000
        seq = self._seq
        line = f"#{seq} {topic}"
        if body:
            line += " " + body
        self._write(line.encode() + b"\n")
        if not wait:
            return None
        return self._await_reply(seq)

    def _await_reply(self, seq: int) -> Reply:
        import json

        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            for raw in self.drain():
                try:
                    doc = json.loads(raw)
                except ValueError:
                    continue
                # Stats arrive unprompted; they are not anybody's reply.
                if doc.get("stat"):
                    continue
                if doc.get("seq") not in (None, seq):
                    continue
                return Reply(ok=bool(doc.get("ok")), payload=doc, seq=doc.get("seq"))
            time.sleep(0.005)
        raise LinkGone(f"no reply to #{seq} within {self.timeout:g}s")

    # ---- pixel streaming -------------------------------------------------------------

    def frame(self, payload: bytes, seq: int, delta: bool = False) -> None:
        self._write(frame_header(seq, len(payload), delta) + payload)


def frame_header(seq: int, length: int, delta: bool = False) -> bytes:
    """The ASCII announcement that precedes a frame's bytes: `!F<seq>:<len>\n`.

    Separate from writing it so the format can be checked without a port to write to - the payload
    encoders below are already testable, and this was the one part of the wire that was not.
    """
    kind = "D" if delta else "F"
    return f"!{kind}{seq % 10000}:{length}\n".encode()


def split_lines(buf: bytes) -> tuple[list[str], bytes]:
    """Complete lines out of a buffer, and whatever is left mid-line.

    The device's log shares this channel with its replies, and a cable can be pulled mid-sentence,
    so what arrives is a stream rather than a sequence of messages. Undecodable bytes are replaced
    rather than raised on: a garbled boot banner is not a reason to drop the connection.
    """
    lines = []
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        lines.append(raw.decode("utf-8", "replace").rstrip("\r"))
    return lines, buf


def reply_payload(line: str) -> str | None:
    """The reply's body, or None when the line is log output.

    Everything not prefixed `<<` is the device talking to itself, including the ROM's own boot
    messages. A reader that tries to parse those will eventually parse one that looks like JSON.
    """
    if line.startswith(REPLY_PREFIX):
        return line[len(REPLY_PREFIX):]
    return None


def encode_full(pixels: list[tuple[int, int, int]]) -> bytes:
    return bytes(c for px in pixels for c in px)


def encode_delta(
    pixels: list[tuple[int, int, int]], previous: list[tuple[int, int, int]] | None
) -> tuple[bytes, bool]:
    """Returns (payload, is_delta), picking whichever encoding is smaller on this frame."""
    full = encode_full(pixels)
    if previous is None or len(previous) != len(pixels):
        return full, False
    out = bytearray()
    for i, (px, was) in enumerate(zip(pixels, previous)):
        if px != was:
            out += i.to_bytes(2, "little") + bytes(px)
    if not out:
        # Nothing moved, but the panel's hold window still needs feeding, so rewrite one pixel with
        # the value it already has: five bytes instead of a redundant 768.
        return (0).to_bytes(2, "little") + bytes(pixels[0]), True
    if len(out) >= len(full):
        return full, False
    return bytes(out), True
