/*
 * link.h - framed UART link to the nRF9151 transmitter.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_LINK_H
#define DECTCAM_LINK_H

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t link_init(void);

/*
 * Fragment a JPEG image into dectcam packets and write them to the UART.
 * Blocks until everything has been handed to the driver.
 *
 * Returns the number of fragments sent, or a negative errno-style value.
 */
int link_send_frame(uint16_t frame_id, const uint8_t *jpeg, size_t len);

/* Announce the current stream parameters (type FRAME_META). */
esp_err_t link_send_meta(uint16_t width, uint16_t height, uint8_t quality, uint8_t fps);

/*
 * Poll the UART for a newline-terminated command from the nRF9151.
 *
 * This is the back-channel that makes runtime control possible without the
 * ESP32's own USB console: the nRF forwards text it receives on its shell.
 *
 * Returns the line length (NUL-terminated in `buf`), or 0 if no complete
 * line is available yet. Non-blocking.
 */
int link_poll_command(char *buf, size_t cap);

/* Counters for the console `stats` command. */
struct link_stats {
	uint32_t frames;
	uint32_t frags;
	uint64_t bytes;
	uint32_t tx_timeouts;   /* fragments abandoned because the link stalled */
	uint32_t frames_dropped;
	uint32_t rx_junk;       /* non-printable bytes dropped from the back-channel */
};

void link_get_stats(struct link_stats *out);

/* Payload bytes per fragment, from Kconfig. */
uint16_t link_frag_payload(void);

#ifdef __cplusplus
}
#endif

#endif /* DECTCAM_LINK_H */
