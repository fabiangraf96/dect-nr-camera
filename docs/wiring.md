# Wiring — Freenove ESP32-S3 ↔ nRF9151 DK or nRF9131 EK (transmitter)

Two radio boards are supported and neither replaces the other. **The camera
board and its firmware are identical for both**; only the pin numbers at the
nRF end change, and those live in the per-board overlay.

| | nRF9151 DK | nRF9131 EK |
|---|---|---|
| Board target | `nrf9151dk/nrf9151/ns` | `nrf9131ek/nrf9131/ns` |
| Link UART | `uart2` | `uart3` (instance 2 is the PMIC's i2c) |
| TX header pins | Arduino D2/D3/D6/D7 | `P0.13`–`P0.16`, left header |
| RX → PC | **VCOM1** (`-if02`) | **VCOM0** (`-if00`) — VCOM1 has no RTS/CTS here |
| RX console | VCOM0 | RTT (VCOM0 carries the video) |

Jump to [nRF9131 EK](#nrf9131-ek-transmitter) if that is the board on the bench.

## Summary

Four signal wires plus ground. UART at 1 Mbaud with hardware flow control in
both directions. Each board is powered from its own USB-C cable.

**All five connections are on the ESP32-S3's right-hand header**, so the module
can sit in a breadboard with only that side reachable.

| # | ESP32-S3 pin | Signal | nRF9151 DK pin | nRF9151 GPIO | In-line part |
|---|---|---|---|---|---|
| 1 | `GPIO42` | video, ESP32 TX → nRF RX | Arduino **D2** | `P0.02` | 1 kΩ series |
| 2 | `GPIO21` | commands, ESP32 RX ← nRF TX | Arduino **D3** | `P0.03` | — |
| 3 | `GPIO1` | ESP32 CTS ← nRF RTS | Arduino **D6** | `P0.06` | — |
| 4 | `GPIO47` | ESP32 RTS → nRF CTS | Arduino **D7** | `P0.07` | 1 kΩ series |
| 5 | `GND` | ground | any `GND` | — | — |

```
     Freenove ESP32-S3 WROOM CAM              nRF9151 DK (TX)
    ┌────────────────────────┐              ┌────────────────────┐
    │ GPIO42 ○──[1k]─────────┼─── video ───►│○ D2  (P0.02)  RX   │
    │ GPIO21 ○◄──────────────┼─── commands ─│○ D3  (P0.03)  TX   │
    │ GPIO1  ○◄──────────────┼─── RTS ──────│○ D6  (P0.06)  RTS  │
    │ GPIO47 ○──[1k]─────────┼─── CTS ─────►│○ D7  (P0.07)  CTS  │
    │ GND    ○───────────────┼─── ground ───│○ GND               │
    │                        │              │                    │
    │ USB-C (marked UART)    │              │ USB-C to the PC    │
    │ own supply + console   │              │ VDD_IO = 3.0 V     │
    └────────────────────────┘              └────────────────────┘
```

Order of the four signals down the right-hand header, so they are easy to find:

```
    GPIO43  console TX
    GPIO44  console RX
    GPIO1   <- CTS          wire 3
    GPIO2
    GPIO42  <- TX           wire 1
    GPIO41
    ... microSD, PSRAM, straps ...
    GPIO47  <- RTS          wire 4
    GPIO21  <- RX           wire 2
    GPIO20
    GPIO19
    GND     <- ground       wire 5
```

## Before connecting anything

1. **Check the DK's VDD_IO rail — it must be 3.0 V, not 1.8 V.**

   The nRF9151 DK has **no SW9 switch**. That was the nRF9160 DK. On this board
   (PCA10171) the GPIO rail is produced by an on-board nPM1300 PMIC and is set
   in software, through the **Board Configurator** app in nRF Connect for
   Desktop. `nrfutil device` exposes no command for it; the only board
   controller operation it offers is `x-boardcontroller-program`, which
   replaces the firmware rather than changing a setting.

   Why it matters, in both directions:

   | Rail | ESP32 3.3 V → nRF input | nRF output → ESP32 input |
   |---|---|---|
   | 3.0 V | fine, at the VDD+0.3 V limit — hence the series resistors | 3.0 V > 2.475 V, read as high |
   | 1.8 V | clamped to ~2.1 V; **the 1 kΩ resistors are what keep this safe** | 1.8 V < 2.475 V, **never read as high** |

   At 1.8 V the video direction would still work, but the two return signals
   would not: the ESP32-S3 needs 0.75 × 3.3 V = 2.475 V to see a logic high.
   The back-channel would be dead and flow control would read as permanently
   "clear to send".

2. **Fit the 1 kΩ series resistors** in the two ESP32→nRF lines (wires 1 and 4).
   At 3.0 V they limit current into the pin protection diodes, which sit right
   at the absolute-maximum rating of VDD+0.3 V. At 1.8 V they are the only
   thing preventing roughly 30 mA of clamp current, which is well beyond the
   ±5 mA a GPIO tolerates. **Do not omit them.**

   The two nRF→ESP32 lines need no resistor: a 3.0 V output into a 3.3 V input
   is a valid logic high with margin to spare.

3. **Do not power the ESP32-S3 from the DK.** It has its own USB-C socket,
   which also carries the console and the flashing protocol. Both boards must
   still share a ground — wire 5 is not optional.

## Why these ESP32-S3 pins

The GPIO matrix on the S3 puts any peripheral on any pin, so the choice is
driven by what is left over — and by which header the pin sits on.

Already taken:

| Pins | Function |
|---|---|
| 4, 5 | SCCB (I²C) to the sensor |
| 6, 7, 13, 15 | VSYNC, HREF, PCLK, XCLK |
| 8–12, 16–18 | 8-bit parallel video data |
| 19, 20 | native USB |
| 35–37 | octal PSRAM |
| 38–40 | microSD |
| 43, 44 | UART0 to the CH343 console bridge |

Excluded on purpose:

| Pins | Reason |
|---|---|
| 0, 3, 45, 46 | boot strapping |
| 2, 48 | on-board LEDs |

`GPIO14` would be the obvious choice for TX and was used first, but it sits on
the **left-hand** header while CTS, RTS, RX and GND are all on the right. In a
breadboard the module covers the left header completely, so that pin cannot be
reached at all.

`GPIO42` is on the right-hand header, is one of the JTAG pads (MTMS), and has
no boot strapping function. The S3 debugs over its native USB rather than these
pads, so nothing is given up by using it.

`GPIO41` is equally free if `GPIO42` is ever awkward.

## Why these nRF9151 pins

Occupied on the DK:

* `P0.00`, `P0.01`, `P0.04`, `P0.05` — LEDs 1–4
* `P0.08`, `P0.09`, `P0.18`, `P0.19` — buttons 1–4
* `P0.26`, `P0.27` — uart0 → VCOM0 (console)
* `P0.28`, `P0.29`, `P0.16`, `P0.17` — VCOM1 (used on the **RX** board)

`P0.02`, `P0.03`, `P0.06`, `P0.07` are free and map to Arduino header pins
D2, D3, D6, D7.

> On the **TX** board the VCOM1 pins are unused, so uart2 is free to be routed
> to the Arduino header. On the **RX** board uart2 goes to the VCOM1 pins
> instead. Same peripheral, different pinctrl — see each app's board overlay.

## Flow control is wired for real now

The previous camera board, an AI-Thinker ESP32-CAM, had no spare boot-safe
output pin, so the nRF's CTS input was parked on `P0.07` with an internal
pull-down — permanently "clear to send", with D7 required to stay unconnected.
That workaround existed because the nRF UARTE has a single `CONFIG.HWFC` bit
covering RTS and CTS together and offers no RTS-only mode.

The ESP32-S3 has the pin, so wire 4 now carries a real signal. The pull-down on
`P0.07` is kept as a fallback: if that wire is ever missing, the input still
reads as asserted and the transmitter keeps working instead of floating.

**Flow control is not optional.** Without it, measurements on the equivalent
VCOM link showed 13 % of all bytes lost at 1 Mbaud. And with it enabled but
wire 3 missing, the ESP32 correctly refuses to transmit — the console then
reports:

```
link stalled at fragment 0/5 (33 drops); is the nRF9151 powered and CTS wired?
```

That message means the wiring, not the firmware.

## nRF9131 EK (transmitter)

Same four signals, same ESP32-S3 pins, same 1 kΩ resistors, same reasoning as
above. Only the far end moves.

**Everything is on the left-hand header, ground included** — the right-hand
header is not needed at all.

| # | ESP32-S3 pin | Signal | nRF9131 EK pin | In-line part |
|---|---|---|---|---|
| 1 | `GPIO42` | video, ESP32 TX → nRF RX | `P0.13` | 1 kΩ series |
| 2 | `GPIO21` | commands, ESP32 RX ← nRF TX | `P0.14` | — |
| 3 | `GPIO1` | ESP32 CTS ← nRF RTS | `P0.15` | — |
| 4 | `GPIO47` | ESP32 RTS → nRF CTS | `P0.16` | 1 kΩ series |
| 5 | `GND` | ground | `GND` between `P0.13` and `P0.14` | — |

The left header runs, top to bottom:

```
    GPIO2
    RST
    VDD
    P0.13   <- video RX      wire 1
    GND     <- ground        wire 5
    P0.14   <- commands TX   wire 2
    P0.15   <- RTS           wire 3
    P0.16   <- CTS           wire 4
    P0.17   ─┐
    P0.18    │ free, but P0.17 and P0.23-P0.25 are VCOM1
    P0.19    │
    P0.20   ─┤ spi3 (disabled by our overlay, so free)
    ...      │
    P0.31   ─┘ P0.28 button, P0.29-P0.31 LEDs
```

`P0.13`–`P0.16` are the only four *contiguous* free pins on this header, and
the header's own `GND` falls in the middle of them. The board spends `P0.17`
and `P0.23`–`P0.25` on VCOM1, `P0.20`–`P0.22` and `P0.26` on spi3, `P0.28` on
the button and `P0.29`–`P0.31` on the LEDs; that leaves `P0.13`–`P0.16`,
`P0.18`, `P0.19` and `P0.27`. The right-hand header carries `P0.00`–`P0.12`.

> **VDD_IO on the EK has not been measured.** The 3.0 V / 1.8 V argument above
> applies here too and the same no-meter diagnostic works — see
> [Telling a 3.0 V rail from a 1.8 V one](#telling-a-30-v-rail-from-a-18-v-one-without-a-meter).
> Fit the 1 kΩ resistors and the wiring is safe either way.

## Receiver board

Nothing to wire, on either board. Connect the RX kit to the PC with USB-C; the
video stream arrives on **VCOM1**, the *second* serial port it enumerates.

### On the EK the video leaves over VCOM0, not VCOM1

This is the one place the two kits genuinely differ, and it cost a long
session to find. **The EK wires RTS/CTS on VCOM0 only.** VCOM1 has none, and
without them the link is unusable.

The failure is not subtle but it is deceptive, because every other number looks
healthy. With video on VCOM1:

```
DECT CRC errors                     0
corrupt when the modem handed over  0     <- receiver checks its own CRC here
fragments queued to the PC        951
arrived at the PC                 441 good, 553 with a broken CRC
outbound ring high water mark       0 B
```

Clean in, clean out, ruined in between. The receiver verifies the application
CRC the instant the modem gives it the packet, so the data provably left the
board intact; roughly 60 % of it still arrived damaged.

It does not respond to any of the usual levers, which is what makes it worth
recording:

| Tried | Result |
|---|---|
| baud 1 M / 460800 / 115200 | all fail, and slower fails *harder* |
| fragment payload 687 → 128 B | no change (~58 % of packets bad either way) |
| DMA chunk 512 → 2048 B | no change |
| `UART_x_NRF_ASYNC_LOW_POWER=n` | no change |
| raising the data rate to fill the line | no consistent change |
| host `crtscts` on / off | no change |

The only packets that survived reliably were the small ones sent while the line
was otherwise idle — metadata and statistics, never a video fragment.

Moving the same firmware to VCOM0, whose pinctrl in the board's own devicetree
declares all four wires, fixes it completely:

```
1000 packets, 0 CRC errors, 1 resync
 192 frames,  0 lost (0.0 %), 13.7 fps at 44.7 kB/s
```

which is the DK's own figure. The receiver's console moves to RTT to make room,
and loses nothing by it: both virtual COM ports share one USB device, so the
console was starved into silence whenever video was flowing anyway.

On the DK nothing changes — video stays on VCOM1, where flow control is wired
and is the measured difference between 13.1 % loss and none.

Three USB devices end up on the host. The EK enumerates exactly like the DK —
`nrfutil device list` reports both as J-Link with `vcom: 0` and `vcom: 1`:

| Device | Enumerates as | Carries |
|---|---|---|
| Transmitter kit | J-Link, two CDC ports | VCOM0 console; VCOM1 unused |
| Receiver kit | J-Link, two CDC ports | VCOM0 console; **VCOM1 = video** |
| Camera board | USB-serial bridge | console and flashing |

The camera board has two USB-C sockets — the UART one behind a CH343 bridge and
the ESP32-S3's own native USB. Either can flash and both carry the console, but
they enumerate under completely different names, so `scripts/boards.sh` looks
for whichever is present rather than hard-coding one.

Address them through `/dev/serial/by-id/`, never `ttyACMx` — the numbering
changes on every re-enumeration. `-if00` is VCOM0 and `-if02` is VCOM1, so the
video port is the receiver kit's **`-if02` on a DK** and its **`-if00` on an
EK**. `scripts/boards.sh` resolves this from `BOARD`.

On the **DK**, the PC side **must** open VCOM1 with RTS/CTS enabled. Without it,
measurements on this hardware showed 13 % byte loss at 1 Mbaud; with it, zero.
On the **EK** it makes no difference, because the lines are not connected.

## Bring-up order

Do not connect everything at once.

1. Flash the camera board alone and confirm it runs: `stats` on its console
   should show the frame counter climbing. With flow control on and nothing
   wired yet, expect the stall message above — that is the expected result, not
   a fault.
2. Flash both DKs and confirm the DECT link with the transmitter's built-in
   test pattern, no camera involved.
3. Only then wire the two together and run the full chain.

Each hop is proven before it becomes a variable in the next one.

## Telling a 3.0 V rail from a 1.8 V one, without a meter

The wiring itself is the test, and it is safe either way as long as the 1 kΩ
resistors are fitted.

Run the full chain and look at what the return direction does:

| Observation | Verdict |
|---|---|
| Video arrives **and** a `cam quality 8` sent from the TX shell changes the picture | VDD_IO is 3.0 V — everything is working |
| Video arrives but `cam …` commands never take effect | The ESP32 is not seeing the nRF's 3.3 V threshold — VDD_IO is 1.8 V |
| The ESP32 console keeps printing `link stalled … is the nRF9151 powered and CTS wired?` | The CTS wire (4) is missing, or the DK is not powered |

The middle row is the diagnostic one: the video direction survives a 1.8 V rail
because the ESP32 drives it, but the two return signals cannot cross the
ESP32-S3's 2.475 V input threshold.

If that is what you see, set the rail to 3.0 V with the Board Configurator app
in nRF Connect for Desktop and repeat.

## Note on the sensor

This board shipped with an **OV3660**, not the OV2640 the older Freenove
documentation and the AI-Thinker board use:

```
camera: Camera PID=0x3660 VER=0x00 MIDL=0x00 MIDH=0x00
camera: Detected OV3660 camera
```

`esp32-camera` supports both, and nothing in this project depends on which one
is fitted. It does affect the sensor clock: see `CONFIG_DECTCAM_XCLK_HZ` in
`main/Kconfig.projbuild` for the measured frame rates.
