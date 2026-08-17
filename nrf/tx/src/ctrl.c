/*
 * ctrl.c - control telegrams received over the air, transmitter side.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The operator drives everything from the PC, which is attached to the
 * receiver. Commands therefore travel against the video: PC -> receiver ->
 * air -> here. Anything aimed at the camera is passed on over the UART
 * back-channel that already exists for that purpose.
 *
 * Two things shape the design:
 *
 *   - There is one radio, and it is busy pushing video. Listening means not
 *     transmitting, so the window is short and infrequent, and the sender has
 *     to repeat a telegram until one lands. Repetition is made safe by the
 *     sequence number: a telegram already acted on is counted and ignored.
 *
 *   - Changing the carrier moves the link itself. If only one end moves, the
 *     link is gone and no further command can reach anyone. That case is
 *     handled by moving on trial and returning automatically -- see below.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ctrl.h"
#include "radio.h"
#include "camctl.h"
#include "dectcam_cfg.h"

LOG_MODULE_DECLARE(dectcam_tx, CONFIG_DECTCAM_LOG_LEVEL);

/*
 * How often to stop transmitting and listen, and for how long.
 *
 * 30 ms out of every 200 costs 15 % of the air time. That sounds like a lot,
 * but the link runs at about half the capacity it has, so it costs no frames
 * in practice -- and it has to be this generous: the far end sends its copies
 * 25 ms apart, so a window narrower than that could fall between two of them
 * and hear nothing at all.
 */
#define LISTEN_PERIOD_MS 200
#define LISTEN_MS        30

/*
 * Carrier change, and why it is done the way it is.
 *
 * The receiver announces the new carrier on the OLD one, then moves. This side
 * moves as soon as it has acknowledged. From that moment the link is unproven:
 * if the receiver never arrives -- wrong channel, interference, a command that
 * was corrupted into a different number -- nothing more can be said to either
 * end, and the only recovery would be a power cycle.
 *
 * So the move is on trial. The receiver confirms with CARRIER_OK once it is
 * actually seeing video on the new carrier. If that confirmation does not
 * arrive in time, this side returns to the previous carrier on its own. The
 * receiver runs the same rollback on a shorter deadline, so the two converge
 * on the old carrier rather than crossing over.
 */
#define CARRIER_TRIAL_MS 6000

static struct ctrl_stats s_stats;

/* Last sequence acted on, so repeats are free of side effects. */
static bool s_have_last_seq;
static uint8_t s_last_seq;

/*
 * Acknowledgement waiting to be slipped into the video stream.
 *
 * Repeated on a clock rather than a count, and for longer than feels
 * necessary, because of when it has to arrive: the far end transmits its
 * command as a one-second burst, and while it is transmitting it is deaf. An
 * acknowledgement sent promptly -- which is what happens, since the command is
 * applied on the first copy that lands -- is therefore usually sent straight
 * into that deafness. Repeating for well past the end of the burst is what
 * makes it land.
 */
#define ACK_REPEAT_MS 150
#define ACK_FOR_MS    2500

static bool s_ack_pending;
static uint8_t s_ack_seq;
static uint8_t s_ack_status;
static int64_t s_ack_until;
static int64_t s_ack_next;

/* Carrier trial state. */
static bool s_carrier_trial;
static uint16_t s_carrier_previous;
static int64_t s_carrier_deadline;

static int64_t s_next_listen;

static void queue_ack(uint8_t seq, uint8_t status)
{
	const int64_t now = k_uptime_get();

	s_ack_pending = true;
	s_ack_seq = seq;
	s_ack_status = status;
	s_ack_until = now + ACK_FOR_MS;
	s_ack_next = now;
}

