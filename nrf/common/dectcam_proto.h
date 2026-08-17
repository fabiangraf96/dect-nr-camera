/*
 * dectcam_proto.h - wire protocol for the DECT NR+ camera link.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Normative specification: docs/protocol.md
 *
 * This header is the reference implementation and is used verbatim by:
 *   - nrf/tx, nrf/rx   (Zephyr / NCS)
 *   - esp32/main                (ESP-IDF)
 * and is mirrored in Python by viewer/dectcam_viewer/proto.py.
 *
 * Keep the three copies in sync. It is deliberately dependency-free
 * (only <stdint.h>/<stddef.h>) so it drops into any of those builds.
 *
 * Packet layout, little-endian throughout:
 *
 *   off  size  field
 *    0    2    magic 0xD2 0xC7
 *    2    1    ver:4 | type:4
 *    3    2    frame_id
 *    5    2    frag_idx
 *    7    2    frag_cnt
 *    9    2    payload_len
 *   11    n    payload
 *  11+n   2    crc16-ccitt-false over bytes [2 .. 11+n)
 */

#ifndef DECTCAM_PROTO_H
#define DECTCAM_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DECTCAM_MAGIC0 0xD2u
#define DECTCAM_MAGIC1 0xC7u

#define DECTCAM_VERSION 1u

/* Byte offsets into a packet. */
#define DECTCAM_OFF_MAGIC       0
#define DECTCAM_OFF_VERTYPE     2
#define DECTCAM_OFF_FRAME_ID    3
#define DECTCAM_OFF_FRAG_IDX    5
#define DECTCAM_OFF_FRAG_CNT    7
#define DECTCAM_OFF_PAYLOAD_LEN 9
#define DECTCAM_OFF_PAYLOAD     11

#define DECTCAM_HDR_LEN 11u
#define DECTCAM_CRC_LEN 2u
/* Total bytes added to every payload. */
#define DECTCAM_OVERHEAD (DECTCAM_HDR_LEN + DECTCAM_CRC_LEN) /* 13 */

/* Packet types. */
#define DECTCAM_TYPE_JPEG_FRAG  1u
#define DECTCAM_TYPE_FRAME_META 2u
#define DECTCAM_TYPE_STATS      3u
#define DECTCAM_TYPE_CTRL       4u
/*
 * What the transmitter's radio is currently set to.
 *
 * Announced periodically for the same reason FRAME_META is: the operator can
 * change these at run time, and a viewer that attaches later has no other way
 * to learn where the link ended up. It also closes the loop on a command --
 * the acknowledgement says "applied", this says what it was applied to.
 */
#define DECTCAM_TYPE_RADIO_CFG  5u

/* Payload sizes of the fixed-layout types. */
#define DECTCAM_META_LEN      8u
#define DECTCAM_STATS_LEN     18u
#define DECTCAM_CTRL_LEN      6u
#define DECTCAM_RADIO_CFG_LEN 8u

/*
 * Control opcodes, carried in a DECTCAM_TYPE_CTRL packet.
 *
 * Control flows the opposite way to video: the PC sends a CTRL packet down the
 * receiver's USB link, the receiver forwards it over the air to the
 * transmitter, and the transmitter either applies it to its own radio or hands
 * it to the camera over the UART back-channel.
 *
 * The transmitter answers by inserting CTRL_ACK into the video stream, which
 * already flows the right way, so no extra channel is needed for
 * acknowledgement.
 */
#define DECTCAM_CTRL_NOP           0u
#define DECTCAM_CTRL_SET_TX_POWER  1u  /* arg: power index 0..13            */
#define DECTCAM_CTRL_SET_MCS       2u  /* arg: 0..4                         */
#define DECTCAM_CTRL_SET_CARRIER   3u  /* arg: carrier number               */
#define DECTCAM_CTRL_SET_RES       4u  /* arg: index into the size table    */
#define DECTCAM_CTRL_SET_FPS       5u  /* arg: 1..30                        */
#define DECTCAM_CTRL_SET_QUALITY   6u  /* arg: 4..63, lower is better       */
#define DECTCAM_CTRL_ACK           7u  /* arg: status, 0 = applied          */
#define DECTCAM_CTRL_CARRIER_OK    8u  /* receiver confirms the new carrier */

/* Status values reported in a CTRL_ACK. */
#define DECTCAM_ACK_OK        0u
#define DECTCAM_ACK_BAD_ARG   1u
#define DECTCAM_ACK_UNSUPP    2u
#define DECTCAM_ACK_FAILED    3u

