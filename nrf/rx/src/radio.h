/*
 * radio.h - DECT NR+ PHY receive wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_RADIO_H
#define DECTCAM_RADIO_H

#include <stdint.h>
#include <stddef.h>

/*
 * Called from the modem event handler for every PDC that passes CRC.
 * Keep it short: it runs in the modem callback context.
 */
typedef void (*radio_pdc_cb_t)(const uint8_t *data, size_t len);

int radio_init(radio_pdc_cb_t cb);

/* Start (or restart) continuous reception. */
int radio_start_rx(void);

uint16_t radio_device_id(void);

struct radio_stats {
	uint32_t pdc_ok;
	uint32_t pdc_crc_err;
	uint32_t pcc_ok;
	uint32_t pcc_crc_err;
	uint32_t rx_restarts;
	uint64_t rx_bytes;
	int16_t last_rssi_2; /* dBm * 2 */
	int16_t last_snr;    /* dB * 4  */
};

void radio_get_stats(struct radio_stats *out);

#endif /* DECTCAM_RADIO_H */
