# Wiring — Freenove ESP32-S3 ↔ nRF9151 DK (transmitter)

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

## Receiver board

Nothing to wire. Connect the RX DK to the PC with USB-C; the video stream
arrives on **VCOM1**, the *second* serial port the DK enumerates.

Three USB devices end up on the host:

| Device | Enumerates as | Carries |
|---|---|---|
| Transmitter DK | J-Link, two CDC ports | VCOM0 console; VCOM1 unused |
| Receiver DK | J-Link, two CDC ports | VCOM0 console; **VCOM1 = video** |
| Camera board | USB-serial bridge | console and flashing |

Address them through `/dev/serial/by-id/`, never `ttyACMx` — the numbering
changes on every re-enumeration. `-if00` is VCOM0 and `-if02` is VCOM1, so the
video port is the J-Link `-if02` of whichever kit you made the receiver.

The PC side **must** open VCOM1 with RTS/CTS enabled. Without it, measurements
on this hardware showed 13 % byte loss at 1 Mbaud; with it, zero.

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
