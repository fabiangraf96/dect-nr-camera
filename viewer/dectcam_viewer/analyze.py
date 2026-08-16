"""Offline analysis of a captured dectcam byte stream.

Decodes a raw capture, reassembles frames, and reports link health. Optionally
writes the recovered JPEGs to a directory so they can be eyeballed.

Usage:
    python3 -m dectcam_viewer.analyze capture.bin [--dump outdir] [--secs 10]
"""

from __future__ import annotations

import argparse
import os
import sys

from . import proto
from .reassembly import Reassembler


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture", help="raw byte capture")
    ap.add_argument("--dump", metavar="DIR", help="write recovered JPEGs here")
    ap.add_argument("--secs", type=float, default=0.0,
                    help="capture duration, for rate figures")
    args = ap.parse_args(argv)

    data = open(args.capture, "rb").read()

    dec = proto.Decoder()
    rea = Reassembler()
    frames = []

    for pkt in dec.feed(data):
        for f in rea.push(pkt):
            frames.append(f)

    print(f"capture      : {args.capture}  ({len(data)} bytes)")
    if args.secs:
        print(f"byte rate    : {len(data)/args.secs/1000:.1f} kB/s")
    print()
    print("--- packet layer ---")
    print(f"packets ok   : {dec.n_packets}")
    print(f"crc errors   : {dec.n_crc_err}")
    print(f"resyncs      : {dec.n_resync}")
    print()
    print("--- frame layer ---")
    print(f"frames ok    : {rea.n_frames_ok}")
    print(f"frames lost  : {rea.n_frames_dropped}  ({rea.loss_pct:.1f} %)")
    print(f"bad jpeg     : {rea.n_bad_jpeg}")
    print(f"fragments    : {rea.n_frags} (dup {rea.n_frags_dup})")
    if args.secs and rea.n_frames_ok:
        print(f"frame rate   : {rea.n_frames_ok/args.secs:.1f} fps")

    if rea.meta:
        m = rea.meta
        print()
        print("--- announced stream parameters ---")
        print(f"resolution   : {m.width}x{m.height}")
        print(f"jpeg quality : {m.jpeg_quality}")
        print(f"target fps   : {m.target_fps}")
        print(f"max payload  : {m.max_payload}")
    else:
        print("\n(no FRAME_META seen)")

    if frames:
        sizes = [len(f.jpeg) for f in frames]
        print()
        print("--- recovered jpeg sizes ---")
        print(f"min/avg/max  : {min(sizes)} / {sum(sizes)//len(sizes)} / {max(sizes)} bytes")
        print(f"frag counts  : {sorted(set(f.n_frags for f in frames))}")
        ids = [f.frame_id for f in frames]
        print(f"frame_id     : {ids[0]} .. {ids[-1]}")

        # Verify the JPEGs actually decode, not just that the markers match.
        try:
            import cv2
            import numpy as np

            good = 0
            dims = set()
            for f in frames:
                img = cv2.imdecode(np.frombuffer(f.jpeg, np.uint8), cv2.IMREAD_COLOR)
                if img is not None:
                    good += 1
                    dims.add((img.shape[1], img.shape[0]))
            print(f"cv2 decoded  : {good}/{len(frames)}  dims={sorted(dims)}")
        except ImportError:
            print("cv2 decoded  : (opencv not installed, skipped)")

    if args.dump and frames:
        os.makedirs(args.dump, exist_ok=True)
        for i, f in enumerate(frames):
            with open(os.path.join(args.dump, f"frame_{i:04d}_id{f.frame_id}.jpg"), "wb") as fh:
                fh.write(f.jpeg)
        print(f"\nwrote {len(frames)} jpegs to {args.dump}/")

    return 0


if __name__ == "__main__":
    sys.exit(main())
