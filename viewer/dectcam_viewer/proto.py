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
# What the transmitter's radio is currently set to. Announced periodically,
# because these are changeable at run time and a viewer that attaches later
# has no other way to learn where the link ended up.
TYPE_RADIO_CFG = 5

META_LEN = 8
STATS_LEN = 20
CTRL_LEN = 6
RADIO_CFG_LEN = 8

MAX_PAYLOAD = 1024
MAX_PACKET = MAX_PAYLOAD + OVERHEAD

# Control opcodes. Mirrors the block in dectcam_proto.h.
#
# Control flows opposite to video: the PC sends a CTRL packet down the
# receiver's USB link, the receiver forwards it over the air, and the
# transmitter either applies it to its own radio or passes it to the camera.
CTRL_NOP = 0
CTRL_SET_TX_POWER = 1
CTRL_SET_MCS = 2
CTRL_SET_CARRIER = 3
CTRL_SET_RES = 4
CTRL_SET_FPS = 5
CTRL_SET_QUALITY = 6
CTRL_ACK = 7
CTRL_CARRIER_OK = 8

ACK_OK = 0
ACK_BAD_ARG = 1
ACK_UNSUPP = 2
ACK_FAILED = 3

ACK_TEXT = {
    ACK_OK: "applied",
    ACK_BAD_ARG: "rejected: value out of range",
    ACK_UNSUPP: "rejected: not supported",
    ACK_FAILED: "failed to apply",
}

# The resolutions the camera understands, in the order its own table uses --
# CTRL_SET_RES carries the index, not a string.
RESOLUTIONS = [
    ("qqvga", 160, 120),
    ("qcif", 176, 144),
    ("hqvga", 240, 176),
    ("qvga", 320, 240),
    ("cif", 400, 296),
    ("hvga", 480, 320),
    ("vga", 640, 480),
    ("svga", 800, 600),
]


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

    #: Deepest the receiver's outbound ring got since the last statistics
    #: packet, in bytes. This is buffered delay: bytes received off the air
    #: that the host has not been given yet. See ``ring_delay_s``.
    ring_peak: int = 0

    #: Packets whose CRC was already wrong when the modem handed them to the
    #: receiver, before anything of ours touched them. The DECT layer has its
    #: own CRC and reports no errors, so anything here means the corruption
    #: happened before the receiver rather than on the way to the PC.
    rx_crc_bad: int = 0

    @classmethod
    def unpack(cls, b: bytes) -> "Stats":
        r, s, ok, err, out, peak, badcrc = struct.unpack("<hhIIIHH", b[:STATS_LEN])
        return cls(r, s, ok, err, out, peak, badcrc)

    def pack(self) -> bytes:
        return struct.pack(
            "<hhIIIHH", self.rssi_2, self.snr, self.pdc_ok,
            self.pdc_crc_err, self.frags_out, self.ring_peak, self.rx_crc_bad,
        )

    def ring_delay_s(self, byte_rate: float) -> Optional[float]:
        """Seconds of delay the receiver's ring is contributing."""
        if byte_rate <= 0:
            return None
        return self.ring_peak / byte_rate

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


@dataclass
class Ctrl:
    """A control telegram.

    `seq` is here because control frames are sent repeatedly: the transmitter
    only listens in short windows between video bursts, so a single copy would
    usually be missed. Whoever acts on a telegram ignores a `seq` it has
    already seen, which makes retransmission harmless.
    """

    opcode: int
    seq: int
    arg32: int

    @classmethod
    def unpack(cls, b: bytes) -> "Ctrl":
        op, seq, arg = struct.unpack("<BBI", b[:CTRL_LEN])
        return cls(op, seq, arg)

    def pack(self) -> bytes:
        return struct.pack("<BBI", self.opcode, self.seq, self.arg32)


@dataclass
class RadioCfg:
    """What the transmitter's radio is currently set to."""

    carrier: int
    mcs: int
    tx_power: int
    subslots: int
    frag_payload: int

    @classmethod
    def unpack(cls, b: bytes) -> "RadioCfg":
        car, mcs, pwr, sub, _res, frag = struct.unpack("<HBBBBH", b[:RADIO_CFG_LEN])
        return cls(car, mcs, pwr, sub, frag)

    def pack(self) -> bytes:
        return struct.pack(
            "<HBBBBH", self.carrier, self.mcs, self.tx_power,
            self.subslots, 0, self.frag_payload,
        )

    @property
    def mhz(self) -> float:
        """Centre frequency for a band 1 carrier number."""
        return 1881.792 + (self.carrier - 1657) * 0.864


def encode_ctrl(opcode: int, seq: int, arg32: int) -> bytes:
    """Build a complete CTRL packet, ready to write to the receiver."""
    return encode(TYPE_CTRL, payload=Ctrl(opcode, seq, arg32).pack())


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

        while True:
            pkt = self._try_one()
            if pkt is None:
                break
            yield pkt

        # Runaway guard, applied to what is LEFT once everything parseable has
        # been taken out.
        #
        # It used to run before parsing, which quietly destroyed data: feeding
        # a whole capture at once -- as the offline analyser does -- put more
        # than max_buffer bytes in the buffer in one go, and half of them were
        # discarded before anything had a chance to parse them. A 1.1 MB
        # capture of a perfectly healthy link decoded as 864 packets instead of
        # 1801, i.e. exactly half, and reported the frame rate as 6.7 fps
        # instead of 14. Nothing was wrong with the hardware.
        #
        # After parsing, a buffer this large means something genuinely is
        # wrong: no valid packet was found in a megabyte of data.
        if len(self._buf) > self._max_buffer:
            del self._buf[: len(self._buf) - self._max_buffer // 2]
            self.n_resync += 1

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
