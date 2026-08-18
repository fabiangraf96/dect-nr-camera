# Measured results

All figures from the actual hardware, not estimates.

Hardware: Freenove ESP32-S3 WROOM CAM (ESP32-S3-WROOM-1 N16R8, **OV3660**) →
UART 1 Mbaud with flow control → nRF9151 DK TX → DECT NR+ carrier 1665
(1888.704 MHz), MCS 4, 8 subslots, TX power idx 13 → nRF9151 DK RX →
VCOM1 1 Mbaud + RTS/CTS → PC.

## End to end, QVGA at q=6 (the default)

Everything rebuilt from clean and reflashed, with hardware flow control active
in both directions:

```
TX board  : 76.7 pkt/s, 46.6 kB/s | radio ok 1535, err 0, lbt 0
                                  | uart pkt 1535, crc 0, resync 3, overrun 0
RX board  : 76.7 pkt/s, 53.7 kB/s | rssi -29.5 dBm, snr 23.5 dB
                                  | pdc ok 1809, crc 0 | pcc ok 1809, crc 0
PC stream : 43.4 kB/s | 1443 packets, 0 CRC errors, 1 resync
            280 frames, 0 lost, 14.0 fps
            280/280 decoded as 320x240
```

JPEG 2982–3038 bytes per frame, 5 fragments each. About 52 % of the link is in
use, so there is room for a better quality setting or a larger frame.

An earlier run at q=12, for comparison — the same link carrying smaller frames:

```
291 frames, 0 lost, 14.6 fps, 35.5 kB/s
JPEG 2369-2380 bytes per frame, 4 fragments each
```

Going from q=12 to q=6 costs about 27 % more data and roughly half a frame per
second. The measured cost is far smaller than the usual rule of thumb suggests,
because the OV3660 compresses a static scene efficiently.

## Resolution ladder

At q=6, changed at runtime, every step verified against the resolution the
camera then announced. All four ran with **zero frame loss**.

| Resolution | Pixels | JPEG/frame | Fragments | fps at the PC | Link |
|---|---|---|---|---|---|
| QVGA 320×240 | 1.0× | 2933 B | 5 | **13.7** | 41.6 kB/s |
| CIF 400×296 | 1.5× | 3982 B | 6 | **13.7** | 56.5 kB/s |
| HVGA 480×320 | 2.0× | 4744 B | 7–8 | **12.6** | 61.7 kB/s |
| VGA 640×480 | 4.0× | 9271 B | 14–30 | 6.1 | 61.3 kB/s |

Reading it:

* **CIF is free.** Half again as many pixels as QVGA at exactly the same frame
  rate, for 15 kB/s more.
* **HVGA costs one frame per second for double the pixels.** This is the point
  where detail stops being cheap but is still nearly free.
* **VGA halves the frame rate, and not because of the link.** At 6.1 fps it is
  drawing 61 kB/s, the same as HVGA — well under the ~83 kB/s ceiling. The
  limit there is the camera path: the driver copies each frame out of its DMA
  buffer into PSRAM, and at 640×480 that copy dominates.

An earlier version of this table was wrong in two places, both worth recording
because both are easy to repeat:

* It reported HVGA at 6.5 fps and VGA at 1.1 fps. Those captures were over a
  megabyte, so the decoder's runaway guard silently discarded half of each one
  before parsing.
* It labelled rows by the resolution that had been *requested*. A `cam res`
  command whose arguments do not survive the shell arrives as a bare `cam`,
  which prints a usage message and changes nothing — so the row was measured
  at the previous resolution and named after the new one. `scripts/set_res.sh`
  now checks the echo and waits for the camera to announce the change.

## Quality ladder

At QVGA, changed at runtime with `cam quality`:

| q | JPEG/frame | Note |
|---|---|---|
| 12 | 2374 B | |
| 6 | 2650 B | the default; +12 %, visibly cleaner on detailed scenes |

The OV3660 compresses a static scene efficiently, so dropping q costs far less
than the rule of thumb suggests.