static uint8_t apply(uint8_t opcode, uint32_t arg)
{
	switch (opcode) {
	case DECTCAM_CTRL_SET_TX_POWER:
		return radio_set_tx_power((uint8_t)arg) == 0
			       ? DECTCAM_ACK_OK
			       : DECTCAM_ACK_BAD_ARG;

	case DECTCAM_CTRL_SET_MCS:
		return radio_set_mcs((uint8_t)arg) == 0
			       ? DECTCAM_ACK_OK
			       : DECTCAM_ACK_BAD_ARG;

	case DECTCAM_CTRL_SET_CARRIER: {
		struct radio_cfg cfg;

		radio_get_cfg(&cfg);
		if (arg == 0 || arg > 0xffff) {
			return DECTCAM_ACK_BAD_ARG;
		}
		if ((uint16_t)arg == cfg.carrier) {
			return DECTCAM_ACK_OK;
		}

		s_carrier_previous = cfg.carrier;
		if (radio_set_carrier((uint16_t)arg) != 0) {
			return DECTCAM_ACK_FAILED;
		}
		s_carrier_trial = true;
		s_carrier_deadline = k_uptime_get() + CARRIER_TRIAL_MS;
		s_stats.carrier_moves++;
		LOG_WRN("carrier %u -> %u on trial; returning in %d ms unless confirmed",
			s_carrier_previous, (uint16_t)arg, CARRIER_TRIAL_MS);
		return DECTCAM_ACK_OK;
	}

	case DECTCAM_CTRL_CARRIER_OK:
		if (s_carrier_trial) {
			s_carrier_trial = false;
			LOG_INF("carrier confirmed by the receiver");
		}
		return DECTCAM_ACK_OK;

	/* Everything below belongs to the camera, not the radio. */
	case DECTCAM_CTRL_SET_RES:
		return camctl_send_res((uint8_t)arg) == 0 ? DECTCAM_ACK_OK
							  : DECTCAM_ACK_BAD_ARG;
	case DECTCAM_CTRL_SET_FPS:
		return camctl_send_fps((uint8_t)arg) == 0 ? DECTCAM_ACK_OK
							  : DECTCAM_ACK_BAD_ARG;
	case DECTCAM_CTRL_SET_QUALITY:
		return camctl_send_quality((uint8_t)arg) == 0 ? DECTCAM_ACK_OK
							      : DECTCAM_ACK_BAD_ARG;

	case DECTCAM_CTRL_NOP:
		return DECTCAM_ACK_OK;

	default:
		return DECTCAM_ACK_UNSUPP;
	}
}

/*
 * Telegram waiting to be acted on.
 *
 * ctrl_on_packet() runs in the modem's event callback -- interrupt context --
 * and must do as little as possible there. Applying a telegram means writing
 * to a UART and logging, neither of which belongs in an interrupt: doing it
 * there truncated a "cam fps 15" into "cam fps 1", which the camera dutifully
 * obeyed.
 *
 * So the callback only parks the telegram, and ctrl_poll() applies it from the
 * main loop. One slot is enough: telegrams arrive at human speed and repeats
 * of the same one are collapsed by the sequence check anyway.
 */
static volatile bool s_inbox_full;
static struct dectcam_ctrl s_inbox;

void ctrl_on_packet(const uint8_t *data, size_t len)
{
	struct dectcam_pkt pkt;
	size_t consumed;

	if (dectcam_decode(data, len, &pkt, &consumed) != DECTCAM_OK) {
		return;
	}
	if (pkt.type != DECTCAM_TYPE_CTRL || pkt.payload_len < DECTCAM_CTRL_LEN) {
		return;
	}
	if (s_inbox_full) {
		/* Still working on the previous one; this is almost certainly a
		 * repeat of it, and the far end will send it again. */
		return;
	}

	dectcam_ctrl_unpack(pkt.payload, &s_inbox);
	s_inbox_full = true;
}

static void process_inbox(void)
{
	if (!s_inbox_full) {
		return;
	}

	const struct dectcam_ctrl c = s_inbox;

	s_inbox_full = false;

	/*
	 * A repeat of something already done. Acknowledge it again -- the
	 * previous acknowledgement evidently did not get through, or the far
	 * end would have stopped repeating -- but do not act on it twice.
	 */
	if (s_have_last_seq && c.seq == s_last_seq) {
		s_stats.duplicates++;
		queue_ack(c.seq, DECTCAM_ACK_OK);
		return;
	}

	const uint8_t status = apply(c.opcode, c.arg32);

	s_have_last_seq = true;
	s_last_seq = c.seq;

	if (status == DECTCAM_ACK_OK) {
		s_stats.applied++;
	} else {
		s_stats.rejected++;
		LOG_WRN("control opcode %u arg %u rejected: status %u", c.opcode,
			(unsigned)c.arg32, status);
	}
	queue_ack(c.seq, status);
}

void ctrl_poll(void)
{
	const int64_t now = k_uptime_get();

	process_inbox();

	/* Carrier trial expired without confirmation: go back. */
	if (s_carrier_trial && now >= s_carrier_deadline) {
		s_carrier_trial = false;
		s_stats.carrier_rollbacks++;
		LOG_ERR("carrier not confirmed; returning to %u", s_carrier_previous);
		radio_set_carrier(s_carrier_previous);
	}

	if (now < s_next_listen) {
		return;
	}
	s_next_listen = now + LISTEN_PERIOD_MS;

	radio_listen(LISTEN_MS);
}

size_t ctrl_pending_ack(uint8_t *out, size_t cap)
{
	if (!s_ack_pending) {
		return 0;
	}

	const int64_t now = k_uptime_get();

	if (now >= s_ack_until) {
		s_ack_pending = false;
		return 0;
	}
	if (now < s_ack_next) {
		return 0;
	}
	s_ack_next = now + ACK_REPEAT_MS;

	const size_t n = dectcam_encode_ctrl(out, cap, DECTCAM_CTRL_ACK, s_ack_seq,
					     s_ack_status);

	if (n == 0) {
		s_ack_pending = false;
	}
	return n;
}

void ctrl_get_stats(struct ctrl_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
