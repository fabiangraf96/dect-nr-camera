# DECT NR+ Live Camera Link — Architecture

## Goal

Stream live video from a camera module to a PC over a DECT NR+ radio link built
from two nRF9151 DKs.

```
┌────────────────┐  UART 1 Mbaud  ┌──────────────┐   DECT NR+    ┌──────────────┐  VCOM1 1 Mbaud ┌─────────┐
│ Freenove S3    │ GPIO42/21/41/47│ nRF9151DK TX │   1.9 GHz     │ nRF9151DK RX │  over USB-C    │ PC      │
│ OV3660 → JPEG  │ ─────────────► │ uart2 ──► TX │ ────────────► │ RX ──► uart2 │ ─────────────► │ viewer  │
│ ESP-IDF v5.4.2 │  framed frags  │  DECT PDC    │   PDC frames  │  DECT PDC    │  framed frags  │ Python  │
└────────────────┘                └──────────────┘               └──────────────┘                └─────────┘
                                     NCS v3.3.1                     NCS v3.3.1                    OpenCV
```

One framing format (`dectcam_proto`) travels unmodified end to end. Both nRF
apps are dumb, low-latency fragment pumps; all image logic lives at the two
ends. That keeps the radio firmware small and makes each hop independently
testable.

## Components

| Directory | Content | Toolchain |
|---|---|---|
| `esp32/` | Sensor capture, JPEG, fragmenter, UART out | ESP-IDF v5.4.2 |
| `nrf/` | `tx/`, `rx/`, shared `common/` | NCS v3.3.1 |
| `viewer/` | Reassembly and live display | Python 3.12 + OpenCV |

The protocol is specified normatively in [`protocol.md`](protocol.md).
`nrf/common/dectcam_proto.h` is the reference implementation; it is copied
verbatim into `esp32/main/` and mirrored as `proto.py` in the viewer.

## Hardware

| | |
|---|---|
| Camera | Freenove ESP32-S3 WROOM CAM (ESP32-S3-WROOM-1 **N16R8**) |
| Sensor | **OV3660** — Freenove ships this board with either an OV2640 or an OV3660; ours is the latter, and `esp32-camera` handles both |
| Memory | 16 MB quad flash, 8 MB octal PSRAM |
| Radio | 2 × nRF9151 DK (PCA10171) **or** 2 × nRF9131 EK (PCA10165), DECT NR+ modem firmware 2.0.0 |

Both boards are powered from their own USB-C cables. See
[`wiring.md`](wiring.md) for the four signal wires between them.

## Why these technology choices

### ESP-IDF, not Zephyr, on the camera

The brief prefers Zephyr everywhere, with an escape clause: *"if Zephyr is
absolutely not supported, use something else."* This is that case, though not
for the reason it first appears.

Zephyr's support looks complete on paper. The ESP32-S3 has the **LCD_CAM**
peripheral, Zephyr has a driver for it (`video_esp32_dvp.c`), it has an OV2640
sensor driver that advertises JPEG up to UXGA, and `esp32s3_eye` — which this
board is pin-identical to — is a supported board. A workspace was set up, a
board definition written, and the upstream video sample built and linked
cleanly for it.

**The driver cannot capture JPEG.** Two independent reasons:

* `subsys/video/format.c` sets `pitch = 0` for `VIDEO_PIX_FMT_JPEG`, because a
  compressed frame has no meaningful row stride. `video_esp32_dvp.c` then
  derives its DMA length from `pitch * height`, which is zero.
* Even given a length, the driver hands a buffer to the application only on
  `DMA_STATUS_COMPLETE` — a *full* transfer. A JPEG frame is shorter than the
  buffer, so the transfer never completes and the frame never arrives.
  `VIDEO_ESP32_VSYNC_MASK` is defined in the driver but never referenced, so
  there is no end-of-frame path either.

Upstream's own configuration corroborates this: the only board file for
`esp32s3_eye` in the video sample sets `CONFIG_VIDEO_PIXEL_FORMAT="RGBP"`.
The JPEG path is never exercised.

Without compression the link cannot carry live video at all — RGB565 at QVGA is
153.6 kB per frame against a measured ceiling of ~83 kB/s, i.e. 0.5 fps. The
ESP32-S3 has no hardware JPEG encoder either (`SOC_JPEG_CODEC_SUPPORTED` is not
defined for it; that is an ESP32-P4 feature), so the sensor's own JPEG engine is
the only option, and reaching it means ESP-IDF's `esp32-camera`.

The board definition itself was sound — only the driver underneath it is the
obstacle. Should the upstream driver ever gain variable-length support, porting
the camera side to Zephyr becomes a contained piece of work: the protocol, both
radio applications and the viewer are unaffected by which RTOS produces the
JPEG frames.

