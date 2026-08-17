/*
 * ctrl.h - control from the PC, receiver side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_RX_CTRL_H
#define DECTCAM_RX_CTRL_H

#include <stdint.h>
#include <stddef.h>

void ctrl_init(void);

/*
 * Feed bytes arriving from the PC on the data port.
 *
 * That port is normally write-only from this end -- it carries video to the
 * host -- but it is a full-duplex UART, so the host can send control packets
 * back along it without any extra wiring.
 */
void ctrl_feed_host(const uint8_t *data, size_t len);

/* Hand a packet that arrived over the air to the control logic. */
void ctrl_on_air_packet(const uint8_t *data, size_t len);

/* Give the control logic a slice of the main loop: repeats and timeouts. */
void ctrl_poll(void);

struct ctrl_stats {
	uint32_t from_host;
	uint32_t sent;          /* telegrams put on the air, including repeats */
	uint32_t acked;
	uint32_t timed_out;
	uint32_t carrier_moves;
	uint32_t carrier_rollbacks;
};

void ctrl_get_stats(struct ctrl_stats *out);

#endif /* DECTCAM_RX_CTRL_H */
