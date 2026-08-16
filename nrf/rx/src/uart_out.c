/*
 * uart_out.c - buffered UART transmission to the PC over VCOM1.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The modem callback must return quickly, so it only appends to a ring buffer.
 * A dedicated thread drains that into the UART using the async API, so
 * EasyDMA does the work.
 *
 * Packets are queued atomically: if a whole packet does not fit, it is dropped
 * entirely rather than truncated. A torn packet would cost the PC decoder a
 * resynchronisation and would very likely corrupt the frame either way, so
 * dropping is strictly better.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include "uart_out.h"
#include "dectcam_proto.h"

LOG_MODULE_DECLARE(dectcam_rx, CONFIG_DECTCAM_LOG_LEVEL);

static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));

RING_BUF_DECLARE(tx_ring, CONFIG_DECTCAM_RX_RING_SIZE);

/*
 * Guards the producer side of tx_ring. See uart_out_send() for why: the ring
 * buffer tolerates one producer, and this application has two.
 *
 * The consumer (pump_thread) needs no lock, since single-producer /
 * single-consumer is exactly the case the ring buffer is designed for once the
 * producers are serialised among themselves.
 */
static struct k_spinlock tx_lock;

/*
 * Two chunk buffers so the next DMA transfer can be prepared while the
 * current one is in flight.
 */
#define CHUNK_SZ 512
static uint8_t chunk[2][CHUNK_SZ];

static K_SEM_DEFINE(tx_done, 1, 1);
static K_SEM_DEFINE(data_ready, 0, 1);
static struct uart_out_stats s_stats;

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done);
		break;
	default:
		break;
	}
}

int uart_out_init(void)
{
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("uart2 not ready");
		return -ENODEV;
	}

	int err = uart_callback_set(uart_dev, uart_cb, NULL);

	if (err) {
		LOG_ERR("uart_callback_set failed: %d", err);
		return err;
	}

	LOG_INF("uart2 TX up @ %d baud (VCOM1), %d B ring", CONFIG_DECTCAM_RX_UART_BAUD,
		CONFIG_DECTCAM_RX_RING_SIZE);
	return 0;
}

int uart_out_send(const uint8_t *data, size_t len)
{
	if (len == 0) {
		return 0;
	}

	/*
	 * Serialise the producers.
	 *
	 * Zephyr's ring buffer is lock-free for ONE producer and ONE consumer.
	 * There are two producers here: the modem callback forwarding video,
	 * and the main loop emitting a statistics packet once a second. When
	 * the two interleave, both compute an allocation against the same head
	 * index, the second ring_buf_put_finish() reports a size that no longer
	 * fits, and its __ASSERT_NO_MSG fires:
	 *
	 *   ASSERTION FAIL [err == 0] @ zephyr/lib/utils/ring_buffer.c:72
	 *   ZEPHYR FATAL ERROR 4: Kernel panic
	 *
	 * The window is a few instructions wide and the statistics packet is
	 * sent once a second, so this took a long time to show up: the receiver
	 * had run for hours before it landed. A spinlock is the right size of
	 * fix -- the critical section is a memcpy of at most a few hundred
	 * bytes, and the modem callback must not block.
	 */
	k_spinlock_key_t key = k_spin_lock(&tx_lock);

	/*
	 * All-or-nothing. ring_buf_put() would happily write a prefix and
	 * report a short count, which is exactly the torn packet we must
	 * avoid, so check the space first.
	 */
	if (ring_buf_space_get(&tx_ring) < len) {
		s_stats.dropped++;
		k_spin_unlock(&tx_lock, key);
		return -ENOSPC;
	}

	const uint32_t put = ring_buf_put(&tx_ring, data, len);

	if (put != len) {
		/* Should be unreachable given the check above. */
		s_stats.dropped++;
		k_spin_unlock(&tx_lock, key);
		return -ENOSPC;
	}

	s_stats.queued++;
	s_stats.bytes += len;

	k_spin_unlock(&tx_lock, key);

	k_sem_give(&data_ready);
	return 0;
}

static void pump_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int idx = 0;

	for (;;) {
		if (ring_buf_is_empty(&tx_ring)) {
			k_sem_take(&data_ready, K_MSEC(100));
			continue;
		}

		/* Wait for the previous DMA transfer to finish. */
		if (k_sem_take(&tx_done, K_MSEC(1000)) != 0) {
			LOG_WRN("UART TX stalled; is the host asserting CTS?");
			continue;
		}

		const uint32_t n = ring_buf_get(&tx_ring, chunk[idx], CHUNK_SZ);

		if (n == 0) {
			k_sem_give(&tx_done);
			continue;
		}

		if (uart_tx(uart_dev, chunk[idx], n, SYS_FOREVER_US) != 0) {
			k_sem_give(&tx_done);
			k_msleep(1);
			continue;
		}
		idx ^= 1;
	}
}

K_THREAD_DEFINE(uart_out_pump, 1024, pump_thread, NULL, NULL, NULL, 5, 0, 0);

void uart_out_get_stats(struct uart_out_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
