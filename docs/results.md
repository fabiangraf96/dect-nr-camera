# Measured results

All figures from the actual hardware, not estimates.

Hardware: Freenove ESP32-S3 WROOM CAM (ESP32-S3-WROOM-1 N16R8, **OV3660**) →
UART 1 Mbaud with flow control → nRF9151 DK TX → DECT NR+ carrier 1665
(1888.704 MHz), MCS 4, 8 subslots, TX power idx 13 → nRF9151 DK RX →
VCOM1 1 Mbaud + RTS/CTS → PC.

## End to end, QVGA (the default)

```
TX board  : 61.6 pkt/s, 36.8 kB/s | radio ok 1235, err 0, lbt 0
                                  | uart pkt 1235, crc 0, resync 4, overrun 0
RX board  : 61.7 pkt/s, 43.2 kB/s | rssi -19.0 dBm, snr 24.75 dB
                                  | pdc ok 1464, crc 0 | pcc ok 1464, crc 0
PC stream : 35.5 kB/s | 1209 packets, 0 CRC errors, 1 resync
            291 frames, 0 lost, 14.6 fps
            291/291 decoded as 320x240
```

JPEG 2369–2380 bytes per frame, 4 fragments each.

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

**The ESP32's CTS input needed inverting.** With the nRF asserting RTS — its
output measured at 51 mV mean, 0 mV minimum, i.e. "receiver ready" — the ESP32
would not transmit a byte. `uart_set_line_inverse(UART_SIGNAL_CTS_INV)` fixes
it. Found by adding a `vdd` console command that reads the CTS wire through
ADC1_CH0; running it was itself the clue, because reconfiguring the pin for the
ADC detaches it from the UART and the stream started at once.

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
