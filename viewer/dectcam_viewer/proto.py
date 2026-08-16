"""dectcam wire protocol - Python mirror of common/dectcam_proto.h.

Normative specification: docs/protocol.md

Packet layout, little-endian throughout::

     off  size  field
      0    2    magic 0xD2 0xC7
      2    1    ver:4 | type:4
      3    2    frame_id
      5    2    frag_idx
      7    2    frag_cnt
      9    2    payload_len
     11    n    payload
    11+n   2    crc16-ccitt-false over bytes [2 .. 11+n)

Keep this in sync with the C header. ``selftest.py`` checks both against the
same reference vector.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Iterator, Optional

MAGIC = b"\xd2\xc7"
VERSION = 1

OFF_VERTYPE = 2
OFF_FRAME_ID = 3
OFF_FRAG_IDX = 5
OFF_FRAG_CNT = 7
OFF_PAYLOAD_LEN = 9
OFF_PAYLOAD = 11

HDR_LEN = 11
CRC_LEN = 2
OVERHEAD = HDR_LEN + CRC_LEN  # 13

TYPE_JPEG_FRAG = 1
TYPE_FRAME_META = 2
TYPE_STATS = 3
TYPE_CTRL = 4

META_LEN = 8
STATS_LEN = 16

MAX_PAYLOAD = 1024
MAX_PACKET = MAX_PAYLOAD + OVERHEAD


# --------------------------------------------------------------------------
# CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor
# --------------------------------------------------------------------------

def _build_table() -> list[int]:
    table = []
    for b in range(256):
        crc = b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC_TABLE = _build_table()


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ byte) & 0xFF]
    return crc


# --------------------------------------------------------------------------
# Packet
# --------------------------------------------------------------------------

@dataclass
class Packet:
    version: int
    type: int
    frame_id: int
    frag_idx: int
    frag_cnt: int
    payload: bytes


@dataclass
class Meta:
    width: int
    height: int
    jpeg_quality: int
    target_fps: int
    max_payload: int

    @classmethod
    def unpack(cls, b: bytes) -> "Meta":
        w, h, q, f, mp = struct.unpack("<HHBBH", b[:META_LEN])
        return cls(w, h, q, f, mp)

    def pack(self) -> bytes:
        return struct.pack(
            "<HHBBH", self.width, self.height, self.jpeg_quality,
            self.target_fps, self.max_payload,
        )


# Sentinels from nrf_modem_dect_phy.h, passed through unchanged.
RSSI2_NOT_MEASURED = -32768
SNR_NOT_MEASURED = -32768


@dataclass
class Stats:
    """Radio quality in the modem's own representation.

    rssi_2 : dBm in 0.5 dBm steps (Q14.1)
    snr    : dB  in 0.25 dB steps (Q13.2)
    """

    rssi_2: int
    snr: int
    pdc_ok: int
    pdc_crc_err: int
    frags_out: int

    @classmethod
    def unpack(cls, b: bytes) -> "Stats":
        r, s, ok, err, out = struct.unpack("<hhIII", b[:STATS_LEN])
        return cls(r, s, ok, err, out)

    def pack(self) -> bytes:
        return struct.pack(
            "<hhIII", self.rssi_2, self.snr, self.pdc_ok,
            self.pdc_crc_err, self.frags_out,
        )

    @property
    def rssi_dbm(self) -> Optional[float]:
        if self.rssi_2 == RSSI2_NOT_MEASURED:
            return None
        return self.rssi_2 / 2.0

    @property
    def snr_db(self) -> Optional[float]:
        if self.snr == SNR_NOT_MEASURED:
            return None
        return self.snr / 4.0


def encode(
    type_: int,
    frame_id: int = 0,
    frag_idx: int = 0,
    frag_cnt: int = 0,
    payload: bytes = b"",
) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)}")
    body = struct.pack(
        "<BHHHH",
        (VERSION << 4) | (type_ & 0x0F),
        frame_id & 0xFFFF,
        frag_idx & 0xFFFF,
        frag_cnt & 0xFFFF,
        len(payload),
    ) + payload
    return MAGIC + body + struct.pack("<H", crc16(body))


class Decoder:
    """Incremental, resynchronising packet decoder.

    Feed it arbitrary byte chunks; iterate the packets it yields. Counters are
    exposed so the viewer HUD can show how healthy the link is.
    """

    def __init__(self, max_buffer: int = 1 << 20) -> None:
        self._buf = bytearray()
        self._max_buffer = max_buffer
        self.n_packets = 0
        self.n_crc_err = 0
        self.n_resync = 0
        self.n_bytes = 0

    def feed(self, data: bytes) -> Iterator[Packet]:
        self.n_bytes += len(data)
        self._buf += data

        # Runaway guard: if we somehow never find a valid packet, do not grow
        # without bound.
        if len(self._buf) > self._max_buffer:
            del self._buf[: len(self._buf) - self._max_buffer // 2]
            self.n_resync += 1

        while True:
            pkt = self._try_one()
            if pkt is None:
                return
            yield pkt

    def _try_one(self) -> Optional[Packet]:
        buf = self._buf
        while True:
            i = buf.find(MAGIC)
            if i < 0:
                # Keep the last byte: it might be the first half of a magic.
                if len(buf) > 1:
                    del buf[:-1]
                return None
            if i > 0:
                del buf[:i]
                self.n_resync += 1

            if len(buf) < HDR_LEN:
                return None

            vertype = buf[OFF_VERTYPE]
            version = vertype >> 4
            payload_len = int.from_bytes(buf[OFF_PAYLOAD_LEN:OFF_PAYLOAD_LEN + 2], "little")

            # A bad version or an implausible length means this "magic" was
            # noise. Skip past it rather than trusting the length field.
            if version != VERSION or payload_len > MAX_PAYLOAD:
                del buf[:2]
                self.n_resync += 1
                continue

            total = payload_len + OVERHEAD
            if len(buf) < total:
                return None

            body = bytes(buf[OFF_VERTYPE:OFF_PAYLOAD + payload_len])
            want = crc16(body)
            got = int.from_bytes(buf[OFF_PAYLOAD + payload_len:total], "little")
            if want != got:
                self.n_crc_err += 1
                del buf[:2]
                continue

            frame_id = int.from_bytes(buf[OFF_FRAME_ID:OFF_FRAME_ID + 2], "little")
            frag_idx = int.from_bytes(buf[OFF_FRAG_IDX:OFF_FRAG_IDX + 2], "little")
            frag_cnt = int.from_bytes(buf[OFF_FRAG_CNT:OFF_FRAG_CNT + 2], "little")
            payload = bytes(buf[OFF_PAYLOAD:OFF_PAYLOAD + payload_len])
            del buf[:total]
            self.n_packets += 1
            return Packet(version, vertype & 0x0F, frame_id, frag_idx, frag_cnt, payload)
