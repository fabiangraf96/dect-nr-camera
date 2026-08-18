# nRF firmware

Two Zephyr applications: a **transmitter** that reads framed video from the
camera board over UART and puts it on the air, and a **receiver** that takes it
off the air and hands it to a PC over USB.

Two boards are supported and neither replaces the other: the **nRF9151 DK** and
the **nRF9131 EK**. The camera board and its firmware are identical for both.
Everything that differs is in `<app>/boards/`, one overlay and one `.conf` per
board — see `docs/wiring.md`.

```
tx/         transmitter
rx/         receiver
common/     shared by both, and with the other two parts of the project
  dectcam_proto.h    wire format (reference implementation)
  dectcam_tbs.h      DECT transport block size tables
  dectcam_cfg.h      derived link parameters + build-time validation
  Kconfig.dectcam    radio settings shared by both applications
  test_proto.c       host self-test for the protocol
tools/pinprobe/      one-off utility for identifying header pins
```

The settings that must match at both ends — carrier, network ID, MCS, packet
length — live in a single `Kconfig.dectcam` that both applications source, so
they cannot drift apart.

## Prerequisites

* **nRF Connect SDK v3.3.1** (Zephyr 4.3.99). Other versions are likely to work
  but are untested; the DECT PHY API is still marked experimental and has moved
  between releases.
* **DECT NR+ modem firmware** on both nRF9151 devices. This is a separate
  download from Nordic Semiconductor and is not included here. Without it the
  radio will not come up.
* A working `west` environment. Follow Nordic's installation guide, then either
  activate the toolchain with `nrfutil toolchain-manager launch --shell` or
  source your SDK's `zephyr-env.sh`.

## Building

```bash
# nRF9151 DK
west build -b nrf9151dk/nrf9151/ns -d build/tx-9151dk tx
west build -b nrf9151dk/nrf9151/ns -d build/rx-9151dk rx

# nRF9131 EK
west build -b nrf9131ek/nrf9131/ns -d build/tx-9131ek tx
west build -b nrf9131ek/nrf9131/ns -d build/rx-9131ek rx
```

The `/ns` variant is required: the applications run in the non-secure domain
with TF-M.

Keep the build directories apart. Sharing one means a rebuild after switching
boards silently reuses the previous board's CMake cache.

An illegal MCS / subslot combination fails the **build**, with a message
explaining the constraint, rather than turning into a runtime modem error.

## Flashing

The two kits are told apart by J-Link serial number. List what is attached:

```bash
nrfutil device list
```

Then flash each image to the right board:

```bash
west flash -d build/tx --dev-id <transmitter-serial>
west flash -d build/rx --dev-id <receiver-serial>
```

Two things worth knowing:

* **Check the receiver's VCOM1 before committing to a role.** The whole video
  path depends on it, and quality varies between kits — one of the two used
  during development is measurably worse, garbling data at 1 Mbaud. The
  transmitter never uses VCOM1, so a kit with a poor one is fine in that role.
* **Do not pass `--erase`.** `ERASEALL` times out on these boards, and a
  sectored program is both faster and keeps well clear of the modem firmware.

## Configuration

In `common/Kconfig.dectcam`; these **must match on both boards**:

| Option | Default | Notes |
|---|---|---|
| `DECTCAM_CARRIER` | 1665 | 1888.704 MHz. Band 1 is 1657–1677, **odd only** |
| `DECTCAM_NETWORK_ID` | 91 | |
| `DECTCAM_MCS` | 4 | |
| `DECTCAM_PACKET_LEN_SUBSLOTS` | 8 | MCS 4 + 8 subslots = 700 B transport block |
| `DECTCAM_TX_POWER` | 13 | maximum; reduce for bench testing |

The camera's fragment size (`CONFIG_DECTCAM_FRAG_PAYLOAD`, default 687) must
equal the transport block size minus the 13-byte protocol header, so that one
fragment fills exactly one DECT packet.

Carrier numbering and the transport block size table are in
[`../docs/architecture.md`](../docs/architecture.md).

## Controlling the camera from the transmitter

The link to the camera is full duplex, and video only uses one direction. The
transmitter's shell (on VCOM0, 115200 baud) forwards text over the other:

```
dectcam-tx:~$ cam quality 4      # 4..63, lower is better
dectcam-tx:~$ cam res hvga
dectcam-tx:~$ camraw <text>      # send a line verbatim
dectcam-tx:~$ dectstat           # link and radio counters
```

No reply comes back — the return direction carries video and must not be
polluted with text. Confirmation arrives as a metadata packet, which the viewer
displays.

## Testing without a camera

The transmitter can generate its own frames, so the radio link can be measured
with nothing wired to it. Enable `CONFIG_DECTCAM_TX_TEST_PATTERN` and rebuild.

Measured this way on the bench (MCS 4, 8 subslots, boards ~30 cm apart):

```
TX  : 89.9 pkt/s, 62.9 kB/s, 0 errors
RX  : 90.1 pkt/s, 63.1 kB/s, 0 PDC CRC errors, SNR 24.75 dB
```

## Notes that cost debugging time

* **`CONFIG_UART_<n>_ASYNC=y` must be stated explicitly.** Enabling the shell
  pulls in `UART_INTERRUPT_DRIVEN`, which defaults *every* UART instance to
  interrupt-driven and silently disables the async API on the link UART. The
  symptom is a receive path that compiles, runs, and never delivers a byte.
  The symbol carries the instance number, so it lives in `boards/<board>.conf`.
* **`uart1` is disabled** in both boards' non-secure devicetrees because TF-M
  claims it for secure logging. The link UART is therefore borrowed from
  another instance and routed to the pads it needs with pinctrl — the nRF91 pin
  matrix allows any UARTE instance on any pin.
* **Which instance is free differs between the boards.** On the nRF91 each
  instance number is shared by SPIM, SPIS, TWIM, TWIS and UARTE, so only one of
  them can exist at a time. The DK uses `uart2`; the EK cannot, because
  instance 2 is its nPM1300 PMIC `i2c2`, so it uses `uart3` and disables the
  `spi3` that would otherwise own instance 3. The C never names an instance —
  the overlay sets `chosen { dectcam,link-uart }` and the source follows it.
* **Hardware flow control on VCOM1 is mandatory on the DK and absent on the
  EK.** On the DK, 1 Mbaud without it loses 13 % of all bytes. The EK does not
  connect RTS/CTS to VCOM1 at all: enabling it there stalls the receiver
  permanently, with the outbound queue backing up and nothing reaching the PC.
* **`CONFIG_CBPRINTF_FP_SUPPORT=y` is required**, or Zephyr renders every `%f`
  in a log message as the literal `*float*`, quietly destroying the rate
  statistics.
* **A stuck UART receiver survives a power cycle.** If the transmitter stops
  sending and its receive counters sit at zero, reflashing it clears the state
  where unplugging it does not.
