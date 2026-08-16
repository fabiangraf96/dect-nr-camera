/*
 * uart_out.h - buffered UART transmission to the PC over VCOM1.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_UART_OUT_H
#define DECTCAM_UART_OUT_H

#include <stdint.h>
#include <stddef.h>

int uart_out_init(void);

/*
 * Queue a blob for transmission. Safe to call from an ISR / modem callback:
 * it only touches the ring buffer and signals the pump thread.
 *
 * Returns 0 on success, -ENOSPC if the buffer is full (the blob is dropped
 * whole, never partially, so the PC never sees a torn packet).
 */
int uart_out_send(const uint8_t *data, size_t len);

struct uart_out_stats {
	uint32_t queued;
	uint32_t dropped;
	uint64_t bytes;
};

void uart_out_get_stats(struct uart_out_stats *out);

#endif /* DECTCAM_UART_OUT_H */
