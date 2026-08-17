# `dectcam_proto` — wire protocol

Normative specification. The reference implementation is
`common/dectcam_proto.h`; it is copied verbatim into
`esp32/main/dectcam_proto.h` and mirrored as
`viewer/dectcam_viewer/proto.py`.

The **same packet format** is used on all three hops:

1. Camera board → nRF9151 TX (UART, 1 Mbaud)
2. nRF9151 TX → nRF9151 RX (DECT NR+ PDC payload)
3. nRF9151 RX → PC (VCOM1, 1 Mbaud, RTS/CTS)

…and on the return path, which carries control the other way:

4. PC → nRF9151 RX (the same VCOM1, which is full duplex)
5. nRF9151 RX → nRF9151 TX (DECT NR+, in the gaps between video bursts)

Using one format everywhere means the nRF firmware never parses or rewrites
packets — it moves opaque byte blobs — and the same decoder can be attached at
any hop for debugging.

## Packet layout

All multi-byte integers are **little-endian**.

```
 offset  size  field
 ------  ----  --------------------------------------------------------------
   0      2    magic         0xD2 0xC7
   2      1    ver:4 | type:4
   3      2    frame_id      wraps at 2^16
   5      2    frag_idx      0-based
   7      2    frag_cnt      total fragments in this frame
   9      2    payload_len   0 .. DECTCAM_MAX_PAYLOAD
  11      n    payload
 11+n     2    crc16         CCITT-FALSE over bytes [2 .. 11+n)
```

Header overhead: **13 bytes**.

Note the CRC covers everything *except* the magic. The magic is a
resynchronisation aid, not data; including it would add nothing since a
mis-synced reader finds the magic by searching for it in the first place.

### `ver`

Currently `1`. A receiver must discard packets whose version it does not know.

### `type`

| Value | Name | Meaning |
|---|---|---|
| 1 | `JPEG_FRAG` | A fragment of a JPEG frame |
| 2 | `FRAME_META` | Stream parameters; sent on change and periodically |
| 3 | `STATS` | Link statistics from the RX board |
| 4 | `CTRL` | Reserved for the future PC→camera back-channel |

## Type 1 — `JPEG_FRAG`

`payload` is a byte range of a JPEG image. Concatenating fragments
`0 .. frag_cnt-1` of a given `frame_id` reproduces the JPEG exactly, starting
with `FF D8` and ending with `FF D9`.

A frame is delivered to the display only when all `frag_cnt` fragments have
arrived with valid CRCs. Otherwise it is dropped. There is no retransmission.

`frame_id` increments by one per captured frame and wraps. Receivers must treat
it as a sliding window, not a monotonic counter.

## Type 2 — `FRAME_META`

Sent whenever the camera configuration changes and every ~1 s as a keepalive,
so a viewer attached mid-stream learns the parameters quickly.

```
 offset  size  field
   0      2    width       pixels
   2      2    height      pixels
   4      1    jpeg_quality  4..63 (OmniVision scale, LOWER = better)
   5      1    target_fps
   6      2    max_payload   fragment payload size in use
```

The quality scale is the sensor's quantisation index, shared by the OV2640 and
the OV3660, and is inverted relative to the usual 0..100 convention.

`frame_id`/`frag_idx`/`frag_cnt` are 0 for this type.

## Type 3 — `STATS`

Emitted by the RX board roughly once per second so the viewer HUD can show
link quality.

```
 offset  size  field
   0      2    rssi_2        int16, dBm in 0.5 dBm steps  (Q14.1)
   2      2    snr           int16, dB  in 0.25 dB steps  (Q13.2)
   4      4    pdc_ok        PDC packets received with good CRC
   8      4    pdc_crc_err   PDC packets received with bad CRC
  12      4    frags_out     fragments forwarded to the PC
   16      2    ring_peak     uint16, deepest the RX outbound ring got
                              since the last STATS, in bytes
```

Total 18 bytes. `ring_peak` is a high water mark over the interval, not a
running total, and it is read-and-cleared by exactly one caller.

It is here rather than in the receiver's log because both virtual COM ports sit
on one J-Link: while video flows on VCOM1 the VCOM0 console is starved and says
nothing, which is precisely when the number is wanted. Divided by the byte rate
it gives the delay the receiver is contributing — see `docs/results.md`.

`rssi_2` and `snr` are carried in **exactly** the representation
`nrf_modem_dect_phy.h` reports them, so nothing is rounded on the way to the
viewer:

```
dBm = rssi_2 / 2.0        dB = snr / 4.0
```

The modem's "not measured" sentinels are passed through unchanged and must be
rendered as "unknown" rather than as a number:

| Constant | Value |
|---|---|
| `NRF_MODEM_DECT_PHY_RSSI2_NOT_MEASURED` | −32768 |
| `NRF_MODEM_DECT_PHY_SNR_NOT_MEASURED` | −32768 |

## Type 4 — `CTRL`

Control, and the acknowledgement of it. Six bytes:

