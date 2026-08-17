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

/* Stop the current receive operation and wait for it to wind down. */
int radio_stop_rx(void);

/*
 * Send one control telegram.
 *
 * The receiver is otherwise a listener; this exists so the PC can steer the
 * far end. Reception is stopped for the duration and restarted afterwards,
 * which costs a few milliseconds of video -- acceptable for something the
 * operator triggers by hand, and unavoidable with one radio.
 */
int radio_send_ctrl(const uint8_t *data, size_t len);

/* Retune the receiver. The far end must be moved separately and first. */
int radio_set_carrier(uint16_t carrier);

uint16_t radio_carrier(void);

uint16_t radio_device_id(void);

struct radio_stats {
	uint32_t pdc_ok;
	uint32_t pdc_crc_err;
	uint32_t pcc_ok;
	uint32_t pcc_crc_err;
	uint32_t rx_restarts;
	uint64_t rx_bytes;
	uint32_t ctrl_tx;     /* control telegrams put on the air */
	uint32_t ctrl_tx_err;
	int16_t last_rssi_2;  /* dBm * 2 */
	int16_t last_snr;     /* dB * 4  */
};

void radio_get_stats(struct radio_stats *out);

#endif /* DECTCAM_RADIO_H */