Quality and resolution are not interchangeable. Lowering q preserves more of
the detail the sensor captured; raising the resolution captures more detail in
the first place. A soft 320×240 picture does not get sharper at q=4, because
the limit is the pixel count.

## What each change was worth

| Change | Before | After |
|---|---|---|
| `fb_count` 1 → 2 (capture overlaps transmit) | 9.1 fps | **14.6 fps** |
| XCLK 10 → 20 MHz | 6.1 fps | 13.9 fps |
| Board: AI-Thinker → Freenove S3 | 11.7 fps, 49.5 kB/s | 14.6 fps, 35.5 kB/s |

The new board produces *less* data at *more* frames per second: the OV3660 at
q=12 compresses to ~2.4 kB where the OV2640 needed ~4.2 kB. The spare budget is
available for resolution or quality.

## Link budget, measured

| Hop | Capacity | Notes |
|---|---|---|
| ESP32-S3 → nRF TX | ≥ 61 kB/s observed | 1 Mbaud UART, no overruns |
| DECT NR+ | ≥ 64 kB/s observed | 92 pkt/s × 700 B, 0 PDC CRC errors |
| nRF RX → PC | **~83 kB/s** | 1 Mbaud VCOM1 + RTS/CTS |

The VCOM1 hop remains the ceiling. At HVGA the receiver starts dropping packets
into it, which is what limits that row of the ladder.

## nRF9131 EK

Same firmware, same camera, same ESP32 pins. QVGA at q=6:

| | nRF9151 DK | nRF9131 EK |
|---|---|---|
| Frames over 20 s | 280 | 275 |
| Lost | 0 | 1 |
| Frame rate | 14.0 fps | 13.7 fps |
| Byte rate | 38.8 kB/s | 44.5 kB/s |
| Frame gap, median / worst | — | 53 / 152 ms |

Two board-specific findings, both of which cost real time:

- **Video must leave the EK over VCOM0, not VCOM1.** The EK wires RTS/CTS on
  VCOM0 only. On VCOM1 the receiver's own CRC check proves the data leaves the
  board intact — 951 fragments queued, 0 corrupt, 0 DECT CRC errors, ring never
  above empty — and 553 of them still arrive at the PC broken. See
  `docs/wiring.md` for everything that was tried and did not help.
- **The camera back-channel needs a level translator on the EK.** Its I/O rail
  is 1.8 V, measured, and the ESP32-S3 needs 2.475 V to read a high. The video
  direction is fine because it runs the other way; the return direction cannot
  work without one, so runtime camera control from the PC is unavailable on the
  EK as wired.

## Delay budget, measured

QVGA at q=6, 13.9 fps, 41 kB/s. Latency is buffering, so each stage is measured
as the bytes standing in it; a queue that fills once tends to stay full, because
it drains at exactly the rate it fills.

| Stage | Standing | Delay | How it is measured |
|---|---|---|---|
| Sensor → last fragment on the UART | — | **114 ms** avg, 132 ms max | `scripts/cam_age.sh` |
| Radio + receiver's outbound ring | **0 B** | ~0 ms | `scripts/measure_latency.sh` |
| Host queue | 0.39 chunks avg, 6 worst | ~10 ms | `scripts/host_backlog.sh` |

The camera dominates, and roughly one frame period of that is inherent: with
`fb_count = 2` the sensor fills one buffer while the previous is still being
fragmented.

Two things that looked like the cause and were not:

- The receiver's 16 kB ring was seen completely full, which would have been
  410 ms. It only fills when nothing is reading the data port -- CTS stays
  deasserted and the ring backs up. With any reader attached it measures 0 B,
  so the ring size is left alone: it is a shock absorber that is normally
  empty, and shrinking it would only cost tolerance for a host that stalls.
- `uart q` in the receiver's log counts packets *through* the ring, not the
  depth *of* it, and reads as alarmingly large while saying nothing about delay.

The receiver's own console cannot be used to watch this while video is flowing:
both virtual COM ports are on one J-Link, and VCOM1 carrying the stream starves
the VCOM0 console into silence. That is why the ring depth is carried in the
statistics packet instead, arriving over the link that is working.

