/*
 * uart_in.h - buffered UART reception from the camera board.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_UART_IN_H
#define DECTCAM_UART_IN_H

#include <stdint.h>
#include <stddef.h>

int uart_in_init(void);

/*
 * Pop one complete, CRC-verified packet into `out`.
 *
 * Blocks up to `timeout_ms` for a packet to become available.
 * Returns the packet length, 0 on timeout, or negative on error.
 */
int uart_in_get_packet(uint8_t *out, size_t cap, int timeout_ms);

struct uart_in_stats {
	uint32_t packets;
	uint32_t crc_err;
	uint32_t resyncs;
	uint32_t overruns; /* ring buffer overflowed, bytes discarded */
	uint64_t bytes;
};

void uart_in_get_stats(struct uart_in_stats *out);

#endif /* DECTCAM_UART_IN_H */