### The nRF9151 has no USB peripheral

"Receiver connected via USB-C" is physically a **USB-to-UART bridge in the
on-board nRF5340 interface MCU**. The nRF91 series has no USB device
controller. The RX board therefore reaches the PC over VCOM1, not native USB —
which is also why the link ceiling is a UART ceiling.

### Not uart1, and not the same instance on both boards

Both boards' non-secure devicetrees disable `uart1` because TF-M claims it for
secure logging. Rather than reconfiguring TF-M, we borrow another UARTE and
route it to the pads we need via pinctrl — the nRF91 pin matrix allows any
UARTE instance on any pin. The TX board points it at a header, the RX board at
the VCOM1 pads.

Which instance is free is a board property, not an application one. On the
nRF91 each instance number is shared by SPIM, SPIS, TWIM, TWIS and UARTE, so
only one of them can exist at a time:

| | free instance | because |
|---|---|---|
| nRF9151 DK | `uart2` | nothing else claims instance 2 |
| nRF9131 EK | `uart3` | instance 2 is `i2c2`, the nPM1300 PMIC; instance 3 is only an already-disabled external flash |

The C names no instance. Each overlay sets `chosen { dectcam,link-uart }` and
the source resolves that, which is what keeps one firmware serving both boards
instead of forking it.

### Hardware flow control is mandatory

Measured on the VCOM1 link (20 s continuous 64-byte blocks, 1 Mbaud):

| Config | Throughput | Block loss |
|---|---|---|
| No flow control | 80.4 kB/s | **13.1 %** |
| `hw-flow-control` (RTS/CTS) | **81.0 kB/s** | **0.00 %** |

Without RTS/CTS the USB path silently drops bytes, at no throughput gain.

On the camera link, flow control also required inverting the ESP32's CTS input
— see the trap list in [`results.md`](results.md).

## Measured / verified facts

Everything below was confirmed on the actual hardware, not assumed.

| Item | Value |
|---|---|
| NCS | v3.3.1, Zephyr 4.3.99, Zephyr SDK 0.17.0 |
| ESP-IDF | v5.4.2, target `esp32s3` |
| Board target | `nrf9151dk/nrf9151/ns` or `nrf9131ek/nrf9131/ns` (TF-M non-secure) |
| DK VDD_IO | **3.0 V** — measured off the wire; there is no SW9 on this DK |
| VCOM1 pins | P0.29 = TX, P0.28 = RX, P0.16 = RTS, P0.17 = CTS |
| VCOM1 ceiling | 1 Mbaud, 81–83 kB/s lossless **with RTS/CTS** |
| Radio quality | RSSI −18.5 to −19 dBm, SNR 22–25 dB, zero errors |

### Choosing which kit is which

Not every kit's VCOM1 is equally good. Of the two used here, one garbles data at
1 Mbaud and is dead at 921600, while the other carries 83 kB/s for twenty
seconds with zero loss. Since the transmitter never touches VCOM1 and the
receiver depends on it entirely, test both before assigning roles and give the
better one to the receiver.

### Addressing the serial ports

Always through `/dev/serial/by-id/`, never as `/dev/ttyACMx`: the numbering
changes on replug — and, under WSL, on every usbip attach — after which two
identical DKs swap places.

Each DK exposes two CDC interfaces:

```
...-if00    VCOM0, the console
...-if02    VCOM1, the video stream on the receiver
```

So the receiver's data port is the J-Link `-if02` belonging to whichever kit you
gave that role. The viewer finds it automatically when only one DK is attached,
and asks for `--port` when the choice is ambiguous.

### Radio band

Band 1 (Europe, 1880–1900 MHz) is channels **1657–1677, odd numbers only**
(ETSI EN 301 406-2 V3.0.1 §4.3.2.3 — odd-only gives the 1.728 MHz carrier
spacing on an 0.864 MHz numbering raster).

```
f = 1881.792 MHz + (n − 1657) × 0.864 MHz
1657 → 1881.792 MHz   …   1665 → 1888.704 MHz   …   1677 → 1899.072 MHz
```

Project default: **carrier 1665 = 1888.704 MHz**.

Channel ranges per band, from
`nrf/samples/dect/dect_phy/dect_shell/src/dect/common/dect_common.h`:

| Band | Channels | Region |
|---|---|---|
| 1 | 1657–1677 | Europe, 1880–1900 MHz ← **ours** |
| 2 | 1680–1700 | ~1.9 GHz |
| 4 | 525–551 | ~1 GHz |
| 9 | 1703–1711 | ~1.9 GHz |
| 22 | 1691–1711 | ~1.9 GHz |

### `band_group_index`

`nrf_modem_dect_phy_config_params.band_group_index` selects the RF front end:

