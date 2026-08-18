"""Live viewer with a control side panel.

One window: the picture on the left, dropdowns on the right for everything
that can be changed while the link is running -- carrier, MCS, transmit power,
resolution, frame rate and JPEG quality.

    python3 -m dectcam_viewer.panel
    python3 -m dectcam_viewer.panel --port /dev/ttyACM2

Tk is used rather than OpenCV's own window because OpenCV has no dropdowns --
only trackbars -- and a row of sliders labelled 0..7 is a poor way to choose a
resolution.

Commands go out on the same serial port the video comes in on. Nothing here
knows how that works; see control.py.
"""

from __future__ import annotations

import argparse
import faulthandler
import glob
import os
import signal
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk
from typing import Optional

import cv2
import numpy as np
from PIL import Image, ImageTk

from . import proto
from .control import Controller
from .reassembly import Reassembler
from .source import SerialSource, FileSource

BY_ID = "/dev/serial/by-id"
RX_PORT_GLOB = "usb-SEGGER_J-Link_*-if02"

# Band 1 is 1657..1677, odd numbers only: the 0.864 MHz numbering raster
# carries 1.728 MHz channels, so every other number is a real channel.
CARRIERS = list(range(1657, 1678, 2))

# Transmit power is an index, not dBm; the mapping depends on band and modem
# firmware, so the panel offers the raw index and says so.
TX_POWERS = list(range(0, 14))

MCS_CHOICES = [0, 1, 2, 3, 4]
FPS_CHOICES = [1, 2, 5, 10, 15, 20, 25, 30]
QUALITY_CHOICES = [4, 6, 8, 10, 12, 16, 20, 25, 30, 40, 50, 63]

# How long a frame may be missing before the picture is called stale.
STALE_AFTER_S = 2.0


def find_rx_port() -> Optional[str]:
    matches = sorted(glob.glob(os.path.join(BY_ID, RX_PORT_GLOB)))
    return matches[0] if len(matches) == 1 else None


class Reader(threading.Thread):
    """Pulls bytes off the port and decodes them, off the UI thread.

    Tk redraws in the main thread; doing serial reads there makes the window
    stutter whenever the link hiccups.
    """

    def __init__(self, source, controller: Controller) -> None:
        super().__init__(daemon=True)
        self.source = source
        self.ctrl = controller
        self.dec = proto.Decoder()
        self.rea = Reassembler()
        self.lock = threading.Lock()
        self.frame: Optional[np.ndarray] = None
        self.frame_at: float = 0.0
        # Bumped on every new frame, so the window can skip redrawing when
        # nothing has changed.
        self.frame_seq = 0
        self.n_frames = 0
        self.running = True
        self.errors = 0
        self.last_error = ""
        self._t0 = time.monotonic()
        self._bytes = 0
        self.kbps = 0.0
        self.fps = 0.0
        self._fps_count = 0
        # Longest a frame ever kept us waiting, within a reporting interval.
        # This is what separates "the data stopped" from "the window stopped":
        # if frames keep arriving on time while the picture is visibly stuck,
        # the fault is in the drawing, not the link.
        self.gap_max = 0.0

    def run(self) -> None:
        """Read, decode, reassemble.

        Wrapped in a catch-all on purpose. This loop used to be bare, so any
        single malformed packet that upset the decoder would kill the thread
        outright -- and because the window carries on drawing the last frame it
        received, the only visible symptom was that the picture froze and the
        frame counter stopped. One bad packet must not end the session.
        """
        while self.running:
            try:
                self._pump()
            except Exception as e:  # noqa: BLE001
                self.errors += 1
                self.last_error = repr(e)
                time.sleep(0.02)

    def _pump(self) -> None:
        data = self.source.read(timeout=0.05)
        if not data:
            self._rates()
            return

        self._bytes += len(data)
        for pkt in self.dec.feed(data):
            self.ctrl.on_packet(pkt)
            for frame in self.rea.push(pkt):
                img = cv2.imdecode(np.frombuffer(frame.jpeg, np.uint8),
                                   cv2.IMREAD_COLOR)
                if img is None:
                    continue
                with self.lock:
                    now = time.monotonic()
                    if self.frame_at:
                        gap = now - self.frame_at
                        if gap > self.gap_max:
                            self.gap_max = gap
                    self.frame = img
                    self.frame_at = now
                    self.frame_seq += 1
                    self.n_frames += 1
                    self._fps_count += 1
        self._rates()

    def _rates(self) -> None:
        now = time.monotonic()
        dt = now - self._t0
        if dt >= 1.0:
            self.kbps = self._bytes / dt / 1000.0
            self.fps = self._fps_count / dt
            self._bytes = 0
            self._fps_count = 0
            self._t0 = now


