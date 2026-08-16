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

Same link, same q=12, changed at runtime with `cam res` from the TX shell.

| Resolution | Pixels | JPEG/frame | fps at the PC | Link | Frames lost |
|---|---|---|---|---|---|
| QVGA 320×240 | 1.0× | 2.4 kB | **14.6** | 35 kB/s | 0 |
| HVGA 480×320 | 2.0× | 3.9 kB | 6.5 | 59 kB/s | 0 |
| VGA 640×480 | 4.0× | ~20 kB | 1.1 | 23 kB/s | — |

QVGA leaves 57 % of the link unused; HVGA runs it close to the VCOM1 ceiling.
VGA is not usable for live video on this link: at 1.1 fps the sensor also
spends much of its time recovering from buffer overflows.

## Quality ladder

At QVGA, changed at runtime with `cam quality`:

| q | JPEG/frame | Note |
|---|---|---|
| 12 | 2374 B | the default |
| 6 | 2650 B | +12 %, visibly cleaner on detailed scenes |

The OV3660 compresses a static scene efficiently, so dropping q costs far less
than the rule of thumb suggests. There is ample link budget for q=6 at QVGA.

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