* **0** — frequencies near **2 GHz** ← what band 1 needs
* **1** — frequencies near **1 GHz** (band 4)

The `hello_dect` sample computes this as
`(CONFIG_CARRIER >= 525 && CONFIG_CARRIER <= 551) ? 1 : 0`, i.e. group 1 only
for band 4. Its Kconfig help calls this "Band index 1", which reads as if it
meant *band* 1 — it does not, it means band *group* index. For carrier 1665 the
expression correctly yields **0**.

### DECT PDC transport block sizes

From `dect_common_utils.c` (ETSI TS 103 636-3 §5.3), µ=1, β=1, bits:

| MCS | 1 subslot | 2 | 4 | 8 | 15 | max usable |
|---|---|---|---|---|---|---|
| 0 | 0 | 264 | 536 | 1064 | 1992 | 249 B @16 |
| 1 | 32 | 552 | 1096 | 2104 | 3960 | 495 B @16 |
| 2 | 56 | 856 | 1640 | 3192 | 5536 | 692 B @15 |
| 3 | 88 | 1128 | 2168 | 4256 | — | 660 B @11 |
| 4 | 144 | 1736 | 3256 | **5600** | — | **700 B @8** |

Best payload per transmission: **700 B at MCS 4, 8 subslots**
(= 1.667 ms air time, ≈ 3.4 Mbps raw). Minus the 13-byte protocol header that
leaves **687 B** of JPEG per fragment.

## Throughput budget

The radio is not the bottleneck and never has been. The receiver's USB path is.

```
ESP32 → nRF TX  :  1 Mbaud UART            ≈ 97 kB/s   (8N1, minus framing)
nRF TX → nRF RX :  DECT MCS4 / 8 subslots  ≥ 64 kB/s   observed, 0 errors
nRF RX → PC     :  1 Mbaud VCOM1 + RTS/CTS ≈ 83 kB/s   measured, lossless
                                             ────────
end-to-end ceiling                           ≈ 83 kB/s
```

Measured operating points, all with **zero frame loss** end to end:

| Resolution | Quality | JPEG/frame | Fragments | fps | Link used |
|---|---|---|---|---|---|
| QVGA 320×240 | q=6 ← **default** | 2.6 kB | 4 | **14.8** | 48 % |
| QVGA 320×240 | q=12 | 2.4 kB | 4 | 14.6 | 43 % |
| HVGA 480×320 | q=12 | 3.9 kB | 6 | 6.5 | 71 % |
| VGA 640×480 | q=12 | ~20 kB | ~30 | 1.1 | not usable live |

Resolution and JPEG quality are runtime-selectable, so the operating point can
be moved without reflashing. See [`results.md`](results.md) for the full
measurements and what each change was worth.

## Design decisions

**No retransmission.** This is live video. A fragment that arrives late is worth
less than the next frame. Frames with a missing fragment or a bad CRC are
dropped whole at the viewer and counted in the HUD.

**Frame-boundary resynchronisation.** Every fragment carries `frame_id` and
`frag_idx`, so the reassembler recovers on the next frame after any loss
without a handshake.

**Control travels against the video, all the way from the PC.** The operator is
at the receiver, so commands go PC → receiver → air → transmitter, and the
transmitter either applies them to its own radio or passes them to the camera
on the UART back-channel:

```
PC  --USB-->  receiver  --DECT-->  transmitter  --UART-->  camera
```

Every hop already existed except the middle one. Adding it meant giving the
transmitter a receive path and the receiver a transmit path, which is the bulk
of the complexity:

* The transmitter cannot listen continuously — it is busy sending video, and
  turning the radio around costs air time. It opens a 30 ms window every
  200 ms, so the receiver sends each telegram as a burst of copies 25 ms apart
  and a sequence number makes the repeats harmless.
* The receiver has one radio, so transmitting means not receiving. It is deaf
  for the second its burst lasts, which is why the transmitter repeats its
  acknowledgement for well past the end of it.

Acknowledgements ride the video stream rather than needing a second radio
turnaround.

The alternative — driving the camera only from the transmitter's own shell —
worked, but put the controls on a different machine from the picture.

**Nobody waits for the other end when changing the carrier.** It is the one
command that can lose the link outright, so both ends move on their own timers
and return on their own if no traffic appears, the receiver first. See
[`protocol.md`](protocol.md).

## Regulatory note

Streaming video is a near-continuous transmit duty cycle. The NCS DECT samples
do **not** implement the channel-access rules of ETSI EN 301 406-2. An LBT
(listen-before-talk, RSSI measure before TX) gate is provided as a Kconfig
option on the TX. **This is lab equipment; it is the operator's responsibility
to confirm local compliance before transmitting.**
