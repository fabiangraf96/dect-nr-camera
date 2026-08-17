/*
 * radio.h - DECT NR+ PHY wrapper for the transmitter.
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

/*
 * Listen for `ms` milliseconds and hand any received packet to the callback
 * registered with radio_set_ctrl_cb(). Blocking.
 *
 * The transmitter has no permanent receive path: it spends most of its time
 * pushing video, and turning the radio around costs air time. Control is rare
 * and small, so the caller opens a short window between video bursts instead.
 * The other end therefore has to repeat a telegram for as long as one window
 * period, which is why control frames carry a sequence number.
 */
int radio_listen(uint32_t ms);

/* Called from the modem callback for every packet received while listening. */
typedef void (*radio_ctrl_cb_t)(const uint8_t *data, size_t len);
void radio_set_ctrl_cb(radio_ctrl_cb_t cb);

uint16_t radio_device_id(void);

/*
 * Live radio parameters.
 *
 * These start at their Kconfig values and can be changed at run time from the
 * receiver. Carrier is the delicate one: both ends must move together, so it
 * is not changed here directly -- see ctrl.c for the sequence that does it.
 */
struct radio_cfg {
	uint16_t carrier;
	uint8_t mcs;
	uint8_t tx_power;
	uint8_t subslots;
	uint16_t tbs_bytes;      /* transport block for the current mcs/subslots */
	uint16_t frag_payload;   /* tbs_bytes minus the protocol overhead        */
};

void radio_get_cfg(struct radio_cfg *out);

/*
 * Apply a new value. Each returns 0, or -EINVAL if the value is out of range
 * or the resulting transport block could not carry the fragments in use.
 */
int radio_set_tx_power(uint8_t idx);
int radio_set_mcs(uint8_t mcs);
int radio_set_carrier(uint16_t carrier);

struct radio_stats {
	uint32_t tx_ok;
	uint32_t tx_err;
	uint32_t tx_lbt_busy;
	uint64_t tx_bytes;
	uint32_t ctrl_rx;        /* control packets received while listening */
	uint32_t listen_windows;
};

void radio_get_stats(struct radio_stats *out);

#endif /* DECTCAM_RADIO_H */