## Radio quality

Boards roughly 30 cm apart, indoors:

```
rssi -18.5 to -19.0 dBm
snr  22.25 to 25.25 dB
pdc  ok 2133, crc 0
pcc  ok 2133, crc 0
```

Zero radio errors throughout. The radio has never been the limiting factor in
this project.

## Traps met along the way

**A stalled UART receiver in the nRF outlives a power cycle, and a measurement
tool can lie.** This one cost more time than everything else together, and the
wrong turnings are the interesting part.

The camera refused to transmit: `link stalled ... is the nRF9151 powered and
CTS wired?`, repeatedly, while the nRF was plainly alive. Inverting the CTS
input made it run, so the polarity looked wrong and the inversion was kept.

It was not a polarity problem. The `cts` command, once it sampled the pad under
both an internal pull-down *and* an internal pull-up, settled it:

```
inversion on   -> pin high 100 %, 0 transitions -> 71 frames
inversion off  -> pin high 100 %, 0 transitions ->  0 frames, 84 stalls
```

Identical pin state, opposite outcome. A receiver that is genuinely throttling
has to move its RTS line; this one never did, because nothing was driving it —
the ESP32's own pull-up held it high, and inverting the input turned
"permanently not clear to send" into "permanently clear to send". Flow control
was not fixed, it was switched off, and the inversion hid that.

What had actually happened is that the nRF's uart2 receiver was left in a state
where RTS stayed deasserted. It survived unplugging the board and running it
from a battery; only reflashing cleared it. The ESP32 had been correct
throughout: it was refusing to transmit into a receiver that said it was not
ready.

Two lessons worth keeping:

* **A single logic level cannot distinguish "driven by the peer" from "held by
  our own pull-up".** Sample under both pulls; a pin that overrides a pull-up
  is being driven, and one that follows whichever pull is applied is floating.
* **The first version of the diagnostic made things worse.** It read the pin
  through the ADC, which stole it from the UART and never gave it back — so
  merely running the command disabled flow control until the next reboot — and
  its one-shot conversions were far too slow for a signal toggling at UART
  rates, making every number it produced an aliasing artefact. The replacement
  uses `gpio_get_level()`, which reads the pad regardless of routing and does
  not disturb a live link.

**`uart_write_bytes()` blocks past its own timeout.** Checking the ring buffer's
free space beforehand is not enough of a guard: with CTS deasserted the capture
task hung inside it after one frame, with the timeout counter still reading
zero, so it looked like a camera fault. `uart_tx_chars()` queues only what fits
and returns, which makes a deadline actually apply.

**Changing resolution needs a driver restart.** esp32-camera sizes its JPEG
frame buffers at init from `frame_size`. The sensor's own `set_framesize()`
changes the picture but not the buffers, so moving to a larger resolution
overflows them: "FB-OVF", then "NO-EOI", then every grab times out.

**The automatic JPEG buffer size is too small at good quality.** `width *
height / 5` is 60 kB at VGA, which a q=12 frame exceeds.
`CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE_CUSTOM` with 128 kB fixes it.

**A stray byte precedes the first back-channel command.** When the nRF first
configures its TX pin the line moves from floating to driven and the receiver
latches a spurious byte, which turned `cam quality 6` into the unknown command
`\xef cam` — while the transmitter reported it as sent. Non-printable bytes are
now dropped and counted.

**`NO-SOI` at start-up is not a fault.** At XCLK 20 MHz the log shows one or two
"JPEG start marker missing" lines while the sensor settles. The driver discards
those frames. During bring-up they were misread as a reason to halve the sensor
clock, which cost half the frame rate for nothing.

**The nRF9151 DK has no SW9.** That was the nRF9160 DK. Here VDD_IO comes from
an on-board nPM1300 and is set through the Board Configurator app in nRF Connect
for Desktop; `nrfutil` cannot read or write it. Measured off the wire instead:
the rail is 3.0 V.
