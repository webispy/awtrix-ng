# Serial control client

Drives a panel over the USB cable instead of the network - the reference client for
[USB serial control](../../docs/reference/serial.md). Use it when the machine driving the panel and
the panel itself are not on the same network, or when you want frame-by-frame pixel control without
opening a port on your LAN.

```bash
python tools/serialctl/serialctl.py text "Hello" --color FF0000
```

## What it needs

**Nothing from the firmware toolchain.** PlatformIO, esptool and the rest of what a build wants are
irrelevant here; this is a client that talks to a running panel.

| | Command | Needs |
|---|---|---|
| 1 | `uv run tools/serialctl/serialctl.py ping` | **uv only.** The script declares its dependencies as [PEP 723](https://peps.python.org/pep-0723/) metadata and uv resolves them into a cache |
| 2 | `python serialctl.py ping` | any Python with `pyserial` |
| 3 | — | without `pyserial` it exits with `pyserial is required (pip install pyserial)`. The frame encoders keep working without it, which is what lets the wire format be checked off-device |

The metadata block is an inert comment to everything but uv, so the two paths do not conflict.

The shebang points at `uv run --script`, so the file is directly executable and a symlink turns it
into a plain command from any directory:

```bash
ln -s "$PWD/tools/serialctl/serialctl.py" ~/.local/bin/awtrix
awtrix bench --seconds 10
```

`serialctl.py` and `link.py` are the whole client - copy those two files to another machine,
install uv, and it works there without a clone or a toolchain.

The panel must be running a firmware with the serial channel compiled in - any build of this tree.
The port is found on its own when exactly one candidate is present; pass `--port` when several are.

---

## Commands

| Command | What it does |
|---|---|
| `ports` | list candidate serial ports and exit |
| `ping` | round-trip the link and print the latency |
| `text <str>` | show text using the **panel's own font**, which covers Latin-1, Latin Extended-A and Cyrillic and renders anything else as `?` |
| `clear` | dismiss the notification on screen |
| `raw <topic> [body]` | send any `cmd/*` topic - the whole [MQTT surface](../../docs/reference/mqtt.md) |
| `log` | follow the device log until interrupted |
| `bench` | stream full frames flat out and report the throughput |

Global flags:

| Flag | Meaning |
|---|---|
| `--port` | serial device; autodetected when exactly one matches |
| `--baud` | must match the firmware's `AWTRIX_SERIAL_BAUD`, default 115200 |
| `--timeout` | reply timeout in seconds, default 1 |
| `--reconnect` | retry with backoff when the cable goes away - **off by default**, see below |
| `-v` | echo the device log alongside whatever else is printed |

`ping` is a request the panel cannot act on (an unknown topic), chosen so that checking the link
never changes what is on screen.

---

## Throughput

8-N-1 spends ten bits per byte, so 115200 carries 11,520 bytes per second and a full 32x8 frame is
768 of them. Measured on a TC001: **13.6 fps** of full frames, no drops.

The client picks the cheaper encoding for every frame - a full frame, or a delta listing only the
pixels that changed - so what you actually get depends on the content. Scrolling text changes one
column's worth per frame and runs far above the full-frame ceiling; a video would not. A still
image costs five bytes per frame, because the delta rewrites one pixel with the value it already
has just to keep the hold window alive.

`bench` sends full frames deliberately, to measure the worst case, and prints both what the host
managed and what the device counted:

```
  {"stat":"stream","fps":13,"frames":100,"gaps":0,"bad":0}
host sent 149 frames in 10.9s = 13.6 fps (10.2 KiB/s payload)
```

`gaps` is the number of breaks in the frame numbering - the only way this cable can report a lost
frame, since it carries no flow control. A non-zero count means the host is outrunning the panel.

---

## Sharing the cable with esptool

A serial port has one owner, so **stop this client before flashing**. That is why reconnecting is
opt-in: a client that grabs the port back in a loop is what makes a later `esptool` run fail with
`Resource busy`. Without `--reconnect` the client exits when the cable goes away, which is the
behaviour you want in a script.

Flashing is unaffected by the baud rate the firmware runs at - esptool syncs at 115200 whatever
`AWTRIX_SERIAL_BAUD` was built with.

## When it goes wrong

| Symptom | What to check |
|---|---|
| `no USB serial port found` | a charge-only cable, or a missing driver for the board's bridge |
| `several ports match` | pass `--port`; the name changes with the physical USB socket |
| `opened but the panel did not answer` | wrong `--baud`, or a firmware without the serial channel |
| `Resource busy` | something else holds the port - another copy of this client, or a monitor |
| Replies stop after a while | check `gaps` in the `bench` output; the host may be outrunning the panel |
| Text shows as `?????` | the panel font has no glyph for it - render the text yourself and stream it as pixels, see [Stream pixels](../../docs/reference/serial.md#stream-pixels) |

The client leaves DTR and RTS untouched on purpose. Both are wired to EN and GPIO0, and the usual
advice to lower them before opening the port is worse than useless here: DTR is active low on the
reset line, so lowering it **holds the board in reset** for as long as the port stays open. The
panel does not reboot, it goes silent - and then boots on the way out, when closing the port
releases the line, which is what makes this look like "opening the port reboots the panel". See
[the reference](../../docs/reference/serial.md#robustness) for the measurements. The client waits
for an answer before its first real command either way, so a board that does reset on open is
handled too.
