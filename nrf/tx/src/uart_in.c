/*
 * uart_in.c - buffered UART reception from the camera board.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Uses the Zephyr async UART API so EasyDMA fills alternating buffers without
 * a per-byte interrupt; at 1 Mbaud alongside the radio there is no headroom
 * for interrupt-driven single-byte reception.
 *
 * Received bytes go into a ring buffer, and a consumer pulls whole dectcam
 * packets out of it, resynchronising on the magic whenever framing is lost.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "uart_in.h"
#include "dectcam_proto.h"

LOG_MODULE_DECLARE(dectcam_tx, CONFIG_DECTCAM_LOG_LEVEL);

/* Named by the board overlay, not here: the DK uses uart2 and the EK uart3,
 * because on the EK instance 2 belongs to the PMIC's i2c. */
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(dectcam_link_uart));

/*
 * Two DMA buffers, swapped by the driver. Each should hold a good fraction of
 * a packet so the swap rate stays low: at 1 Mbaud a 256-byte buffer fills in
 * 2.5 ms.
 */
#define DMA_BUF_SZ  256
#define DMA_BUF_CNT 2
/*
 * RX timeout: flush a partially filled DMA buffer after this long so a packet
 * that does not happen to end on a buffer boundary is not stuck waiting for
 * the next one.
 */
#define RX_TIMEOUT_US 2000

static uint8_t dma_buf[DMA_BUF_CNT][DMA_BUF_SZ];
static int dma_next;

RING_BUF_DECLARE(rx_ring, CONFIG_DECTCAM_TX_RING_SIZE);

static K_SEM_DEFINE(data_sem, 0, 1);
static struct uart_in_stats s_stats;

/*
 * Completion of an outgoing line.
 *
 * The back-channel to the camera shares this UART, and it has to use the same
 * async API the receive side uses. Mixing uart_poll_out() with an active
 * uart_rx_enable() on the nRF UARTE loses bytes: "cam fps 15" arrived at the
 * camera as "cam fps 1", which it obeyed. The last character before the
 * newline was the one that went missing, every time.
 */
static K_SEM_DEFINE(tx_done, 1, 1);

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done);
		break;

	case UART_RX_RDY: {
		const uint8_t *p = &evt->data.rx.buf[evt->data.rx.offset];
		const uint32_t n = evt->data.rx.len;
		const uint32_t put = ring_buf_put(&rx_ring, p, n);

		s_stats.bytes += n;
		if (put < n) {
			/*
			 * Consumer is behind. Dropping the newest bytes keeps
			 * the buffered data contiguous, so the decoder only has
			 * to resync once rather than repeatedly.
			 */
			s_stats.overruns++;
		}
		k_sem_give(&data_sem);
		break;
	}
	case UART_RX_BUF_REQUEST:
		/* Hand the driver the other buffer. */
		uart_rx_buf_rsp(dev, dma_buf[dma_next], DMA_BUF_SZ);
		dma_next = (dma_next + 1) % DMA_BUF_CNT;
		break;
	case UART_RX_DISABLED:
		/* Should not happen while streaming; restart so a transient
		 * error does not silently kill the link. */
		LOG_WRN("UART RX disabled, restarting");
		uart_rx_enable(dev, dma_buf[0], DMA_BUF_SZ, RX_TIMEOUT_US);
		dma_next = 1;
		break;
	case UART_RX_STOPPED:
		LOG_WRN("UART RX stopped, reason %d", evt->data.rx_stop.reason);
		break;
	default:
		break;
	}
}

