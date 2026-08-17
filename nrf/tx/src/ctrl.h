/*
 * ctrl.h - control telegrams received over the air, transmitter side.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_TX_CTRL_H
#define DECTCAM_TX_CTRL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Hand a packet received during a listen window to the control logic. */
void ctrl_on_packet(const uint8_t *data, size_t len);

/*
 * Give the control logic a slice of the main loop.
 *
 * Opens a listen window when one is due, applies anything that arrived, and
 * runs the carrier rollback timer. Returns the number of milliseconds the
 * caller should not transmit for, which is zero unless a window is open.
 */
void ctrl_poll(void);

/*
 * Fill `out` with the acknowledgement that is waiting to go into the video
 * stream, if any. Returns its length, or 0 when there is nothing to send.
 *
 * Acknowledgements travel with the video because that direction already
 * exists and already works; adding a second radio turnaround for them would
 * cost air time for no benefit.
 */
size_t ctrl_pending_ack(uint8_t *out, size_t cap);

struct ctrl_stats {
	uint32_t applied;
	uint32_t duplicates;   /* repeats of a telegram already acted on */
	uint32_t rejected;
	uint32_t carrier_moves;
	uint32_t carrier_rollbacks;
};

void ctrl_get_stats(struct ctrl_stats *out);

#endif /* DECTCAM_TX_CTRL_H */
