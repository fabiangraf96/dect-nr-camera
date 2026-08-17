/*
 * uart_out.h - the link to the PC over VCOM1.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mostly outbound -- it exists to push video at the host -- but the same
 * full-duplex UART carries the operator's control packets the other way, so
 * steering the link needs no second cable.
 */

#ifndef DECTCAM_UART_OUT_H
#define DECTCAM_UART_OUT_H

#include <stdint.h>
#include <stddef.h>

int uart_out_init(void);

/*
 * Called from the UART callback for every chunk the host sends. Runs in
 * interrupt context: keep it short.
 */
typedef void (*uart_out_rx_cb_t)(const uint8_t *data, size_t len);
void uart_out_set_rx_cb(uart_out_rx_cb_t cb);

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
	uint32_t peak;	/* high water mark of the ring, in bytes */
};

void uart_out_get_stats(struct uart_out_stats *out);

/* Read and clear the ring high water mark. One caller only: see the note in
 * uart_out.c. */
uint32_t uart_out_take_peak(void);

#endif /* DECTCAM_UART_OUT_H */
