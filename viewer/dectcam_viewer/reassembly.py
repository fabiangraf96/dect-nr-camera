"""Frame reassembly for the dectcam stream.

Collects JPEG_FRAG packets into complete frames. There is no retransmission:
a frame missing any fragment when it ages out is dropped and counted.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional

from . import proto


@dataclass
class _Partial:
    frag_cnt: int
    frags: Dict[int, bytes] = field(default_factory=dict)
    first_seen: float = field(default_factory=time.monotonic)

    @property
    def complete(self) -> bool:
        return len(self.frags) == self.frag_cnt

    def assemble(self) -> bytes:
        return b"".join(self.frags[i] for i in range(self.frag_cnt))


@dataclass
class Frame:
    frame_id: int
    jpeg: bytes
    n_frags: int


class Reassembler:
    """Turn a packet stream into complete JPEG frames.

    ``timeout`` is how long an incomplete frame is kept before being written
    off. It should be comfortably longer than the time to transmit one frame
    but short enough that memory does not grow: at ~80 kB/s and 15 kB frames a
    frame takes ~0.2 s, so 2 s is generous.
    """

    def __init__(self, timeout: float = 2.0, max_pending: int = 8) -> None:
        self._pending: Dict[int, _Partial] = {}
        self._timeout = timeout
        self._max_pending = max_pending

        self.meta: Optional[proto.Meta] = None
        self.stats: Optional[proto.Stats] = None

        self.n_frames_ok = 0
        self.n_frames_dropped = 0
        self.n_frags = 0
        self.n_frags_dup = 0
        self.n_bad_jpeg = 0

    def push(self, pkt: proto.Packet) -> Iterator[Frame]:
        if pkt.type == proto.TYPE_FRAME_META:
            if len(pkt.payload) >= proto.META_LEN:
                self.meta = proto.Meta.unpack(pkt.payload)
            return
        if pkt.type == proto.TYPE_STATS:
            if len(pkt.payload) >= proto.STATS_LEN:
                self.stats = proto.Stats.unpack(pkt.payload)
            return
        if pkt.type != proto.TYPE_JPEG_FRAG:
            return

        if pkt.frag_cnt == 0 or pkt.frag_idx >= pkt.frag_cnt:
            return

        self.n_frags += 1

        p = self._pending.get(pkt.frame_id)
        if p is None:
            p = _Partial(frag_cnt=pkt.frag_cnt)
            self._pending[pkt.frame_id] = p
        elif p.frag_cnt != pkt.frag_cnt:
            # frame_id wrapped onto a stale entry; start over.
            p = _Partial(frag_cnt=pkt.frag_cnt)
            self._pending[pkt.frame_id] = p

        if pkt.frag_idx in p.frags:
            self.n_frags_dup += 1
        p.frags[pkt.frag_idx] = pkt.payload

        if p.complete:
            del self._pending[pkt.frame_id]
            jpeg = p.assemble()
            if self._looks_like_jpeg(jpeg):
                self.n_frames_ok += 1
                yield Frame(pkt.frame_id, jpeg, p.frag_cnt)
            else:
                # Every fragment passed CRC, so this means the fragments were
                # misassembled or the encoder emitted something odd. Worth
                # counting separately from ordinary loss.
                self.n_bad_jpeg += 1

        self._expire()

    @staticmethod
    def _looks_like_jpeg(b: bytes) -> bool:
        return len(b) > 4 and b[:2] == b"\xff\xd8" and b[-2:] == b"\xff\xd9"

    def _expire(self) -> None:
        now = time.monotonic()
        dead = [fid for fid, p in self._pending.items() if now - p.first_seen > self._timeout]
        # Also bound memory if frame_ids are churning faster than the timeout.
        if len(self._pending) > self._max_pending:
            oldest = sorted(self._pending.items(), key=lambda kv: kv[1].first_seen)
            dead += [fid for fid, _ in oldest[: len(self._pending) - self._max_pending]]
        for fid in set(dead):
            self._pending.pop(fid, None)
            self.n_frames_dropped += 1

    @property
    def loss_pct(self) -> float:
        total = self.n_frames_ok + self.n_frames_dropped
        return 100.0 * self.n_frames_dropped / total if total else 0.0
