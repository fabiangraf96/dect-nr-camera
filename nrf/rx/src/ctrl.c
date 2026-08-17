/*
 * ctrl.c - control from the PC, receiver side.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The operator's commands arrive here, on the same USB serial port that
 * carries video the other way, and are relayed over the air to the
 * transmitter. This side is the one that knows whether a command worked,
 * because it can see the acknowledgement come back in the video stream.
 *
 * A telegram is repeated until it is acknowledged or the attempt times out.
 * That is not belt and braces: the transmitter only listens for 20 ms out of
 * every 250, so a single copy would usually be missed.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ctrl.h"
#include "radio.h"
#include "dectcam_cfg.h"

LOG_MODULE_DECLARE(dectcam_rx, CONFIG_DECTCAM_LOG_LEVEL);

/*
 * A burst of copies, close enough together that any listen window at the far
 * end catches several.
 *
 * The transmitter listens for 30 ms out of every 200. Copies 25 ms apart mean
 * at least one lands in every window, and 40 of them span a full second --
 * five windows. Reception is stopped for that second, so this is deliberately
 * a one-off cost rather than a background trickle.
 */
#define BURST_COUNT  40
#define BURST_GAP_MS 25

/* How long to wait for the acknowledgement, and how many bursts to try. */
#define ACK_WAIT_MS  1500
#define MAX_BURSTS   3

/*
 * Carrier rollback deadline.
 *
 * Deliberately shorter than the transmitter's 6 s trial. If the move fails,
 * this end returns first and is already listening on the old carrier by the
 * time the far end follows -- the two converge instead of crossing over, which
 * is what would happen if both used the same deadline and drifted apart.
 */
#define CARRIER_TRIAL_MS 4000

static struct ctrl_stats s_stats;

/* The telegram currently being sent, if any. */
static bool s_busy;
static uint8_t s_opcode;
static uint32_t s_arg;
static uint8_t s_seq;
static int s_bursts_done;
static int64_t s_wait_until;

/* Carrier trial state. */
static bool s_carrier_trial;
static uint16_t s_carrier_previous;
static uint16_t s_carrier_target;
static int64_t s_carrier_deadline;
static uint32_t s_pdc_at_switch;

/*
 * Sequence numbers for telegrams this end originates rather than relays --
 * currently only the carrier confirmation. Kept well away from the host's
 * numbering by counting down from the top, so the two cannot collide and
 * suppress one another as duplicates.
 */
static uint8_t s_own_seq = 255;

/* Reassembles packets arriving from the host, which need not be aligned. */
static uint8_t s_host_buf[128];
static size_t s_host_len;

/*
 * Start sending a telegram.
 *
 * `seq` comes from the host and is carried through unchanged, so the
 * acknowledgement that eventually comes back can be matched to the command
 * that caused it -- by the transmitter for de-duplication, and by the operator
 * at the far end of the USB cable.
 *
 * De-duplication at the far end compares against the previous sequence only,
 * so a host that restarts and reuses a number does not lose a command; it
 * would have to reuse the number twice in a row.
 */
static void send_burst(uint8_t opcode, uint8_t seq, uint32_t arg)
{
	uint8_t pkt[DECTCAM_CTRL_LEN + DECTCAM_OVERHEAD];
	const size_t n = dectcam_encode_ctrl(pkt, sizeof(pkt), opcode, seq, arg);

	if (n == 0) {
		return;
	}
	if (radio_send_ctrl_burst(pkt, n, BURST_COUNT, BURST_GAP_MS) == 0) {
		s_stats.sent++;
	}
}

static void start(uint8_t opcode, uint8_t seq, uint32_t arg)
{
	s_busy = true;
	s_opcode = opcode;
	s_arg = arg;
	s_seq = seq;
	s_bursts_done = 0;
	s_wait_until = 0; /* burst immediately */
}

/*
 * Begin a carrier change.
 *
 * The order matters. The new carrier is announced on the OLD one, where the
 * far end is still listening; only once that has been acknowledged does this
 * end retune. If the far end never arrives on the new carrier, both sides give
 * up and return -- see the deadline above.
 */
static void begin_carrier_change(uint8_t seq, uint16_t carrier)
{
	s_carrier_previous = radio_carrier();
	s_carrier_target = carrier;
	start(DECTCAM_CTRL_SET_CARRIER, seq, carrier);
	LOG_WRN("moving carrier %u -> %u", s_carrier_previous, carrier);
}

void ctrl_init(void)
{
	s_host_len = 0;
}