int uart_in_init(void)
{
	int err;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("uart2 not ready");
		return -ENODEV;
	}

	err = uart_callback_set(uart_dev, uart_cb, NULL);
	if (err) {
		LOG_ERR("uart_callback_set failed: %d", err);
		return err;
	}

	err = uart_rx_enable(uart_dev, dma_buf[0], DMA_BUF_SZ, RX_TIMEOUT_US);
	if (err) {
		LOG_ERR("uart_rx_enable failed: %d", err);
		return err;
	}
	dma_next = 1;

	LOG_INF("uart2 RX up @ %d baud, %d B ring", CONFIG_DECTCAM_TX_UART_BAUD,
		CONFIG_DECTCAM_TX_RING_SIZE);
	return 0;
}

/* Discard bytes up to, but not including, the next plausible magic. */
static void resync(void)
{
	uint8_t b;

	/* Drop one byte, then scan for 0xD2. */
	ring_buf_get(&rx_ring, &b, 1);
	s_stats.resyncs++;

	while (ring_buf_size_get(&rx_ring) > 0) {
		if (ring_buf_peek(&rx_ring, &b, 1) != 1) {
			return;
		}
		if (b == DECTCAM_MAGIC0) {
			return;
		}
		ring_buf_get(&rx_ring, &b, 1);
	}
}

int uart_in_get_packet(uint8_t *out, size_t cap, int timeout_ms)
{
	const int64_t deadline = k_uptime_get() + timeout_ms;

	for (;;) {
		uint32_t avail = ring_buf_size_get(&rx_ring);

		if (avail >= DECTCAM_HDR_LEN) {
			uint8_t hdr[DECTCAM_HDR_LEN];

			ring_buf_peek(&rx_ring, hdr, sizeof(hdr));

			if (hdr[0] != DECTCAM_MAGIC0 || hdr[1] != DECTCAM_MAGIC1) {
				resync();
				continue;
			}

			const uint8_t version = hdr[DECTCAM_OFF_VERTYPE] >> 4;
			const uint16_t plen = dectcam_get_u16(&hdr[DECTCAM_OFF_PAYLOAD_LEN]);

			/*
			 * Reject an implausible length before it is used to
			 * size anything: a corrupted header must never be
			 * trusted to skip the stream forward.
			 */
			if (version != DECTCAM_VERSION || plen > DECTCAM_MAX_PAYLOAD ||
			    (size_t)plen + DECTCAM_OVERHEAD > cap) {
				resync();
				continue;
			}

			const size_t total = (size_t)plen + DECTCAM_OVERHEAD;

			if (avail >= total) {
				ring_buf_peek(&rx_ring, out, total);

				struct dectcam_pkt pkt;
				size_t consumed;

				if (dectcam_decode(out, total, &pkt, &consumed) == DECTCAM_OK) {
					/* NULL discards without copying again. */
					ring_buf_get(&rx_ring, NULL, total);
					s_stats.packets++;
					return (int)total;
				}
				s_stats.crc_err++;
				resync();
				continue;
			}
		}

		const int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return 0;
		}
		k_sem_take(&data_sem, K_MSEC(MIN(left, 20)));
	}
}

int uart_in_send_line(const char *line)
{
	static uint8_t out[96];
	size_t n = 0;

	if (!device_is_ready(uart_dev)) {
		return -ENODEV;
	}

	while (line[n] != '\0' && n < sizeof(out) - 1) {
		out[n] = (uint8_t)line[n];
		n++;
	}
	out[n++] = '\n';

	/* Wait for any previous line to finish before reusing the buffer. */
	if (k_sem_take(&tx_done, K_MSEC(200)) != 0) {
		return -EBUSY;
	}

	const int err = uart_tx(uart_dev, out, n, SYS_FOREVER_US);

	if (err) {
		k_sem_give(&tx_done);
		return err;
	}

	/*
	 * Block until it is on the wire. The caller is the control path, which
	 * runs at human speed, and returning early would let the next line
	 * overwrite this one mid-transmission.
	 */
	if (k_sem_take(&tx_done, K_MSEC(200)) != 0) {
		return -ETIMEDOUT;
	}
	k_sem_give(&tx_done);
	return 0;
}

void uart_in_get_stats(struct uart_in_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
