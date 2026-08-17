# Viewer

Live display for the DECT NR+ camera link. Reads the framed packet stream from
the receiver DK's VCOM1 port, reassembles JPEG frames, and shows them with a
link-quality overlay.

## Setup

```bash
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
```

> Use `opencv-python`, **not** `opencv-python-headless`. The headless wheel
> reports `GUI: NONE` and cannot open a window.

## Running

Two front ends over the same machinery.

**With the control panel** — picture on the left, dropdowns on the right for
carrier, MCS, transmit power, resolution, frame rate and JPEG quality:

```bash
./run_panel.sh                               # autodetects the port
./run_panel.sh --port /dev/ttyACM2 --scale 3
./run_panel.sh --replay capture.bin          # controls are inert
```

Tk is used here rather than OpenCV's own window, because OpenCV has no
dropdowns — only trackbars — and a row of sliders labelled 0..7 is a poor way
to choose a resolution. On Debian and Ubuntu `tkinter` needs the `python3-tk`
package, even though it is part of the standard library.

**Picture only**, no panel:

```bash
./run_viewer.sh                              # autodetects the port
./run_viewer.sh --port /dev/ttyACM2 --scale 3
./run_viewer.sh --no-gui                     # statistics only, no window
./run_viewer.sh --replay capture.bin --replay-rate 60
./run_viewer.sh --record session.bin         # also save the raw stream
```

The video is on **VCOM1**, the *second* serial port the receiver DK enumerates;
VCOM0 is the console and carries no video. The port is found automatically
under `/dev/serial/by-id/` by looking for a J-Link `-if02` interface.

With both DKs attached that is ambiguous — the transmitter also exposes a
VCOM1, and it looks identical from the host — so the viewer refuses to guess
and lists what it found:

```
Several DK data ports are attached; name one with --port:
  /dev/serial/by-id/usb-SEGGER_J-Link_00<serial-a>-if02
  /dev/serial/by-id/usb-SEGGER_J-Link_00<serial-b>-if02
```

Pick the receiver's. `nrfutil device list` maps serial numbers to boards if you
are unsure which is which.

Address ports by their `by-id` path rather than `/dev/ttyACMx`: the numbering
changes whenever devices are re-enumerated, and two identical DKs then swap
places.

The viewer opens the port **exclusively**. Stop it before running anything else
that reads the same stream.

Hardware flow control is on by default and should stay on: measured on this
hardware, 1 Mbaud without RTS/CTS loses 13 % of bytes, with it none.

### Keys

| Key | Action |
|---|---|
| `q` / `Esc` | quit |
| `h` | toggle the overlay |
| `s` | save the current frame as PNG |
| `r` | reset the rate statistics |

### What the overlay shows

```
14.8 fps    39.9 kB/s
320x240  q6  target 15 fps
frames 177  lost 0 (0.0%)
rssi -18.5 dBm  snr 23.75 dB
```

The metadata packet the camera sends once a second carries resolution, quality
and target frame rate, so the overlay follows changes made with `cam res` or
`cam quality` without being told separately.

RSSI and SNR come from the receiver in the modem's own representation — RSSI in
0.5 dBm steps, SNR in 0.25 dB steps — so nothing is rounded on the way to the
display.

If the stream stops, the picture is dimmed and marked **NO SIGNAL** with the
age of the last frame. It also distinguishes two cases, which is worth knowing:
the receiver keeps sending its own statistics packets even when the air is
silent, so *"receiver alive, transmitter silent"* and *"nothing from the
receiver at all"* are different faults in different places.

## Offline analysis

```bash
./.venv/bin/python -m dectcam_viewer.analyze capture.bin --secs 10 --dump frames/
```

Reports packet-layer health (CRC errors, resyncs), frame-layer health (frames
completed versus dropped), the announced stream parameters, and JPEG size
statistics. `--dump` writes out the recovered images.

## Modules

| File | Purpose |
|---|---|
| `proto.py` | wire format — Python mirror of `dectcam_proto.h` |
| `reassembly.py` | fragments → complete JPEG frames |
| `source.py` | serial and file byte sources; the serial one also writes |
| `control.py` | issues control telegrams, matches acknowledgements to them |
| `panel.py` | live display with the control side panel |
| `viewer.py` | live display, no panel |
| `analyze.py` | offline capture analysis |
| `selftest.py` | cross-checks `proto.py` against the C reference vector |

`control.py` is deliberately separate from the window: the panel says "set the
carrier to 1667" and does not have to know about sequence numbers, retries, or
which commands can be acknowledged at all. Carrier changes cannot be — the far
end answers on a channel this end has not moved to yet — so success is judged
by watching where the link ends up.

## Self-test

```bash
./.venv/bin/python -m dectcam_viewer.selftest
```

Verifies the CRC check value, that the Python encoder reproduces the C
implementation's reference vector byte for byte, resynchronisation, corruption
detection, and the metadata and statistics round trips.

Run it together with the C side after any protocol change:

```bash
cc -o /tmp/test_proto ../nrf/common/test_proto.c && /tmp/test_proto
```

## Design note: no retransmission

This is live video. A frame missing any fragment is dropped whole and counted;
a late frame is worth less than the next fresh one. The `frame_id` and
`frag_idx` fields let the reassembler recover on the very next frame after any
loss, with no handshake.
