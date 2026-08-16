/*
 * radio.h - DECT NR+ PHY transmit wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_RADIO_H
#define DECTCAM_RADIO_H

#include <stdint.h>
#include <stddef.h>

/* Bring up the modem, the DECT PHY, and activate the radio. Blocking. */
int radio_init(void);

/*
 * Transmit one packet. Blocks until the operation completes so the caller
 * cannot outrun the modem.
 *
 * Returns 0 on success, or a negative errno.
 */
int radio_tx(const uint8_t *data, size_t len);

uint16_t radio_device_id(void);

struct radio_stats {
	uint32_t tx_ok;
	uint32_t tx_err;
	uint32_t tx_lbt_busy;
	uint64_t tx_bytes;
};

void radio_get_stats(struct radio_stats *out);

#endif /* DECTCAM_RADIO_H */