```
 offset  size  field
   0      1    opcode
   1      1    seq
   2      4    arg
```

| Opcode | Name | Argument |
|---|---|---|
| 1 | `SET_TX_POWER` | power index, 0–13 |
| 2 | `SET_MCS` | 0–4 |
| 3 | `SET_CARRIER` | carrier number |
| 4 | `SET_RES` | index into the resolution table |
| 5 | `SET_FPS` | 1–30 |
| 6 | `SET_QUALITY` | 4–63, lower is better |
| 7 | `ACK` | status: 0 applied, 1 bad argument, 2 unsupported, 3 failed |
| 8 | `CARRIER_OK` | the receiver confirms the new carrier |

`SET_RES` carries an index rather than a name, so **the order of the resolution
table is part of the protocol**. It is `qqvga, qcif, hqvga, qvga, cif, hvga,
vga, svga`, and the self-tests on both sides pin it down.

### Why there is a sequence number

The transmitter cannot listen continuously — it is busy sending video, and
turning the radio around costs air time. It opens a 30 ms window every 200 ms,
so the receiver sends each telegram as a burst of copies 25 ms apart. `seq`
lets the far end tell a fresh command from a repeat of one it has already acted
on. Without it, "set the resolution" would be harmless to repeat but anything
relative would not.

De-duplication compares against the previous sequence only, so a host that
restarts and reuses a number does not lose a command; it would have to reuse
the same number twice in succession.

### Acknowledgement

The transmitter answers by putting a `CTRL` packet with opcode `ACK` into the
**video stream**, which already flows in the right direction. It repeats it
every 150 ms for 2.5 s: the receiver is deaf while transmitting its burst, and
a prompt acknowledgement would otherwise be sent straight into that deafness.

`SET_CARRIER` is the exception — it is never acknowledged, and cannot be. The
transmitter applies the new carrier the moment it hears the telegram and
answers there, on a channel the receiver has not moved to yet. Both ends move
on their own timers instead; see below.

## Type 5 — `RADIO_CFG`

What the transmitter's radio is currently set to, announced once a second.
Eight bytes:

```
 offset  size  field
   0      2    carrier
   2      1    mcs
   3      1    tx_power
   4      1    subslots
   5      1    reserved
   6      2    frag_payload
```

Exists for the same reason `FRAME_META` does: these are changeable at run time
now, and a viewer that attaches later has no other way to learn where the link
ended up. It is also how a `SET_CARRIER` is judged, since that one has no
acknowledgement.

## Changing the carrier

This is the one command that can lose the link outright. If the two ends end up
on different channels, nothing can be said to either of them again.

Neither end waits for the other:

1. The receiver announces the new carrier on the **old** one, as a burst
   lasting about a second, then retunes regardless.
2. The transmitter applies it as soon as it hears it, and starts a 6 s trial.
3. The receiver watches for video on the new carrier. If it appears, it sends
   `CARRIER_OK`, which ends the transmitter's trial.
4. If no video appears within 4 s, the receiver returns to the old carrier. If
   no `CARRIER_OK` arrives within 6 s, so does the transmitter.

The receiver's deadline is deliberately the shorter one, so the two converge on
the old channel rather than crossing over.

Measured: hopping 1665 → 1657 → 1671 → 1677 → 1665 keeps the link every time,
video included.

## Fragmentation

The fragment payload size is chosen so that a whole packet fits one DECT PDC
transport block:

```
DECTCAM_MAX_PAYLOAD = TBS(mcs, subslots) − DECTCAM_HDR_LEN − DECTCAM_CRC_LEN
                    = TBS(mcs, subslots) − 13
```

For the default MCS 4 / 8 subslots (TBS = 700 B) that is **687 bytes**.

The TX firmware asserts at build time that the configured MCS and subslot
combination is legal (see the TBS table in `architecture.md`; many
combinations are marked unsupported) and that the resulting payload is > 0.

## CRC

CRC-16/CCITT-FALSE: polynomial `0x1021`, init `0xFFFF`, no reflection, no final
XOR. Transmitted little-endian.

## Receiver rules

1. Scan for magic `D2 C7`.
2. If fewer than `DECTCAM_HDR_LEN` bytes are buffered, wait.
3. Read `payload_len`; reject if greater than `DECTCAM_MAX_PAYLOAD` and resync
   one byte past the magic.
4. Wait for the full packet, verify CRC; on failure resync one byte past the
   magic.
5. Dispatch by `type`.

Rule 3 matters: a corrupted `payload_len` must never be trusted to size a
buffer or to skip forward in the stream.

## Rationale for retaining the magic over DECT

On the UART hops the magic is required for byte-level resynchronisation. Over
DECT the PDC is already a framed unit, so the magic is redundant there — but
keeping the format byte-identical lets the nRF firmware forward payloads
without touching them, and lets a logic analyser or a serial dump be decoded
with the same tool at any point in the chain. 13 bytes on a 700-byte packet is
1.9 % overhead, which is a fair price.
