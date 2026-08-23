# USB serial control

The same commands MQTT accepts, sent down the USB cable instead of the network. Use it when the
machine driving the panel and the panel itself are not on the same network - a laptop on one Wi-Fi,
the clock on another - or when you want frame-by-frame pixel control without opening a port on your
LAN.

It shares the cable and the baud rate with the boot log, so nothing changes about flashing or
`pio device monitor`.

A reference client lives in [`tools/serialctl/`](https://github.com/webispy/awtrix-ng/tree/serial-control/tools/serialctl)
- it speaks everything below, including the pixel streaming, and its README is the manual.

| | |
|---|---|
| Port | UART0 - the one the USB bridge is wired to |
| Speed | **115200**, 8-N-1. Build with `-D AWTRIX_SERIAL_BAUD=230400` to raise it - see [Raising the speed](#raising-the-speed) |
| Framing | one command per line, LF or CRLF |
| Always on | no setting to enable, no authentication - see [Security](#security) |

!!! warning "One program at a time"
    A serial port has a single owner. Close your control script before flashing, or `esptool`
    fails with `Resource busy`. The same applies in reverse.

---

## Send a command

Every [MQTT command topic](mqtt.md) works unchanged. Write the topic, a space, and the JSON body:

```text
cmd/notify {"text":"Hello","textColor":"#FF0000","stack":false}
```

AWTRIX answers on the same line-based channel, prefixed with `<<`:

```text
<<{"ok":true}
```

The reply carries the ordinary [error envelope](errors.md) when something is wrong:

```text
cmd/notify {"text":}
<<{"error":{"code":"invalidJson","message":"payload is not valid JSON"}}
```

Anything **not** starting with `<<` is log output, including the ROM's own boot messages. A reader
should hand those to a log sink and never try to parse them.

### Pair replies with requests

Prefix a line with `#` and a number and it comes back in the reply. The log shares this wire and a
cable can be pulled mid-reply, so a sender that cares which answer belongs to which request should
number them:

```text
#7 cmd/settings {"brightness":40}
<<{"seq":7,"ok":true}
```

The number is optional and is remembered for one line only. Keep it under 1,000,000.

For **frames** the number is not optional in practice, because it is what lets AWTRIX tell you a
frame went missing. Increment it by one per frame and wrap wherever you like - 10,000 keeps it four
digits wide, which is worth a byte or two a frame. A wrap is recognised by the number not advancing,
so any wrap point works. (It did not always: the receiver compared against a literal 10,000 while
this page called it a suggestion, so a sender wrapping anywhere else scored one phantom lost frame
per cycle.)

### What the commands are

There is no serial-specific command list: the topic is routed by the same code that serves MQTT, so
[MQTT topics](mqtt.md) is the reference. The ones you will reach for first:

| Line | Effect |
|---|---|
| `cmd/notify {"text":"Hi","stack":false}` | Replaces what is on screen. Without `"stack":false` it **queues** behind the notifications already waiting - see [Limits](limits.md#apps-and-notifications) |
| `cmd/apps/pushed/mac {"text":"CPU 42%"}` | Creates or updates an app that stays in the rotation |
| `cmd/apps/pushed/mac` | Empty body: removes that app |
| `cmd/settings {"brightness":40}` | Any [setting](settings.md) |
| `cmd/apps/next` | Advances the rotation |
| `cmd/device/reboot` | Reboots |

### Read device state

Four serial-only, bodyless queries expose the hardware state without requiring Wi-Fi or MQTT:

| Line | Reply |
|---|---|
| `qry/capabilities` | Protocol version, available sensors, supported queries, and whether event delivery or button capture is supported |
| `qry/sensors` | Temperature, humidity, pressure, light/ADC, and battery readings; unavailable hardware is `null` |
| `qry/buttons` | Current debounced `left`, `select`, and `right` button states |
| `qry/display` | Configured `brightness`, effective `brightnessActual`, `autoBrightness` mode, and display `gamma` |

They use the same optional sequence prefix as commands:

```text
#8 qry/sensors
<<{"seq":8,"temperature":21.5,"humidity":45,"pressureHpa":null,"lightLevel":38.2,"ldrRaw":1410,"batteryPercent":87,"batteryVoltage":4.01,"batteryPinMillivolts":2240,"lowBattery":false}
```

Protocol version 2 adds pushed, debounced button edges while retaining `qry/buttons` for an initial
snapshot and recovery from a lost serial byte. Feature-detect it through `events:true` and
`eventTypes:["button"]`, not a firmware version string:

```text
<<{"event":"button","button":"select","pressed":true,"atMs":123456}
<<{"event":"button","button":"select","pressed":false,"atMs":123601}
```

`atMs` is monotonic device uptime in milliseconds, not Unix time. An event is emitted immediately
after the 35 ms hardware debounce, once for press and once for release. The names are the physical
buttons, unaffected by rotation or `swapButtons`; simultaneous changes are emitted left, select,
then right. `buttonCapture:false` remains explicit: queries and events only observe button state.
They never consume a press or disable the clock's normal navigation.

!!! note "Text is Latin and Cyrillic only"
    The built-in fonts map Latin-1, Latin Extended-A and Cyrillic. Everything else - CJK, Greek,
    emoji - renders as one `?` per character, on this channel exactly as over HTTP. See
    [Umlauts, accents and other languages](../guides/text.md#umlauts-accents-and-other-languages).
    To put Korean, Japanese or Chinese on the panel, render the text on your computer and stream it
    as pixels, below.

---

## Stream pixels

The panel can be driven as a plain 32×8 framebuffer, the same way [Art-Net](../guides/artnet.md)
does it over the network - apps, notifications and transitions all step aside while frames keep
arriving.

A frame is announced by an ASCII header and followed by exactly that many raw bytes:

```text
!F<seq>:<len>\n<len bytes>
```

| Field | Meaning |
|---|---|
| `!F` | full frame: 3 bytes per pixel, `R G B`, row-major from the top-left |
| `!D` | delta: 5-byte records, pixel index as little-endian `uint16` then `R G B` |
| `<seq>` | frame counter. A gap tells AWTRIX a frame was lost, which it reports in the stats line |
| `<len>` | payload length in bytes. Must divide by 3 for `!F`, by 5 for `!D` |

The payload is read by count, not by delimiter, precisely because pixel data contains `0x0A` and
`0x0D`. The parser returns to line mode the moment the last byte lands, so commands and frames can
be interleaved on one connection.

`x = index % 32`, `y = index / 32`. You address the logical grid; AWTRIX applies its own wiring map
afterwards, so the panel's zigzag is not your problem.

### The hold window

Streaming is not a mode you enter and leave. While the last frame is less than **5 seconds** old
your pixels own the panel; five seconds after the last one, the app rotation comes back on its own.
Two consequences:

* **Resend a static image** at least once every 5 seconds, or it expires.
* **You cannot release early.** Stop sending and wait it out, or leave a last frame you are happy
  with on screen.

The first frame of a new session starts from black, so a partial frame cannot reveal a strip of the
app that was on screen. *Within* a session pixels persist, which is what makes `!D` and partial
`!F` frames useful.

A powered-off display and mood light both beat streaming, as they do Art-Net. Unlike Art-Net,
streaming *does* beat the provisioning screen: a device with no Wi-Fi credentials is exactly the
one somebody drives over the cable, so your frames win while they keep arriving and the
`awtrixng-xxxxxx` screen returns when the hold window lapses.

### Raising the speed

115200 is the wire limit for full frames, so a faster line is the only way past ~14 fps at 24-bit
colour. The baud is a build option rather than a setting, because the boot log shares the port and
a device that came up at a speed your monitor does not expect looks broken:

```bash
pio run -e awtrix_fast_serial -t upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -b 230400
```

`awtrix_fast_serial` is the stock `awtrix` env with the flag added; define your own env the same way
for another rate rather than overriding `build_flags` on the command line, which would drop the
project's linker flags. Pass the same rate to any host tool. The RX ring scales with the setting
automatically.

Whether a given board tolerates it is a property of its USB bridge, not of AWTRIX, and it has to be
measured rather than assumed: **the bridge on a TC001 may refuse anything above 115200.** Flash at
115200 either way - esptool syncs at that speed regardless of what the firmware later runs at - so
a failed experiment costs one reflash, never a brick. If the boot log comes back as garbage at the
higher rate, the bridge did not take it.

### Throughput

8-N-1 spends ten bits per byte, so 115200 baud carries 11,520 bytes per second. A 32×8 panel is
768 bytes per full frame:

| What you send | Bytes/frame | Frames/s at 115200 |
|---|---|---|
| `!F` full frame, 24-bit | 768 | **≈ 14** |
| `!D` delta, 100 pixels changed | 500 | ≈ 22 |
| `!D` delta, 30 pixels changed | 150 | ≈ 70, wire no longer the limit |

The device caps out around 55-65 fps regardless: 256 WS2812 LEDs take 7.7 ms to clock out, and the
loop still has to run everything else. So the wire is the constraint for full frames and the panel
is the constraint for sparse deltas.

Every 100 frames AWTRIX reports what it actually achieved, which is the number worth trusting:

```text
<<{"stat":"stream","fps":14,"frames":100,"gaps":0,"bad":0}
```

`gaps` counts breaks in your `seq` numbering and `bad` counts frames that were refused outright -
between them the only way this cable can tell you a frame did not arrive, since it carries no flow
control. Frames are deliberately **not** acknowledged one by one: a round trip per frame would cost
more than the frame does.

### A lost frame is told at once

The throughput line above is a summary. A break in your numbering also gets its own notice, the
moment it is seen:

```text
<<{"stat":"gap","expected":412,"got":414,"missed":3}
```

`expected` is the number that should have come next, `got` is what did, and `missed` is the running
count for this window. At most one of these is sent every 500 ms, so a cable that has genuinely
fallen apart does not spend what capacity it has left telling you so.

**If you stream deltas, you must act on this.** A `!D` frame describes the difference from what the
panel is holding, so a frame that never arrived leaves your idea of the panel and the panel itself
permanently disagreeing - and every delta after it inherits the error. Send one `!F` and the two
agree again. Waiting for the hundred-frame summary is far too late, which is why this notice exists.

A sender that only ever writes `!F` can ignore it: the next frame repaints everything anyway.

Two other things streaming does not control, exactly as with Art-Net: **brightness** (including
auto-brightness, which will dim your frames as the room darkens) and the **colour pipeline**
(saturation, gamma, tint). Pin a brightness and turn auto-brightness off if you want predictable
output.

---

## Robustness

The channel is built for a cable that gets unplugged mid-sentence.

**Partial lines expire.** A line or frame payload that stops arriving is discarded after **200 ms**
of silence, so the fragment left behind by an unplugged cable cannot splice onto the first bytes of
the next session. A sender that wants to be sure can write a bare newline after connecting; blank
lines are ignored.

**Lines are capped at 2048 bytes.** A longer one is answered with `payloadTooLarge` and swallowed to
the next newline rather than half-parsed. The cap covers the whole line, so the topic and any `#seq`
prefix come out of the same budget as the body - about 25 bytes of it for a typical topic.

**Leave DTR and RTS alone.** Both are wired to the reset circuit - EN and GPIO0 - which is how
`esptool` puts the chip into download mode. The common advice is to lower them before opening the
port so that the reset does not fire. On a TC001 that advice does not merely fail, it produces the
worst outcome of the three:

| Opened with | Result |
|---|---|
| lines untouched | no reset; the first command answers in 20-30 ms |
| DTR driven high | no reset; answers normally |
| **DTR driven low** | **the panel goes silent and stays silent for as long as the port is open** |
| RTS pulsed while DTR is low | reboots, which is what `esptool` is asking for |

DTR is active low on the reset line, so lowering it *holds* the board in reset rather than
restarting it. Nothing is printed, nothing answers, and the boot only arrives later, when the port
is closed and the line is released - which is why this reads from the outside as "opening the port
reboots the panel" when what actually happened is that the panel was dead the whole time and
started up on the way out.

So a client that lowers DTR does not get a panel that reboots. It gets one that never speaks.
Measured with two different serial libraries, in Python and in Rust, on 2026-08-21.

```python
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = port, 115200, 1.0
ser.open()                # DTR/RTS deliberately not set
ser.write(b"\n")          # discard anything left half-sent
ser.reset_input_buffer()
```

Bridges differ, so do not assume either way on new hardware - open the port twice and watch for a
second boot banner. Whichever way it goes, **wait for the panel to answer before sending the first
real command**: a board that does reset swallows about half a second of input while it boots. An
unknown topic such as `cmd/` is the probe to use, because it is the one request that provably
changes nothing.

Keep one connection open and reuse it rather than opening a port per command.

**Settings are written 1.5 s after they change.** Send `cmd/settings` and pull the cable
immediately and the value is lost - the panel changed, the flash did not. Wait two seconds.

---

## Security

There is no authentication, and there is no setting to switch this off. Anything that can open the
port can drive the panel - which is also true of the flashing path that shares the cable, so the
trust boundary is physical access to the USB port, not the protocol.

Only `cmd/*` topics, the documented `qry/*` reads, and frames are reachable. File, icon and script
uploads are not exposed here.

---

## Related

* [MQTT topics](mqtt.md) - the command reference this channel shares
* [Art-Net](../guides/artnet.md) - the same pixel streaming over the network
* [Errors](errors.md) - the error envelope the replies use
* [Flashing](../getting-started/flashing.md) - the other thing this cable does
* `tools/serialctl/README.md` - the reference client for this channel
