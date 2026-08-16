/*
 * main.c - DECT NR+ camera link, transmitter.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reads dectcam packets from the camera board over uart2 and retransmits each one
 * verbatim as a DECT NR+ PDC. The packet format is chosen so that exactly one
 * packet fits one transport block, so this side never has to parse or
 * re-fragment anything: it is a pure pump.
 *
 * See docs/architecture.md for the whole picture.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "radio.h"
#include "uart_in.h"
#include "dectcam_cfg.h"

LOG_MODULE_REGISTER(dectcam_tx, CONFIG_DECTCAM_LOG_LEVEL);

/* LED0 = radio up, LED1 = TX activity. */
static const struct gpio_dt_spec led_up = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_tx = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static uint8_t pkt_buf[DECTCAM_MAX_PACKET];

static void leds_init(void)
{
	if (gpio_is_ready_dt(&led_up)) {
		gpio_pin_configure_dt(&led_up, GPIO_OUTPUT_INACTIVE);
	}
	if (gpio_is_ready_dt(&led_tx)) {
		gpio_pin_configure_dt(&led_tx, GPIO_OUTPUT_INACTIVE);
	}
}

#if CONFIG_DECTCAM_TX_TEST_PATTERN
/*
 * Synthetic frames so the radio link can be characterised without the camera
 * or the ESP32 attached. The payload is a counting pattern, which makes any
 * corruption obvious in a hex dump at the far end.
 */
#include "dectcam_proto.h"

static int send_test_frame(uint16_t frame_id)
{
	const uint16_t frag_payload = DECTCAM_FRAG_PAYLOAD;
	const uint16_t total = CONFIG_DECTCAM_TX_TEST_PATTERN_SIZE;
	const uint16_t nfrag = (total + frag_payload - 1) / frag_payload;
	static uint8_t body[DECTCAM_MAX_PAYLOAD];
	int sent = 0;

	for (uint16_t i = 0; i < nfrag; i++) {
		const uint16_t chunk =
			MIN(frag_payload, (uint16_t)(total - i * frag_payload));

		for (uint16_t k = 0; k < chunk; k++) {
			body[k] = (uint8_t)(frame_id + i + k);
		}

		const size_t plen = dectcam_encode(pkt_buf, sizeof(pkt_buf),
						   DECTCAM_TYPE_JPEG_FRAG, frame_id, i, nfrag,
						   body, chunk);

		if (plen == 0 || radio_tx(pkt_buf, plen) != 0) {
			return sent;
		}
		sent++;
	}
	return sent;
}
#endif

int main(void)
{
	LOG_INF("DECT camera transmitter starting");

	leds_init();

	if (radio_init() != 0) {
		LOG_ERR("radio init failed; halting");
		return -EIO;
	}
	gpio_pin_set_dt(&led_up, 1);

#if !CONFIG_DECTCAM_TX_TEST_PATTERN
	if (uart_in_init() != 0) {
		LOG_ERR("uart init failed; halting");
		return -EIO;
	}
#else
	LOG_WRN("TEST PATTERN mode: uart input is not used");
#endif

	int64_t last_stats = k_uptime_get();
	uint32_t tx_since = 0;
	uint64_t bytes_since = 0;

#if CONFIG_DECTCAM_TX_TEST_PATTERN
	uint16_t frame_id = 0;
	const int64_t period_ms = 1000 / CONFIG_DECTCAM_TX_TEST_PATTERN_FPS;
#endif

	for (;;) {
#if CONFIG_DECTCAM_TX_TEST_PATTERN
		const int64_t t0 = k_uptime_get();
		int n = send_test_frame(frame_id++);

		tx_since += n;
		bytes_since += (uint64_t)n * DECTCAM_TBS_BYTES;
		gpio_pin_toggle_dt(&led_tx);

		const int64_t spent = k_uptime_get() - t0;

		if (spent < period_ms) {
			k_msleep(period_ms - spent);
		}
#else
		int len = uart_in_get_packet(pkt_buf, sizeof(pkt_buf), 100);

		if (len > 0) {
			if (radio_tx(pkt_buf, len) == 0) {
				tx_since++;
				bytes_since += len;
				/*
				 * Toggling per packet would be a blur; every
				 * 16 gives a visible flicker that tracks
				 * activity.
				 */
				if ((tx_since & 0x0f) == 0) {
					gpio_pin_toggle_dt(&led_tx);
				}
			}
		}
#endif

#if CONFIG_DECTCAM_STATS_INTERVAL_MS > 0
		const int64_t now = k_uptime_get();

		if (now - last_stats >= CONFIG_DECTCAM_STATS_INTERVAL_MS) {
			const double secs = (now - last_stats) / 1000.0;
			struct radio_stats rs;

			radio_get_stats(&rs);

#if CONFIG_DECTCAM_TX_TEST_PATTERN
			LOG_INF("tx %.1f pkt/s, %.1f kB/s | ok %u err %u lbt_busy %u",
				tx_since / secs, bytes_since / secs / 1000.0, rs.tx_ok,
				rs.tx_err, rs.tx_lbt_busy);
#else
			struct uart_in_stats us;

			uart_in_get_stats(&us);
			LOG_INF("tx %.1f pkt/s, %.1f kB/s | radio ok %u err %u lbt %u | "
				"uart pkt %u crc %u resync %u ovr %u",
				tx_since / secs, bytes_since / secs / 1000.0, rs.tx_ok,
				rs.tx_err, rs.tx_lbt_busy, us.packets, us.crc_err, us.resyncs,
				us.overruns);
#endif
			tx_since = 0;
			bytes_since = 0;
			last_stats = now;
		}
#endif
	}

	return 0;
}
