"""Find the receiver's video port by listening for video on it.

Guessing from the device name stopped working once a second kit joined the
bench. The nRF9151 DK sends video over VCOM1 (``-if02``) and the nRF9131 EK
over VCOM0 (``-if00``), because the EK wires RTS/CTS on VCOM0 only and the link
is unusable without them. Worse, the name gives no clue which kit is the
receiver: the transmitter enumerates identically, and pointing the viewer at
the transmitter's VCOM1 is an easy mistake that looks exactly like a dead link.

So rather than infer it, listen. Every candidate is opened briefly and checked
for framed packets. The one that is actually sending video is the one to use,
which is true regardless of board, cabling or which kit was flashed as what.
"""
from __future__ import annotations

import glob
import os
from typing import List, Optional

BY_ID = "/dev/serial/by-id"

#: Both virtual COM ports of every attached J-Link. Which of them carries video
#: depends on the board, so all of them are candidates.
CANDIDATE_GLOBS = (
    "usb-SEGGER_J-Link_*-if00",
    "usb-SEGGER_J-Link_*-if02",
)


def candidates() -> List[str]:
    found: List[str] = []
    for pattern in CANDIDATE_GLOBS:
        found.extend(glob.glob(os.path.join(BY_ID, pattern)))
    return sorted(found)


def carries_video(port: str, baud: int = 1_000_000, secs: float = 0.6) -> bool:
    """True if framed packets appear on this port within a moment."""
    import serial  # local, so replay mode works without pyserial

    from . import proto

    try:
        with serial.Serial(port, baud, timeout=secs, rtscts=True) as ser:
            ser.reset_input_buffer()
            data = ser.read(4096)
    except Exception:
        return False

    if not data:
        return False

    dec = proto.Decoder()
    for _ in dec.feed(data):
        return True
    return False


def find_rx_port(baud: int = 1_000_000) -> Optional[str]:
    """The port with video on it, or None if that is not a single obvious one.

    Returning None rather than picking one is deliberate: with two kits
    attached, choosing wrongly wastes far more time than asking.
    """
    hits = [p for p in candidates() if carries_video(p, baud)]
    return hits[0] if len(hits) == 1 else None


def main() -> int:
    """Report what each candidate port is doing. Run as a module."""
    found = candidates()
    if not found:
        print("no J-Link serial ports found")
        return 1

    for port in found:
        print(f"  {'VIDEO  ' if carries_video(port) else 'silent '} {port}")

    chosen = find_rx_port()
    print()
    print(f"chosen: {chosen}" if chosen else
          "chosen: none -- name one with --port")
    return 0 if chosen else 1


if __name__ == "__main__":
    raise SystemExit(main())