static void handle_from_host(const struct dectcam_ctrl *c)
{
	s_stats.from_host++;

	if (s_busy) {
		LOG_WRN("a command is already in flight; ignoring opcode %u", c->opcode);
		return;
	}

	if (c->opcode == DECTCAM_CTRL_SET_CARRIER) {
		begin_carrier_change(c->seq, (uint16_t)c->arg32);
	} else {
		start(c->opcode, c->seq, c->arg32);
	}
}

void ctrl_feed_host(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s_host_len < sizeof(s_host_buf)) {
			s_host_buf[s_host_len++] = data[i];
		} else {
			/* Nothing valid in a full buffer: start over. */
			s_host_len = 0;
		}

		/* Try to pull a packet out of what we have so far. */
		while (s_host_len >= DECTCAM_HDR_LEN) {
			struct dectcam_pkt pkt;
			size_t consumed = 0;
			const int rc = dectcam_decode(s_host_buf, s_host_len, &pkt, &consumed);

			if (rc == DECTCAM_ERR_SHORT) {
				break;
			}
			if (rc != DECTCAM_OK) {
				/* Resynchronise one byte at a time. */
				memmove(s_host_buf, s_host_buf + 1, --s_host_len);
				continue;
			}

			if (pkt.type == DECTCAM_TYPE_CTRL &&
			    pkt.payload_len >= DECTCAM_CTRL_LEN) {
				struct dectcam_ctrl c;

				dectcam_ctrl_unpack(pkt.payload, &c);
				handle_from_host(&c);
			}

			memmove(s_host_buf, s_host_buf + consumed, s_host_len - consumed);
			s_host_len -= consumed;
		}
	}
}

void ctrl_on_air_packet(const uint8_t *data, size_t len)
{
	struct dectcam_pkt pkt;
	size_t consumed;

	if (dectcam_decode(data, len, &pkt, &consumed) != DECTCAM_OK) {
		return;
	}
	if (pkt.type != DECTCAM_TYPE_CTRL || pkt.payload_len < DECTCAM_CTRL_LEN) {
		return;
	}

	struct dectcam_ctrl c;

	dectcam_ctrl_unpack(pkt.payload, &c);

	if (c.opcode != DECTCAM_CTRL_ACK || !s_busy || c.seq != s_seq) {
		return;
	}

	s_busy = false;
	s_stats.acked++;

	if (c.arg32 != DECTCAM_ACK_OK) {
		LOG_WRN("far end rejected opcode %u (status %u)", s_opcode,
			(unsigned)c.arg32);
		return;
	}

	/*
	 * The far end has taken the new carrier. Follow it, and start the
	 * clock: if no video appears there, both ends go back.
	 */
	if (s_opcode == DECTCAM_CTRL_SET_CARRIER) {
		struct radio_stats rs;

		radio_get_stats(&rs);
		s_pdc_at_switch = rs.pdc_ok;
		s_carrier_trial = true;
		s_carrier_deadline = k_uptime_get() + CARRIER_TRIAL_MS;
		s_stats.carrier_moves++;
		radio_set_carrier(s_carrier_target);
		LOG_WRN("now listening on %u; returning to %u in %d ms unless video appears",
			s_carrier_target, s_carrier_previous, CARRIER_TRIAL_MS);
	}
}

void ctrl_poll(void)
{
	const int64_t now = k_uptime_get();

	if (s_carrier_trial) {
		struct radio_stats rs;

		radio_get_stats(&rs);

		if (rs.pdc_ok > s_pdc_at_switch) {
			/* Video is arriving: the move worked. Tell the far end
			 * so it stops its own rollback timer. */
			s_carrier_trial = false;
			LOG_INF("video on carrier %u; confirming", s_carrier_target);
			send_burst(DECTCAM_CTRL_CARRIER_OK, s_own_seq--, s_carrier_target);
			if (s_own_seq < 128) {
				s_own_seq = 255;
			}
		} else if (now >= s_carrier_deadline) {
			s_carrier_trial = false;
			s_stats.carrier_rollbacks++;
			LOG_ERR("no video on %u; returning to %u", s_carrier_target,
				s_carrier_previous);
			radio_set_carrier(s_carrier_previous);
		}
	}

	if (!s_busy) {
		return;
	}

	/* Still waiting for the acknowledgement from the last burst. */
	if (now < s_wait_until) {
		return;
	}

	if (s_bursts_done >= MAX_BURSTS) {
		s_busy = false;
		s_stats.timed_out++;
		LOG_WRN("no acknowledgement for opcode %u after %d bursts", s_opcode,
			MAX_BURSTS);
		return;
	}

	s_bursts_done++;
	send_burst(s_opcode, s_seq, s_arg);
	s_wait_until = k_uptime_get() + ACK_WAIT_MS;
}

void ctrl_get_stats(struct ctrl_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
