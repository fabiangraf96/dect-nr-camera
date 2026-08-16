"""Cross-check the Python protocol mirror against the C reference vector.

Run:  python3 -m dectcam_viewer.selftest
"""

import sys

from . import proto

# Produced by nrf/common/test_proto.c (print_reference_vector).
# JPEG_FRAG, frame_id=0x0102, frag_idx=3, frag_cnt=4, payload FFD8AABBFFD9
C_REFERENCE = bytes.fromhex("D2C7110201030004000600FFD8AABBFFD97D81")

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        print(f"FAIL: {msg}")
        failures += 1


def main() -> int:
    # 1. CRC check value from the CRC-16/CCITT-FALSE standard.
    crc = proto.crc16(b"123456789")
    check(crc == 0x29B1, f'crc16("123456789") = 0x{crc:04X}, want 0x29B1')
    print(f'  crc16("123456789") = 0x{crc:04X}')

    # 2. The Python encoder must reproduce the C reference vector byte for byte.
    mine = proto.encode(
        proto.TYPE_JPEG_FRAG,
        frame_id=0x0102,
        frag_idx=3,
        frag_cnt=4,
        payload=bytes.fromhex("FFD8AABBFFD9"),
    )
    check(mine == C_REFERENCE, f"encode mismatch\n  py={mine.hex().upper()}\n   c={C_REFERENCE.hex().upper()}")
    print(f"  reference vector matches C: {mine.hex().upper()}")

    # 3. Decode it back.
    dec = proto.Decoder()
    pkts = list(dec.feed(C_REFERENCE))
    check(len(pkts) == 1, f"expected 1 packet, got {len(pkts)}")
    if pkts:
        p = pkts[0]
        check(p.type == proto.TYPE_JPEG_FRAG, "type")
        check(p.frame_id == 0x0102, f"frame_id {p.frame_id:#x}")
        check(p.frag_idx == 3, "frag_idx")
        check(p.frag_cnt == 4, "frag_cnt")
        check(p.payload == bytes.fromhex("FFD8AABBFFD9"), "payload")

    # 4. Resynchronisation: bury a packet in garbage, split across chunks.
    dec = proto.Decoder()
    stream = b"\x00\x01\xd2junk\xff" + C_REFERENCE + b"\xd2\xc7trailing"
    got = []
    for i in range(0, len(stream), 3):
        got += list(dec.feed(stream[i:i + 3]))
    check(len(got) == 1, f"resync: expected 1 packet, got {len(got)}")
    print(f"  resync ok ({dec.n_resync} resyncs)")

    # 5. Corruption must be caught, not silently accepted.
    missed = 0
    for i in range(2, len(C_REFERENCE)):
        bad = bytearray(C_REFERENCE)
        bad[i] ^= 0x01
        d = proto.Decoder()
        if list(d.feed(bytes(bad))):
            missed += 1
    check(missed == 0, f"{missed} single-bit corruptions accepted")
    print(f"  all {len(C_REFERENCE) - 2} single-bit corruptions detected")

    # 6. Meta / stats roundtrip.
    m = proto.Meta(320, 240, 12, 10, 687)
    check(proto.Meta.unpack(m.pack()) == m, "meta roundtrip")
    # -37 half-dBm = -18.5 dBm, matching what the DK link actually reports.
    s = proto.Stats(-37, 40, 1234, 5, 999)
    s2 = proto.Stats.unpack(s.pack())
    check(s2 == s, "stats roundtrip")
    check(abs(s2.rssi_dbm - (-18.5)) < 1e-9, f"rssi_dbm {s2.rssi_dbm}")
    check(abs(s2.snr_db - 10.0) < 1e-9, f"snr_db {s2.snr_db}")
    print(f"  stats: rssi={s2.rssi_dbm} dBm snr={s2.snr_db} dB")

    ns = proto.Stats.unpack(
        proto.Stats(proto.RSSI2_NOT_MEASURED, proto.SNR_NOT_MEASURED, 0, 0, 0).pack()
    )
    check(ns.rssi_dbm is None, "rssi sentinel -> None")
    check(ns.snr_db is None, "snr sentinel -> None")
    print("  not-measured sentinels map to None")

    print("\n" + ("FAILED" if failures else "ALL PASS") + f" ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