class Panel:
    def __init__(self, root: tk.Tk, reader: Reader, ctrl: Controller,
                 scale: float) -> None:
        self.root = root
        self.reader = reader
        self.ctrl = ctrl
        self.scale = scale
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._last_seq = -1
        self._last_stale = -1
        self._last_labels = 0.0
        # Start the clock now, so the first report covers a real interval
        # rather than the time since the epoch.
        self._last_diag = time.monotonic()
        self._last_seen = 0
        #: Set by close(); run_loop() watches this and tears the window down.
        self.closing = False
        self._last_tick = 0.0
        self._tick_gap_max = 0.0
        self._draw_max = 0.0
        self._ctrl_max = 0.0
        self._lbl_max = 0.0

        root.title("DECT NR+ camera")
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)

        self.video = tk.Label(root, bg="#101014")
        self.video.grid(row=0, column=0, sticky="nsew")

        side = ttk.Frame(root, padding=10)
        side.grid(row=0, column=1, sticky="ns")

        self._build_radio(side)
        self._build_camera(side)
        self._build_status(side)

        root.protocol("WM_DELETE_WINDOW", self.close)
        self.tick()

    # ---- widgets ----------------------------------------------------------

    def _combo(self, parent, label: str, values, row: int, width: int = 10):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=2)
        var = tk.StringVar()
        box = ttk.Combobox(parent, textvariable=var, values=[str(v) for v in values],
                           width=width, state="readonly")
        box.grid(row=row, column=1, sticky="e", pady=2)
        return var, box

    def _build_radio(self, side) -> None:
        f = ttk.LabelFrame(side, text="Radio", padding=8)
        f.pack(fill="x", pady=(0, 10))

        self.v_carrier, self.c_carrier = self._combo(f, "Carrier", CARRIERS, 0)
        self.c_carrier.bind("<<ComboboxSelected>>", self.on_carrier)

        self.v_mcs, self.c_mcs = self._combo(f, "MCS", MCS_CHOICES, 1)
        self.c_mcs.bind("<<ComboboxSelected>>", self.on_mcs)

        self.v_power, self.c_power = self._combo(f, "TX power idx", TX_POWERS, 2)
        self.c_power.bind("<<ComboboxSelected>>", self.on_power)

        self.l_freq = ttk.Label(f, text="", foreground="#666")
        self.l_freq.grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))

        ttk.Label(f, text="Changing the carrier moves both ends.\n"
                          "If one does not follow, both return\n"
                          "to the previous channel by themselves.",
                  foreground="#888", justify="left").grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(6, 0))

    def _build_camera(self, side) -> None:
        f = ttk.LabelFrame(side, text="Camera", padding=8)
        f.pack(fill="x", pady=(0, 10))

        names = [r[0] for r in proto.RESOLUTIONS]
        self.v_res, self.c_res = self._combo(f, "Resolution", names, 0)
        self.c_res.bind("<<ComboboxSelected>>", self.on_res)

        self.v_fps, self.c_fps = self._combo(f, "Target fps", FPS_CHOICES, 1)
        self.c_fps.bind("<<ComboboxSelected>>", self.on_fps)

        self.v_q, self.c_q = self._combo(f, "JPEG quality", QUALITY_CHOICES, 2)
        self.c_q.bind("<<ComboboxSelected>>", self.on_quality)

        ttk.Label(f, text="Quality is the sensor's scale:\nlower is better, and bigger.",
                  foreground="#888", justify="left").grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))

    def _build_status(self, side) -> None:
        f = ttk.LabelFrame(side, text="Link", padding=8)
        f.pack(fill="x")
        self.l_rate = ttk.Label(f, text="-", font=("TkDefaultFont", 10, "bold"))
        self.l_rate.pack(anchor="w")
        self.l_quality = ttk.Label(f, text="-")
        self.l_quality.pack(anchor="w")
        self.l_frames = ttk.Label(f, text="-")
        self.l_frames.pack(anchor="w")
        # Anything that would otherwise make the picture stop with no
        # explanation: a dead reader, serial errors, or the host falling behind.
        self.l_health = ttk.Label(f, text="", foreground="#a00", wraplength=200,
                                  justify="left")
        self.l_health.pack(anchor="w")
        self.l_result = ttk.Label(f, text="", foreground="#046", wraplength=200,
                                  justify="left")
        self.l_result.pack(anchor="w", pady=(6, 0))

    # ---- commands ---------------------------------------------------------

    def on_carrier(self, _e=None) -> None:
        v = int(self.v_carrier.get())
        self.ctrl.send(proto.CTRL_SET_CARRIER, v, f"carrier {v}")

    def on_mcs(self, _e=None) -> None:
        v = int(self.v_mcs.get())
        self.ctrl.send(proto.CTRL_SET_MCS, v, f"MCS {v}")

    def on_power(self, _e=None) -> None:
        v = int(self.v_power.get())
        self.ctrl.send(proto.CTRL_SET_TX_POWER, v, f"power {v}")

    def on_res(self, _e=None) -> None:
        name = self.v_res.get()
        idx = [r[0] for r in proto.RESOLUTIONS].index(name)
        self.ctrl.send(proto.CTRL_SET_RES, idx, f"resolution {name}")

    def on_fps(self, _e=None) -> None:
        v = int(self.v_fps.get())
        self.ctrl.send(proto.CTRL_SET_FPS, v, f"{v} fps")

    def on_quality(self, _e=None) -> None:
        v = int(self.v_q.get())
        self.ctrl.send(proto.CTRL_SET_QUALITY, v, f"quality {v}")

    # ---- refresh ----------------------------------------------------------

    def tick(self) -> None:
        # Time between consecutive ticks. Tk asks for one every 40 ms, so
        # anything much larger means the UI thread was blocked -- by the draw,
        # by Tk itself, or by the display server. Measured here because a
        # stalled window and a stalled link look identical from the outside.
        now0 = time.monotonic()
        if self._last_tick:
            gap = now0 - self._last_tick
            if gap > self._tick_gap_max:
                self._tick_gap_max = gap
        self._last_tick = now0

        self.ctrl.tick()
        t_ctrl = time.monotonic()
        self._draw_frame()
        draw_done = time.monotonic()
        self._ctrl_max = max(self._ctrl_max, t_ctrl - now0)
        self._draw_max = max(self._draw_max, draw_done - t_ctrl)

        # The text only has to keep up with a reader, not with the video.
        # Rewriting seven labels 25 times a second is work Tk does not need to
        # do, and it competes with the redraw for the same thread.
        now = time.monotonic()
        if now - self._last_labels >= 0.25:
            self._last_labels = now
            self._update_labels()
            self._lbl_max = max(self._lbl_max, time.monotonic() - now)

        if now - self._last_diag >= 1.0:
            interval = now - self._last_diag
            self._last_diag = now
            with self.reader.lock:
                fgap = self.reader.gap_max
                self.reader.gap_max = 0.0
                seen = self.reader.n_frames
                # Age of the newest frame, so an ongoing stall shows up. A
                # gap is only recorded when a frame arrives to close it, so
                # without this a complete stop reports zero -- the same as
                # perfectly smooth.
                age = time.monotonic() - self.reader.frame_at if self.reader.frame_at else 0.0
            n = seen - self._last_seen
            self._last_seen = seen
            print(f"[diag] over {interval:4.1f} s | frames {n:3d} | "
                  f"gap max {fgap*1000:5.0f} ms | "
                  f"age now {age*1000:5.0f} ms | "
                  f"tick gap {self._tick_gap_max*1000:5.0f} ms | "
                  f"ctrl {self._ctrl_max*1000:5.0f} ms | "
                  f"draw {self._draw_max*1000:5.0f} ms | "
                  f"labels {self._lbl_max*1000:5.0f} ms | "
                  f"backlog {getattr(self.reader.source, 'backlog', 0)} | "
                  f"lost {self.reader.rea.n_frames_dropped}",
                  flush=True)
            self._tick_gap_max = 0.0
            self._draw_max = 0.0
            self._ctrl_max = 0.0
            self._lbl_max = 0.0

    def _draw_frame(self) -> None:
        with self.reader.lock:
            img = self.reader.frame
            seq = self.reader.frame_seq
            age = time.monotonic() - self.reader.frame_at if self.reader.frame_at else 1e9

        if img is None:
            return

        # Redraw only when there is something new, or when the picture has just
        # gone stale and needs the overlay put on it.
        #
        # This used to redraw on every tick regardless. At 25 ticks a second
        # against 14 frames, nearly half the work was spent rebuilding an
        # identical image -- a full-size copy, a resize, a colour conversion and
        # a fresh Tk image object each time. Under motion, when frames are
        # larger and the decoder has more to do, that was enough to make the
        # window fall behind and appear to seize up.
        stale = age >= STALE_AFTER_S
        # While stale, the overlay counts the seconds, so allow one redraw per
        # second even though the picture underneath is unchanged.
        stale_key = int(age) if stale else -1
        if seq == self._last_seq and stale_key == self._last_stale:
            return
        self._last_seq = seq
        self._last_stale = stale_key

        # No copy: the reader never mutates a frame once it has published it,
        # it only ever replaces the reference.
        if self.scale != 1.0:
            img = cv2.resize(img, None, fx=self.scale, fy=self.scale,
                             interpolation=cv2.INTER_NEAREST)
        elif stale:
            img = img.copy()  # about to be drawn on

        if stale:
            # A frozen picture is indistinguishable from a very still scene.
            img = (img * 0.35).astype(np.uint8)
            h, w = img.shape[:2]
            cv2.putText(img, "NO SIGNAL", (w // 2 - 110, h // 2),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.1, (110, 110, 250), 2, cv2.LINE_AA)
            cv2.putText(img, f"last frame {age:.0f} s ago", (w // 2 - 90, h // 2 + 28),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.5, (200, 200, 200), 1, cv2.LINE_AA)

        rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
        pil = Image.fromarray(rgb)

        # Reuse the same PhotoImage and overwrite its pixels, rather than
        # building a new one for every frame.
        #
        # Each PhotoImage owns an image on the X server. Creating one per frame
        # meant allocating and freeing a server-side image fourteen times a
        # second, and the client call returns immediately because X is
        # asynchronous -- so the cost did not show up in the draw timing. It
        # landed later, inside Tk's own event loop: the callback finished in
        # 25 ms and then Tk did not call back for five and a half seconds.
        # Smooth for a few frames, stopped for a few seconds, repeatedly.
        #
        # paste() writes into the image already on the server, so nothing is
        # allocated or freed per frame.
        if self._photo is None or self._photo.width() != pil.width \
                or self._photo.height() != pil.height:
            self._photo = ImageTk.PhotoImage(pil)
            self.video.configure(image=self._photo)
        else:
            self._photo.paste(pil)

    def _update_labels(self) -> None:
        r = self.reader
        self.l_rate.config(text=f"{r.fps:.1f} fps    {r.kbps:.1f} kB/s")

        rea = r.rea
        lost = rea.n_frames_dropped
        self.l_frames.config(
            text=f"frames {rea.n_frames_ok}   lost {lost} ({rea.loss_pct:.1f} %)")

        st = rea.stats
        if st and st.rssi_dbm is not None:
            self.l_quality.config(text=f"rssi {st.rssi_dbm:.1f} dBm   "
                                       f"snr {st.snr_db:.2f} dB")

        self.l_health.config(text=self._health())

        # Reflect what the far end reports, but never fight the operator: only
        # fill a dropdown that is not currently being changed.
        cfg = self.ctrl.radio
        if cfg:
            self.l_freq.config(text=f"{cfg.mhz:.3f} MHz, {cfg.frag_payload} B fragments")
            if not self.ctrl.busy:
                self._sync(self.v_carrier, cfg.carrier)
                self._sync(self.v_mcs, cfg.mcs)
                self._sync(self.v_power, cfg.tx_power)

        meta = self.ctrl.meta
        if meta and not self.ctrl.busy:
            for name, w, h in proto.RESOLUTIONS:
                if (w, h) == (meta.width, meta.height):
                    self._sync(self.v_res, name)
                    break
            self._sync(self.v_fps, meta.target_fps)
            self._sync(self.v_q, meta.jpeg_quality)

        self.l_result.config(text=self.ctrl.last_result)

    def _health(self) -> str:
        """Anything that would otherwise stop the picture without saying why."""
        notes = []
        r = self.reader

        if not r.is_alive():
            notes.append("decoder thread has stopped")
        if r.errors:
            notes.append(f"{r.errors} decode errors ({r.last_error[:60]})")

        src = r.source
        if getattr(src, "alive", True) is False:
            notes.append("serial thread has stopped")
        if getattr(src, "errors", 0):
            notes.append(f"{src.errors} serial errors ({src.last_error[:60]})")
        if getattr(src, "overruns", 0):
            notes.append(f"{src.overruns} host overruns: not keeping up")
        if getattr(src, "backlog", 0) > 2:
            notes.append(f"{src.backlog} chunks buffered: picture is behind")

        d = r.dec
        if d.n_crc_err:
            notes.append(f"{d.n_crc_err} packet CRC errors")

        return "\n".join(notes)

    @staticmethod
    def _sync(var: tk.StringVar, value) -> None:
        s = str(value)
        if var.get() != s:
            var.set(s)

    def close(self) -> None:
        # Just say so; run_loop() owns the window and does the tearing down.
        # Destroying it from here would pull the widgets out from under the
        # tick that is still running.
        self.reader.running = False
        self.closing = True


def run_loop(root: tk.Tk, panel: "Panel", period: float = 0.04) -> None:
    """Drive the window from here instead of handing control to Tk.

    The obvious way to write this is root.mainloop() with the work scheduled by
    root.after(40, ...). That is what this did, and in a WSLg session it stalls:
    the window freezes for almost exactly 5.5 seconds at a time, over and over,
    while frames keep arriving on time and our callback keeps returning in about
    five milliseconds. The main thread was caught inside tkinter's mainloop on
    every single sample.

    It is not our code and not the video. scripts/tk_stall_test.py cuts it down
    to a bare Tk window with an after(40) timer, no serial, no threads, not even
    an image update, and it stalls identically -- one tick per 5.5 seconds. The
    same loop drawn by OpenCV in the same session holds 24 ticks a second, which
    is why run_viewer.sh was always smooth while this was not.

    The difference is the waiting. mainloop() blocks until Tk decides there is
    something to do, and it is that wait which hangs. update() handles what is
    already pending and returns straight away, so the timing comes from the
    sleep below rather than from Tk's scheduler. Same window, same widgets, same
    redraws -- only who decides when to look. Measured: 23 ticks a second with a
    49 ms worst gap, against one tick per 5.5 s.
    """
    root.protocol("WM_DELETE_WINDOW", panel.close)

    while not panel.closing:
        try:
            panel.tick()
            root.update()
        except tk.TclError:
            # The window went away underneath us -- closing it is not an error.
            break
        time.sleep(period)

    try:
        root.destroy()
    except tk.TclError:
        pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None, help="the receiver's VCOM1")
    ap.add_argument("--replay", metavar="FILE", help="replay a capture; controls are inert")
    ap.add_argument("--baud", type=int, default=1_000_000)
    ap.add_argument("--scale", type=float, default=2.0)
    args = ap.parse_args(argv)

    # Dump every thread's stack on SIGUSR1.
    #
    # The window stalls for seconds at a time while our own timing says the
    # callback returns in five milliseconds, so the time is going somewhere
    # inside Tk. faulthandler is the right tool because it works from a C
    # signal handler: it prints even when the main thread is blocked inside a
    # C call and cannot run ordinary Python.
    faulthandler.register(signal.SIGUSR1, all_threads=True, chain=False)

    if args.replay:
        source = FileSource(args.replay)
        write = None
    else:
        port = args.port or find_rx_port()
        if port is None:
            found = sorted(glob.glob(os.path.join(BY_ID, RX_PORT_GLOB)))
            print("could not decide which port to open.", file=sys.stderr)
            if found:
                print("\nSeveral DK data ports are attached; name one with --port:",
                      file=sys.stderr)
                for f in found:
                    print(f"  {f}", file=sys.stderr)
            else:
                print(f"\nNo {RX_PORT_GLOB} under {BY_ID}. Is the receiver attached?",
                      file=sys.stderr)
            return 1
        source = SerialSource(port, args.baud, rtscts=True)
        write = source.write
        print(f"reading {port}")

    if write is None:
        def write(_b: bytes) -> None:
            raise RuntimeError("replay mode: nothing to send to")

    ctrl = Controller(write)
    reader = Reader(source, ctrl)
    reader.start()

    root = tk.Tk()
    panel = Panel(root, reader, ctrl, args.scale)
    try:
        run_loop(root, panel)
    finally:
        reader.running = False
        source.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
