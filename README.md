# DECT NR+ live camera link

Live video from a camera module to a PC over a **DECT NR+** radio link built
from two nRF9151 development kits.

```
┌────────────────┐  UART 1 Mbaud  ┌──────────────┐   DECT NR+    ┌──────────────┐  VCOM1 1 Mbaud ┌─────────┐
│ ESP32-S3 + cam │ ─────────────► │ nRF9151 DK   │ ────────────► │ nRF9151 DK   │ ─────────────► │ PC      │
│ JPEG frames    │  framed frags  │ transmitter  │   1.9 GHz     │ receiver     │  framed frags  │ viewer  │
└────────────────┘                └──────────────┘               └──────────────┘                └─────────┘
```

The sensor produces JPEG, the camera board chops each frame into fragments
sized to exactly one DECT transport block, and both radio boards move those
fragments without ever looking inside them. All image handling lives at the two
ends, which keeps the radio firmware small and lets every hop be tested on its
own.

## Measured

End to end at QVGA, JPEG quality 6, boards about 30 cm apart:

```
291 frames, 0 lost, 14.6 fps, 41.1 kB/s
291/291 decoded, 0 packet CRC errors
RSSI -33 dBm, SNR 24 dB, 0 radio errors
```

Resolution and quality are changeable at runtime. See
[`docs/results.md`](docs/results.md) for the full measurements, the resolution
ladder, and the traps met along the way.

## Repository layout

| Path | What |
|---|---|
| [`nrf/`](nrf) | Transmitter and receiver firmware — Zephyr / nRF Connect SDK |
| [`esp32/`](esp32) | Camera firmware — ESP-IDF |
| [`viewer/`](viewer) | Reassembly and live display — Python + OpenCV |
| [`docs/`](docs) | Architecture, wire protocol, wiring, measurements |

The wire format is specified in [`docs/protocol.md`](docs/protocol.md).
`nrf/common/dectcam_proto.h` is the reference implementation; it is copied
verbatim into `esp32/main/` and mirrored in Python as
`viewer/dectcam_viewer/proto.py`, and a self-test checks the copies against one
another.

## Hardware

| | |
|---|---|
| Radio | 2 × nRF9151 DK, with DECT NR+ modem firmware installed |
| Camera | ESP32-S3 board with an OV2640 or OV3660 sensor and PSRAM |
| Wiring | 4 jumper wires plus ground, 2 series resistors |

Developed on a Freenove ESP32-S3 WROOM CAM, which is pin-compatible with
Espressif's ESP32-S3-EYE. Any ESP32-S3 camera board works if you adjust the pin
map in `esp32/main/cam.c`.

DECT NR+ operation needs the DECT PHY modem firmware on the nRF9151, which is
distributed separately by Nordic Semiconductor and is not part of this
repository.

See [`docs/wiring.md`](docs/wiring.md) before connecting anything — in
particular the two series resistors and the I/O rail voltage.

## Getting started

Each part builds with its own vendor toolchain; there is no top-level build.

```bash
# 1. radio firmware  (needs nRF Connect SDK)
cd nrf
west build -b nrf9151dk/nrf9151/ns -d build/tx tx
west build -b nrf9151dk/nrf9151/ns -d build/rx rx
west flash -d build/tx --dev-id <transmitter-serial>
west flash -d build/rx --dev-id <receiver-serial>

# 2. camera firmware  (needs ESP-IDF)
cd ../esp32
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash

# 3. viewer  (needs Python 3.10+)
cd ../viewer
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
./run_viewer.sh
```

Full instructions, including how to tell the two DKs apart and what to check
when nothing appears, are in each subdirectory's README.

## Runtime control

Everything adjustable is reachable from the PC, through a side panel next to
the picture:

```bash
cd viewer && ./run_panel.sh
```

| Radio | Camera |
|---|---|
| Carrier (1657–1677, odd) | Resolution |
| MCS | Target frame rate |
| Transmit power index | JPEG quality |

Commands travel against the video — PC → receiver → over the air → transmitter
— and the transmitter either applies them to its radio or passes them to the
camera. It announces its settings once a second, so the panel shows where the
link actually is rather than what was last asked for.

Changing the carrier moves both ends. Neither waits for the other: each retunes
on its own timer and returns to the previous channel by itself if no traffic
appears, so a command that only half arrives cannot strand them apart.

The same commands are available without the GUI:

```bash
scripts/send_ctrl.py carrier 1667
scripts/send_ctrl.py res hvga
scripts/read_state.py
```

and the transmitter's own shell still works for the camera:

```
dectcam-tx:~$ cam quality 4
dectcam-tx:~$ dectstat
```

## Regulatory note

Streaming video is a near-continuous transmit duty cycle. This firmware does
**not** implement the channel-access rules of ETSI EN 301 406-2; a
listen-before-talk gate is available as a build option but is not a substitute
for compliance work. This is lab equipment, and it is the operator's
responsibility to confirm what is permitted locally before transmitting.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
