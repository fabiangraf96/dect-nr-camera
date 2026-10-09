# Camera firmware

ESP-IDF firmware for an ESP32-S3 camera board: captures JPEG frames from the
sensor and streams them, fragmented into `dectcam` packets, to the nRF9151
transmitter over UART.

## The board

Developed on a **Freenove ESP32-S3 WROOM CAM** (ESP32-S3-WROOM-1 N16R8 —
16 MB flash, 8 MB octal PSRAM), which is pin-compatible with Espressif's
ESP32-S3-EYE for the camera interface.

The sensor is whatever the board shipped with: these boards carry either an
**OV2640** or an **OV3660**, `esp32-camera` detects which at start-up, and
nothing here depends on the answer. The measurements in
[`../docs/results.md`](../docs/results.md) were taken with an OV3660.

Any ESP32-S3 camera board will do — adjust the pin map at the top of
`main/cam.c` and the UART pins in `main/Kconfig.projbuild`.

The board is powered from its own USB-C cable, which also carries the console
and the flashing protocol. It is **not** powered from the DK.

## Prerequisites

* **ESP-IDF v5.4.2** or compatible, with the `esp32s3` target installed.
* The `esp32-camera` component, pulled automatically by the component manager
  from `main/idf_component.yml`.

## Building

```bash
idf.py set-target esp32s3
idf.py build
```

`main/dectcam_proto.h` is a **copy** of `nrf/common/dectcam_proto.h`. If you
change the protocol, change it there and copy it across; the Python mirror in
`viewer/dectcam_viewer/proto.py` needs the same treatment, and the self-tests
on both sides will tell you if the three drift apart.

## Flashing

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Use the USB-C socket marked **UART**, which is behind the on-board USB-serial
bridge. On Linux the port is typically `/dev/ttyUSB0` or `/dev/ttyACM0`; prefer
the stable path under `/dev/serial/by-id/`, since the number changes on replug.

Note that opening the console port resets the board, because the bridge's
DTR/RTS lines drive the auto-reset circuit. Anything set at runtime is lost
when you attach a terminal.

## Why ESP-IDF and not Zephyr

Zephyr's support looks complete on paper: the ESP32-S3 has the LCD_CAM
peripheral, Zephyr has a driver for it (`video_esp32_dvp.c`), it has an OV2640
driver that advertises JPEG up to UXGA, and `esp32s3_eye` — which this board is
pin-identical to — is a supported board. A Zephyr workspace was set up, a board
definition written, and the upstream video sample built and linked cleanly.

**The driver cannot capture JPEG.** Two independent reasons:

* `subsys/video/format.c` sets `pitch = 0` for `VIDEO_PIX_FMT_JPEG`, because a
  compressed frame has no meaningful row stride. `video_esp32_dvp.c` derives
  its DMA length from `pitch * height`, which is then zero.
* Even given a length, the driver delivers a buffer only on
  `DMA_STATUS_COMPLETE` — a *full* transfer. A JPEG frame is shorter than the
  buffer, so the transfer never completes and the frame never arrives.
  `VIDEO_ESP32_VSYNC_MASK` is defined in the driver but never referenced, so
  there is no end-of-frame path either.

Upstream's own configuration agrees: the only board file for `esp32s3_eye` in
the video sample selects RGB565, never JPEG.

Without compression the link cannot carry live video — RGB565 at QVGA is
153.6 kB per frame against a measured ceiling of about 83 kB/s, i.e. 0.5 fps.
The ESP32-S3 has no hardware JPEG encoder either, so the sensor's own engine is
the only option, and reaching it means `esp32-camera`.

## Runtime control

Two ways in, both reaching the same parser.

**On the board's own console** (115200 baud):

```
cam info                 show current settings
cam res qvga             qqvga qcif hqvga qvga cif hvga vga svga
cam quality 6            4..63, LOWER is better quality
cam fps 15               1..30
cam vflip 1 / hmirror 1
cam pause / cam resume
stats                    link counters
meta                     resend the stream-parameter packet
cts                      diagnose the flow-control wire
```

**From the transmitter's shell**, over the UART back-channel — no second cable
needed. See [`../nrf/README.md`](../nrf/README.md).

Changing resolution or quality re-announces the parameters to the viewer
automatically.

## Pin usage

The camera, microSD, PSRAM, native USB and the console take most of the GPIOs.
Excluding the strapping pins (0, 3, 45, 46) and the on-board LEDs (2, 48), what
is left for the link is `GPIO14`, `GPIO21`, `GPIO41`, `GPIO42`, `GPIO47`.

| Signal | Pin | To the DK | |
|---|---|---|---|
| TX (video) | `GPIO42` | D2 | 1 kΩ in series |
| RX (commands) | `GPIO21` | D3 | |
| CTS | `GPIO41` | D6 | |
| RTS | `GPIO47` | D7 | 1 kΩ in series |

`GPIO42` rather than the more obvious `GPIO14`: the latter is on the module's
**left-hand** header while everything else is on the right, and in a breadboard
the module covers the left header completely. `GPIO42` is a JTAG pad (MTMS),
free here because the S3 debugs over its native USB.

`GPIO41` rather than `GPIO1` for CTS: both are equally free, but `GPIO1` sits
on row 3 of the right-hand header while TX/RX/RTS/GND sit on rows 5/16/17/20.
On a short breadboard (17 rows) that 18-row span cannot be covered without a
pin hanging unsupported off one end. `GPIO41` sits on row 6, which keeps all
five required pins — TX, CTS, RTS, RX and GND — inside one contiguous 17-row
window (rows 4–20). See [`../docs/wiring.md`](../docs/wiring.md#breadboard-footprint)
for the row-by-row layout.

See [`../docs/wiring.md`](../docs/wiring.md) for the full rationale and the
series resistors.

## Diagnosing a stalled link

If the console repeats

```
link stalled at fragment 0/4 (33 drops); is the nRF9151 powered and CTS wired?
```

then the camera is being held off by flow control. Run `cts`, which samples the
CTS pin under both an internal pull-down and pull-up:

```
with pull-down : high  0.0 %   transitions 0
with pull-up   : high  0.0 %   transitions 0
VERDICT: driven low. The wire is good and the nRF is ready.
```

Reading it under both pulls is what makes the answer trustworthy. A single
reading cannot separate "held high by the peer" from "held high by our own
pull-up because nothing is connected", and that ambiguity is exactly what
produced two wrong diagnoses during development.

The command reads the pad without unrouting it, so it is safe to run on a live
link.

## Bench mode

`sdkconfig.bench` streams out of the console UART instead of the nRF link, so
the whole camera → JPEG → fragment → protocol chain can be validated with **no
jumper wires connected at all**:

```bash
idf.py -B build_bench -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.bench" build
```

Then capture the serial port and feed the bytes to the viewer's `analyze`
module. Logging is disabled in this mode, because log text on the same wire
would corrupt the binary stream.

`sdkconfig.noflow` is the equivalent for running without flow control, which
needs only two wires plus ground.