/*
 * Largest payload we will ever have to buffer on a receiver. The transmitter
 * picks its actual fragment size from the DECT transport block size (see
 * dectcam_tbs.h); this is just a safe upper bound for reassembly buffers.
 */
#define DECTCAM_MAX_PAYLOAD 1024u
#define DECTCAM_MAX_PACKET  (DECTCAM_MAX_PAYLOAD + DECTCAM_OVERHEAD)

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                              */
/* ------------------------------------------------------------------ */

static inline void dectcam_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint16_t dectcam_get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void dectcam_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint32_t dectcam_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflect, no xorout */
/* ------------------------------------------------------------------ */

static inline uint16_t dectcam_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFFu;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (int b = 0; b < 8; b++) {
			if (crc & 0x8000u) {
				crc = (uint16_t)((crc << 1) ^ 0x1021u);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

/* ------------------------------------------------------------------ */
/* Encode                                                             */
/* ------------------------------------------------------------------ */

/*
 * Build a packet into `out`, which must hold at least
 * payload_len + DECTCAM_OVERHEAD bytes.
 *
 * `payload` may be NULL when payload_len is 0.
 *
 * Returns the total packet length, or 0 if the arguments do not fit.
 */
static inline size_t dectcam_encode(uint8_t *out, size_t out_cap, uint8_t type,
				    uint16_t frame_id, uint16_t frag_idx,
				    uint16_t frag_cnt, const uint8_t *payload,
				    uint16_t payload_len)
{
	const size_t total = (size_t)payload_len + DECTCAM_OVERHEAD;

	if (out == NULL || out_cap < total) {
		return 0;
	}
	if (payload_len > DECTCAM_MAX_PAYLOAD) {
		return 0;
	}

	out[DECTCAM_OFF_MAGIC + 0] = DECTCAM_MAGIC0;
	out[DECTCAM_OFF_MAGIC + 1] = DECTCAM_MAGIC1;
	out[DECTCAM_OFF_VERTYPE] = (uint8_t)((DECTCAM_VERSION << 4) | (type & 0x0Fu));
	dectcam_put_u16(&out[DECTCAM_OFF_FRAME_ID], frame_id);
	dectcam_put_u16(&out[DECTCAM_OFF_FRAG_IDX], frag_idx);
	dectcam_put_u16(&out[DECTCAM_OFF_FRAG_CNT], frag_cnt);
	dectcam_put_u16(&out[DECTCAM_OFF_PAYLOAD_LEN], payload_len);

	if (payload_len > 0 && payload != NULL) {
		for (uint16_t i = 0; i < payload_len; i++) {
			out[DECTCAM_OFF_PAYLOAD + i] = payload[i];
		}
	}

	/* CRC covers everything except the magic. */
	const uint16_t crc = dectcam_crc16(&out[DECTCAM_OFF_VERTYPE],
					   (size_t)(DECTCAM_HDR_LEN - 2u) + payload_len);
	dectcam_put_u16(&out[DECTCAM_OFF_PAYLOAD + payload_len], crc);

	return total;
}

/* ------------------------------------------------------------------ */
/* Decode                                                             */
/* ------------------------------------------------------------------ */

struct dectcam_pkt {
	uint8_t version;
	uint8_t type;
	uint16_t frame_id;
	uint16_t frag_idx;
	uint16_t frag_cnt;
	uint16_t payload_len;
	const uint8_t *payload; /* points into the caller's buffer */
};

enum dectcam_decode_result {
	DECTCAM_OK = 0,
	DECTCAM_ERR_SHORT = -1,   /* need more bytes, keep buffering */
	DECTCAM_ERR_MAGIC = -2,   /* no magic at offset 0 */
	DECTCAM_ERR_VERSION = -3, /* unknown protocol version */
	DECTCAM_ERR_LENGTH = -4,  /* implausible payload_len */
	DECTCAM_ERR_CRC = -5,     /* checksum mismatch */
};

/*
 * Try to decode one packet starting at buf[0].
 *
 * On DECTCAM_OK, *consumed is set to the packet length and *pkt is filled in
 * (pkt->payload aliases buf, so it is only valid while buf is).
 *
 * On DECTCAM_ERR_SHORT the caller should read more data and retry. On any
 * other error the caller should resynchronise by searching for the magic
 * starting one byte past buf[0]: a corrupted length field must never be
 * trusted to skip forward.
 */
static inline int dectcam_decode(const uint8_t *buf, size_t len,
				 struct dectcam_pkt *pkt, size_t *consumed)
{
	if (len < DECTCAM_HDR_LEN) {
		return DECTCAM_ERR_SHORT;
	}
	if (buf[0] != DECTCAM_MAGIC0 || buf[1] != DECTCAM_MAGIC1) {
		return DECTCAM_ERR_MAGIC;
	}

	const uint8_t vertype = buf[DECTCAM_OFF_VERTYPE];
	const uint8_t version = (uint8_t)(vertype >> 4);

	if (version != DECTCAM_VERSION) {
		return DECTCAM_ERR_VERSION;
	}

	const uint16_t payload_len = dectcam_get_u16(&buf[DECTCAM_OFF_PAYLOAD_LEN]);

	if (payload_len > DECTCAM_MAX_PAYLOAD) {
		return DECTCAM_ERR_LENGTH;
	}

	const size_t total = (size_t)payload_len + DECTCAM_OVERHEAD;

	if (len < total) {
		return DECTCAM_ERR_SHORT;
	}

	const uint16_t want = dectcam_crc16(&buf[DECTCAM_OFF_VERTYPE],
					    (size_t)(DECTCAM_HDR_LEN - 2u) + payload_len);
	const uint16_t got = dectcam_get_u16(&buf[DECTCAM_OFF_PAYLOAD + payload_len]);

	if (want != got) {
		return DECTCAM_ERR_CRC;
	}

	if (pkt != NULL) {
		pkt->version = version;
		pkt->type = (uint8_t)(vertype & 0x0Fu);
		pkt->frame_id = dectcam_get_u16(&buf[DECTCAM_OFF_FRAME_ID]);
		pkt->frag_idx = dectcam_get_u16(&buf[DECTCAM_OFF_FRAG_IDX]);
		pkt->frag_cnt = dectcam_get_u16(&buf[DECTCAM_OFF_FRAG_CNT]);
		pkt->payload_len = payload_len;
		pkt->payload = &buf[DECTCAM_OFF_PAYLOAD];
	}
	if (consumed != NULL) {
		*consumed = total;
	}
	return DECTCAM_OK;
}

/* ------------------------------------------------------------------ */
/* FRAME_META payload helpers                                         */
/* ------------------------------------------------------------------ */

struct dectcam_meta {
	uint16_t width;
	uint16_t height;
	uint8_t jpeg_quality;
	uint8_t target_fps;
	uint16_t max_payload;
};

static inline void dectcam_meta_pack(uint8_t out[DECTCAM_META_LEN],
				     const struct dectcam_meta *m)
{
	dectcam_put_u16(&out[0], m->width);
	dectcam_put_u16(&out[2], m->height);
	out[4] = m->jpeg_quality;
	out[5] = m->target_fps;
	dectcam_put_u16(&out[6], m->max_payload);
}

static inline void dectcam_meta_unpack(const uint8_t in[DECTCAM_META_LEN],
				       struct dectcam_meta *m)
{
	m->width = dectcam_get_u16(&in[0]);
	m->height = dectcam_get_u16(&in[2]);
	m->jpeg_quality = in[4];
	m->target_fps = in[5];
	m->max_payload = dectcam_get_u16(&in[6]);
}

/* ------------------------------------------------------------------ */
/* STATS payload helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Radio quality fields are carried in exactly the representation the modem
 * reports them (nrf_modem_dect_phy.h), so nothing is lost or rounded on the
 * way to the viewer:
 *
 *   rssi_2 : dBm in 0.5 dBm steps (Q14.1)  -> dBm = rssi_2 / 2.0
 *   snr    : dB  in 0.25 dB steps (Q13.2)  -> dB  = snr    / 4.0
 *
 * Sentinels, passed through unchanged:
 *   NRF_MODEM_DECT_PHY_RSSI2_NOT_MEASURED = -32768
 *   NRF_MODEM_DECT_PHY_SNR_NOT_MEASURED   = -32768
 */
#define DECTCAM_RSSI2_NOT_MEASURED (-32768)
#define DECTCAM_SNR_NOT_MEASURED   (-32768)

struct dectcam_stats {
	int16_t rssi_2; /* dBm * 2 */
	int16_t snr;    /* dB * 4 */
	uint32_t pdc_ok;
	uint32_t pdc_crc_err;
	uint32_t frags_out;

	/*
	 * Deepest the receiver's outbound ring got since the last statistics
	 * packet, in bytes.
	 *
	 * This is the delay between something happening and the PC seeing it,
	 * expressed as a queue: whatever stands here has been received off the
	 * air but not yet handed to the host. Divide by the byte rate to get
	 * seconds. It belongs in the packet rather than the log because the
	 * two virtual COM ports share one USB device, and while video is
	 * flowing on VCOM1 the console on VCOM0 is starved -- so the console
	 * is silent at exactly the moment the number matters.
	 */
	uint16_t ring_peak;
};

static inline void dectcam_stats_pack(uint8_t out[DECTCAM_STATS_LEN],
				      const struct dectcam_stats *s)
{
	dectcam_put_u16(&out[0], (uint16_t)s->rssi_2);
	dectcam_put_u16(&out[2], (uint16_t)s->snr);
	dectcam_put_u32(&out[4], s->pdc_ok);
	dectcam_put_u32(&out[8], s->pdc_crc_err);
	dectcam_put_u32(&out[12], s->frags_out);
	dectcam_put_u16(&out[16], s->ring_peak);
}

static inline void dectcam_stats_unpack(const uint8_t in[DECTCAM_STATS_LEN],
					struct dectcam_stats *s)
{
	s->rssi_2 = (int16_t)dectcam_get_u16(&in[0]);
	s->snr = (int16_t)dectcam_get_u16(&in[2]);
	s->pdc_ok = dectcam_get_u32(&in[4]);
	s->pdc_crc_err = dectcam_get_u32(&in[8]);
	s->frags_out = dectcam_get_u32(&in[12]);
	s->ring_peak = dectcam_get_u16(&in[16]);
}

/* ------------------------------------------------------------------ */
/* CTRL payload helpers                                               */
/* ------------------------------------------------------------------ */

/*
 * A control telegram.
 *
 * `seq` exists because control frames are sent repeatedly: the transmitter
 * only listens in short windows between video bursts, so a single copy would
 * usually be missed. The receiver of a telegram ignores a `seq` it has already
 * acted on, which makes retransmission free of side effects -- important when
 * the opcode is "step the transmit power down one".
 *
 * `arg32` is wide enough for any parameter so far and leaves room; `arg` is
 * simply its low half, kept as a separate accessor because every current
 * opcode fits in 16 bits.
 */
struct dectcam_ctrl {
	uint8_t opcode;
	uint8_t seq;
	uint32_t arg32;
};

static inline void dectcam_ctrl_pack(uint8_t out[DECTCAM_CTRL_LEN],
				     const struct dectcam_ctrl *c)
{
	out[0] = c->opcode;
	out[1] = c->seq;
	dectcam_put_u32(&out[2], c->arg32);
}

static inline void dectcam_ctrl_unpack(const uint8_t in[DECTCAM_CTRL_LEN],
				       struct dectcam_ctrl *c)
{
	c->opcode = in[0];
	c->seq = in[1];
	c->arg32 = dectcam_get_u32(&in[2]);
}

/* ------------------------------------------------------------------ */
/* RADIO_CFG payload helpers                                          */
/* ------------------------------------------------------------------ */

struct dectcam_radio_cfg {
	uint16_t carrier;
	uint8_t mcs;
	uint8_t tx_power;
	uint8_t subslots;
	uint8_t reserved;
	uint16_t frag_payload;
};

static inline void dectcam_radio_cfg_pack(uint8_t out[DECTCAM_RADIO_CFG_LEN],
					  const struct dectcam_radio_cfg *r)
{
	dectcam_put_u16(&out[0], r->carrier);
	out[2] = r->mcs;
	out[3] = r->tx_power;
	out[4] = r->subslots;
	out[5] = 0;
	dectcam_put_u16(&out[6], r->frag_payload);
}

static inline void dectcam_radio_cfg_unpack(const uint8_t in[DECTCAM_RADIO_CFG_LEN],
					    struct dectcam_radio_cfg *r)
{
	r->carrier = dectcam_get_u16(&in[0]);
	r->mcs = in[2];
	r->tx_power = in[3];
	r->subslots = in[4];
	r->reserved = in[5];
	r->frag_payload = dectcam_get_u16(&in[6]);
}

/* Build a complete CTRL packet. Returns its length, or 0 on overflow. */
static inline size_t dectcam_encode_ctrl(uint8_t *out, size_t out_cap, uint8_t opcode,
					 uint8_t seq, uint32_t arg32)
{
	const struct dectcam_ctrl c = {
		.opcode = opcode,
		.seq = seq,
		.arg32 = arg32,
	};
	uint8_t body[DECTCAM_CTRL_LEN];

	dectcam_ctrl_pack(body, &c);
	return dectcam_encode(out, out_cap, DECTCAM_TYPE_CTRL, 0, 0, 0, body, sizeof(body));
}

#ifdef __cplusplus
}
#endif

#endif /* DECTCAM_PROTO_H */
