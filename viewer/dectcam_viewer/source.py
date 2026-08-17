"""Byte sources for the viewer: a serial port, or a file for replay."""

from __future__ import annotations

import threading
import queue
import time
from typing import Optional


class SerialSource:
    """Reads a serial port on a background thread.

    The read loop is kept separate from the display loop so that a slow
    frame decode or a stalled window redraw can never back up the OS serial
    buffer, which would show up as (apparent) link loss.

    RTS/CTS is on by default and should stay on: measured on this hardware,
    1 Mbaud without flow control loses 13 % of bytes, with it 0 %.
    """

    def __init__(self, port: str, baud: int = 1_000_000, rtscts: bool = True,
                 queue_max: int = 256) -> None:
        import serial  # imported here so replay works without pyserial

        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = baud
        self._ser.rtscts = rtscts
        self._ser.timeout = 0.05
        self._ser.open()
        self._ser.reset_input_buffer()

        self.port = port
        self.baud = baud
        self.rtscts = rtscts

        self._q: queue.Queue[bytes] = queue.Queue(maxsize=queue_max)
        self._stop = threading.Event()
        self._overruns = 0
        self._errors = 0
        self.last_error = ""
        self._t = threading.Thread(target=self._run, name="serial-rx", daemon=True)
        self._t.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._ser.read(4096)
            except Exception as e:  # noqa: BLE001
                # Do NOT give up here. This used to break out of the loop, so
                # a single transient read error -- entirely possible on USB
                # serial, and more so when the port is forwarded into WSL --
                # ended the thread and the stream stopped for good, with no
                # message and a window still showing the last frame.
                self._errors += 1
                self.last_error = repr(e)
                time.sleep(0.05)
                continue
            if not data:
                continue
            try:
                self._q.put_nowait(data)
            except queue.Full:
                # Consumer is hopelessly behind; drop rather than grow without
                # bound. Counted so it shows up in the HUD.
                self._overruns += 1

    def read(self, timeout: float = 0.1) -> bytes:
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return b""

    def write(self, data: bytes) -> None:
        """Send bytes back to the receiver.

        The port is full duplex and mostly unused in this direction, so control
        telegrams travel here rather than over a second connection. Writes are
        tiny and infrequent; the read thread is unaffected.
        """
        self._ser.write(data)
        self._ser.flush()

    @property
    def overruns(self) -> int:
        return self._overruns

    @property
    def errors(self) -> int:
        return self._errors

    @property
    def alive(self) -> bool:
        return self._t.is_alive()

    def close(self) -> None:
        self._stop.set()
        self._t.join(timeout=1.0)
        try:
            self._ser.close()
        except Exception:
            pass


class FileSource:
    """Replays a capture file, optionally paced to imitate a real link.

    Useful for working on the viewer without any hardware attached.
    """

    def __init__(self, path: str, chunk: int = 4096, rate_kBps: Optional[float] = None) -> None:
        self._data = open(path, "rb").read()
        self._pos = 0
        self._chunk = chunk
        self._rate = rate_kBps
        self._last = time.monotonic()
        self.port = path
        self.baud = 0
        self.rtscts = False
        self.overruns = 0

    def read(self, timeout: float = 0.1) -> bytes:
        if self._pos >= len(self._data):
            return b""
        if self._rate:
            # Sleep just long enough to hold the requested byte rate.
            want = self._chunk / (self._rate * 1000.0)
            elapsed = time.monotonic() - self._last
            if elapsed < want:
                time.sleep(want - elapsed)
            self._last = time.monotonic()
        out = self._data[self._pos:self._pos + self._chunk]
        self._pos += len(out)
        return out

    @property
    def exhausted(self) -> bool:
        return self._pos >= len(self._data)

    def close(self) -> None:
        pass
