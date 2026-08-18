"""Live viewer for the DECT NR+ camera link.

Reads the framed packet stream from the receiver's VCOM1 port, reassembles
JPEG frames and displays them with a link-quality overlay.

    python3 -m dectcam_viewer.viewer --port /dev/ttyACM3
    python3 -m dectcam_viewer.viewer --replay capture.bin --replay-rate 60

Keys:
    q / ESC   quit
    h         toggle the overlay
    s         save the current frame as a PNG
    r         reset the statistics
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import time
from collections import deque
from typing import Optional

import cv2
import numpy as np

from . import portfind, proto
from .reassembly import Reassembler
from .source import SerialSource, FileSource

# Where to look for the receiver's VCOM1.
#
# Ports are found under /dev/serial/by-id/ rather than as /dev/ttyACMx,
# because the ttyACM numbering changes whenever the devices are re-enumerated
# -- a replug is enough -- and two identical DKs then swap places.
#
# The nRF91 DKs expose two CDC interfaces per board: if00 is VCOM0, the
# console, and if02 is VCOM1, which carries the video. So the port we want is
# a J-Link "-if02".
BY_ID_DIR = "/dev/serial/by-id"
RX_PORT_GLOB = "usb-SEGGER_J-Link_*-if02"


def find_rx_port() -> Optional[str]:
    """Locate the port with video on it, or None if that is ambiguous.

    This used to glob for VCOM1 and take it if there was exactly one. That
    stopped being right twice over: the nRF9131 EK sends video over VCOM0
    rather than VCOM1, and with two kits attached the transmitter's ports look
    identical to the receiver's. See portfind.
    """
    return portfind.find_rx_port()

# How long without a decoded frame before the picture is called stale.
#
# Comfortably longer than any plausible gap at the operating frame rates, and
# short enough that a dead link is obvious rather than being mistaken for a
# very still scene.
STALE_AFTER_S = 2.0


class Rate:
    """Sliding-window rate estimate."""

    def __init__(self, window: float = 3.0) -> None:
        self._ev: deque[tuple[float, float]] = deque()
        self._window = window

    def add(self, amount: float = 1.0) -> None:
        now = time.monotonic()
        self._ev.append((now, amount))
        cutoff = now - self._window
        while self._ev and self._ev[0][0] < cutoff:
            self._ev.popleft()

    @property
    def per_sec(self) -> float:
        if len(self._ev) < 2:
            return 0.0
        span = self._ev[-1][0] - self._ev[0][0]
        if span <= 0:
            return 0.0
        return sum(a for _, a in self._ev) / span

    def reset(self) -> None:
        self._ev.clear()


def draw_hud(img, lines, alpha: float = 0.55):
    """Draw text lines on a translucent panel in the top-left corner."""
    if not lines:
        return img
    font = cv2.FONT_HERSHEY_SIMPLEX
    scale = 0.42
    thick = 1
    pad = 6
    lh = 16

    w = max(cv2.getTextSize(t, font, scale, thick)[0][0] for t, _ in lines) + 2 * pad
    h = lh * len(lines) + 2 * pad

    overlay = img.copy()
    cv2.rectangle(overlay, (0, 0), (w, h), (0, 0, 0), -1)
    cv2.addWeighted(overlay, alpha, img, 1 - alpha, 0, img)

    y = pad + 11
    for text, colour in lines:
        cv2.putText(img, text, (pad, y), font, scale, colour, thick, cv2.LINE_AA)
        y += lh
    return img


def placeholder(msg: str, w: int = 480, h: int = 320):
    img = np.zeros((h, w, 3), np.uint8)
    img[:] = (28, 28, 32)
    cv2.putText(img, msg, (18, h // 2), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (90, 180, 240), 1,
                cv2.LINE_AA)
    return img


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src_grp = ap.add_mutually_exclusive_group()
    src_grp.add_argument("--port", default=None,
                         help="serial port of the receiver's VCOM1; autodetected "
                              f"from {BY_ID_DIR} when exactly one DK is attached")
    src_grp.add_argument("--replay", metavar="FILE", help="replay a raw capture instead")
    ap.add_argument("--baud", type=int, default=1_000_000)
    ap.add_argument("--no-rtscts", action="store_true",
                    help="disable hardware flow control (NOT recommended: measured "
                         "13%% byte loss without it)")
    ap.add_argument("--replay-rate", type=float, default=None,
                    help="pace replay at this kB/s")
    ap.add_argument("--scale", type=float, default=2.0, help="display magnification")
    ap.add_argument("--record", metavar="FILE", help="also append the raw byte stream here")
    ap.add_argument("--no-gui", action="store_true",
                    help="decode and print statistics only, no window")
    ap.add_argument("--save-dir", default=".", help="where the 's' key writes PNGs")
    args = ap.parse_args(argv)

    # Open the source.
    if args.replay:
        source = FileSource(args.replay, rate_kBps=args.replay_rate)
        print(f"replaying {args.replay}")
    else:
        port = args.port or find_rx_port()
        if port is None:
            found = portfind.candidates()
            print("no port is sending video.", file=sys.stderr)
            if found:
                print("\nListened on each of these and heard nothing framed:",
                      file=sys.stderr)
                for f in found:
                    print(f"  {f}", file=sys.stderr)
                print("\nThe video port is the RECEIVER's, and which one that is "
                      "depends on the kit:\n"
                      "  nRF9151 DK -> VCOM1, the -if02\n"
                      "  nRF9131 EK -> VCOM0, the -if00   (it wires flow control "
                      "there, not on VCOM1)\n"
                      "The transmitter's ports look identical from here and carry "
                      "no video.\n"
                      "Name one explicitly with --port, or check that the camera "
                      "is running.", file=sys.stderr)
            else:
                print(f"\nNo J-Link serial ports under {BY_ID_DIR}.", file=sys.stderr)
                print("Check that the receiver is plugged in, and on WSL that it "
                      "has been\nforwarded with usbipd.", file=sys.stderr)
            return 1

        try:
            source = SerialSource(port, args.baud, rtscts=not args.no_rtscts)
        except Exception as e:
            print(f"could not open {port}: {e}", file=sys.stderr)
            print("\nThe video stream is on VCOM1, the SECOND serial port the DK\n"
                  "enumerates. VCOM0 is the console and carries no video.",
                  file=sys.stderr)
            return 1
        print(f"reading {port} @ {args.baud} baud, rtscts={not args.no_rtscts}")

    rec = open(args.record, "ab") if args.record else None

    dec = proto.Decoder()
    rea = Reassembler()

    fps = Rate()
    byte_rate = Rate()

    last_frame = None
    last_frame_at = None
    last_pkt_at = None
    show_hud = True
    t_start = time.monotonic()
    last_print = t_start
    saved = 0

    win = "DECT NR+ camera"
    if not args.no_gui:
        cv2.namedWindow(win, cv2.WINDOW_NORMAL)
        cv2.resizeWindow(win, 640, 480)

    try:
        while True:
            data = source.read(timeout=0.05)
            if data:
                byte_rate.add(len(data))
                if rec:
                    rec.write(data)
                for pkt in dec.feed(data):
                    last_pkt_at = time.monotonic()
                    for frame in rea.push(pkt):
                        img = cv2.imdecode(np.frombuffer(frame.jpeg, np.uint8),
                                           cv2.IMREAD_COLOR)
                        if img is not None:
                            last_frame = img
                            last_frame_at = time.monotonic()
                            fps.add()

            if isinstance(source, FileSource) and source.exhausted and not data:
                print("replay finished")
                break

            if args.no_gui:
                now = time.monotonic()
                if now - last_print >= 1.0:
                    print(f"{fps.per_sec:5.1f} fps  {byte_rate.per_sec/1000:6.1f} kB/s  "
                          f"frames {rea.n_frames_ok} lost {rea.n_frames_dropped} "
                          f"crc {dec.n_crc_err} resync {dec.n_resync}")
                    last_print = now
                continue

            # ---- display ----
            #
            # A frozen picture is indistinguishable from a very still scene, so
            # say so explicitly once the stream stops. Without this the viewer
            # keeps showing the last frame it ever decoded, which reads as "the
            # link is fine" when in fact nothing is arriving.
            stale_for = 0.0
            if last_frame_at is not None:
                stale_for = time.monotonic() - last_frame_at
            stale = last_frame is not None and stale_for >= STALE_AFTER_S

            if last_frame is None:
                canvas = placeholder("waiting for video...")
            else:
                canvas = last_frame
                if args.scale != 1.0:
                    canvas = cv2.resize(
                        canvas, None, fx=args.scale, fy=args.scale,
                        interpolation=cv2.INTER_NEAREST)
                else:
                    canvas = canvas.copy()

                if stale:
                    # Dim it, so a glance is enough to tell the picture is old.
                    canvas = (canvas * 0.35).astype(np.uint8)
                    h, w = canvas.shape[:2]
                    txt = "NO SIGNAL"
                    sub = f"last frame {stale_for:.0f} s ago"
                    (tw, th), _ = cv2.getTextSize(txt, cv2.FONT_HERSHEY_SIMPLEX, 1.1, 2)
                    (sw, sh), _ = cv2.getTextSize(sub, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
                    cv2.putText(canvas, txt, ((w - tw) // 2, (h - th) // 2),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.1, (110, 110, 250), 2,
                                cv2.LINE_AA)
                    cv2.putText(canvas, sub, ((w - sw) // 2, (h - th) // 2 + sh + 14),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1,
                                cv2.LINE_AA)

            if show_hud:
                white = (235, 235, 235)
                green = (120, 240, 120)
                amber = (110, 200, 250)
                red = (110, 110, 250)

                loss = rea.loss_pct
                loss_col = green if loss < 1 else (amber if loss < 10 else red)

                lines = [
                    (f"{fps.per_sec:5.1f} fps   {byte_rate.per_sec/1000:6.1f} kB/s",
                     red if stale else white),
                ]
                if stale:
                    lines.append((f"NO VIDEO for {stale_for:.0f} s", red))
                    # Distinguishing these two saves a diagnostic session: the
                    # receiver keeps sending its own STATS packets even when
                    # the air is silent, so packets still arriving means the
                    # receiver and the USB path are fine and the transmitter
                    # is the problem.
                    if last_pkt_at is not None and \
                            time.monotonic() - last_pkt_at < STALE_AFTER_S:
                        lines.append(("receiver alive, transmitter silent", amber))
                    else:
                        lines.append(("nothing from the receiver at all", red))
                if rea.meta:
                    m = rea.meta
                    lines.append((f"{m.width}x{m.height}  q{m.jpeg_quality} "
                                  f"target {m.target_fps} fps", white))
                lines.append((f"frames {rea.n_frames_ok}  lost {rea.n_frames_dropped} "
                              f"({loss:.1f}%)", loss_col))
                lines.append((f"crc err {dec.n_crc_err}  resync {dec.n_resync}",
                              green if dec.n_crc_err == 0 else amber))

                if rea.stats:
                    s = rea.stats
                    r = s.rssi_dbm
                    n = s.snr_db
                    rtxt = f"{r:.1f} dBm" if r is not None else "n/a"
                    ntxt = f"{n:.2f} dB" if n is not None else "n/a"
                    lines.append((f"rssi {rtxt}   snr {ntxt}", white))
                    tot = s.pdc_ok + s.pdc_crc_err
                    per = 100.0 * s.pdc_crc_err / tot if tot else 0.0
                    lines.append((f"pdc ok {s.pdc_ok}  crc {s.pdc_crc_err} ({per:.2f}%)",
                                  green if per < 1 else amber))

                if getattr(source, "overruns", 0):
                    lines.append((f"host overruns {source.overruns}", red))

                draw_hud(canvas, lines)

            cv2.imshow(win, canvas)
            key = cv2.waitKey(1) & 0xFF

            if key in (ord("q"), 27):
                break
            elif key == ord("h"):
                show_hud = not show_hud
            elif key == ord("r"):
                fps.reset()
                byte_rate.reset()
            elif key == ord("s") and last_frame is not None:
                os.makedirs(args.save_dir, exist_ok=True)
                path = os.path.join(args.save_dir, f"dectcam_{saved:04d}.png")
                cv2.imwrite(path, last_frame)
                print(f"saved {path}")
                saved += 1

            # Window closed with the title-bar X.
            #
            # Once the window is gone the Qt backend has already torn down its
            # GUI receiver, and this query raises rather than reporting the
            # window as invisible. Closing the window would then look like a
            # crash, complete with a traceback, which is a poor way to say
            # "you closed the window".
            try:
                if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                    break
            except cv2.error:
                break

    except KeyboardInterrupt:
        pass
    finally:
        source.close()
        if rec:
            rec.close()
        if not args.no_gui:
            cv2.destroyAllWindows()

    el = time.monotonic() - t_start
    print(f"\n--- session summary ({el:.1f} s) ---")
    print(f"packets ok   : {dec.n_packets}")
    print(f"crc errors   : {dec.n_crc_err}")
    print(f"resyncs      : {dec.n_resync}")
    print(f"frames ok    : {rea.n_frames_ok}  ({rea.n_frames_ok/el:.1f} fps avg)")
    print(f"frames lost  : {rea.n_frames_dropped} ({rea.loss_pct:.1f} %)")
    print(f"bad jpeg     : {rea.n_bad_jpeg}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
