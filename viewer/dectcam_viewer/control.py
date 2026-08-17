"""Sending control telegrams, and tracking what came back.

Separate from the user interface on purpose: the panel should only have to say
"set the carrier to 1667" without knowing about sequence numbers, retries, or
which commands can be acknowledged and which cannot.

Everything here writes to the same serial port the video arrives on. That port
is full duplex, so no second connection is needed -- the receiver reads
commands off it and relays them over the air.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import proto


@dataclass
class Pending:
    """A command that has been sent and is waiting for its acknowledgement."""

    opcode: int
    seq: int
    arg: int
    sent_at: float
    label: str


# How long to wait before calling a command unanswered. The receiver tries up
# to three bursts of a second each, and the transmitter repeats its
# acknowledgement for 2.5 s, so a slow success can legitimately take a while.
ACK_TIMEOUT_S = 9.0

# A carrier change is never acknowledged: the far end applies it and answers on
# the new channel, which this end has not moved to yet. Both ends move on their
# own timers instead, so success is judged by where the link ends up.
CARRIER_SETTLE_S = 8.0


class Controller:
    """Issues commands and matches acknowledgements to them."""

    def __init__(self, write: Callable[[bytes], None]) -> None:
        self._write = write
        self._seq = 1
        self.pending: Optional[Pending] = None
        # Most recent outcome, for the panel to display.
        self.last_result: str = ""
        self.radio: Optional[proto.RadioCfg] = None
        self.meta: Optional[proto.Meta] = None
        self._carrier_target: Optional[int] = None
        self._carrier_deadline: float = 0.0

    def _next_seq(self) -> int:
        s = self._seq
        self._seq = self._seq % 255 + 1
        return s

    @property
    def busy(self) -> bool:
        return self.pending is not None

    def send(self, opcode: int, arg: int, label: str) -> bool:
        """Send one command. Refuses while another is outstanding."""
        if self.pending is not None:
            self.last_result = "busy: a command is already in flight"
            return False

        seq = self._next_seq()
        try:
            self._write(proto.encode_ctrl(opcode, seq, arg))
        except Exception as e:  # noqa: BLE001 - surfaced to the operator
            self.last_result = f"could not send: {e}"
            return False

        self.pending = Pending(opcode, seq, arg, time.monotonic(), label)
        self.last_result = f"{label}: sent"

        if opcode == proto.CTRL_SET_CARRIER:
            self._carrier_target = arg
            self._carrier_deadline = time.monotonic() + CARRIER_SETTLE_S
            self.last_result = f"{label}: moving, both ends retune on a timer"

        return True

    def on_packet(self, pkt: proto.Packet) -> None:
        """Feed every decoded packet in, so state can be tracked."""
        if pkt.type == proto.TYPE_RADIO_CFG:
            self.radio = proto.RadioCfg.unpack(pkt.payload)
        elif pkt.type == proto.TYPE_FRAME_META:
            self.meta = proto.Meta.unpack(pkt.payload)
        elif pkt.type == proto.TYPE_CTRL:
            c = proto.Ctrl.unpack(pkt.payload)
            if c.opcode != proto.CTRL_ACK:
                return
            p = self.pending
            # The transmitter repeats each acknowledgement for a couple of
            # seconds, so one from the previous command is very likely still
            # arriving. Only the matching sequence counts.
            if p is None or c.seq != p.seq:
                return
            self.pending = None
            self.last_result = f"{p.label}: {proto.ACK_TEXT.get(c.arg32, c.arg32)}"

    def tick(self) -> None:
        """Time out anything that was never answered."""
        now = time.monotonic()

        if self._carrier_target is not None and now >= self._carrier_deadline:
            target = self._carrier_target
            self._carrier_target = None
            self.pending = None
            if self.radio and self.radio.carrier == target:
                self.last_result = f"carrier {target}: link moved"
            elif self.radio:
                self.last_result = (
                    f"carrier {target}: rolled back, still on {self.radio.carrier}"
                )
            else:
                self.last_result = f"carrier {target}: link did not come back"
            return

        p = self.pending
        if p is None or p.opcode == proto.CTRL_SET_CARRIER:
            return
        if now - p.sent_at >= ACK_TIMEOUT_S:
            self.pending = None
            self.last_result = f"{p.label}: no answer"
