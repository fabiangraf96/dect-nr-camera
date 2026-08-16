/*
 * main.c - DECT NR+ camera link, receiver.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Receives dectcam packets over DECT NR+ and forwards each one verbatim to the
 * PC over VCOM1. Like the transmitter, this side does not interpret the video
 * at all: it is a pump with a statistics side-channel.
 *
 * The PC sees the SECOND serial port the DK enumerates (VCOM1), and must open
 * it with RTS/CTS enabled -- see docs/wiring.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "radio.h"
#include "uart_out.h"
#include "dectcam_cfg.h"

LOG_MODULE_REGISTER(dectcam_rx, CONFIG_DECTCAM_LOG_LEVEL);

/* LED0 = radio up, LED1 = RX activity. */
static const struct gpio_dt_spec led_up = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_rx = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static uint32_t s_forwarded;
static uint32_t s_malformed;
static uint32_t s_padding_saved;

/*
 * Modem callback context: do the minimum.
 *
 * The PDC is always a whole transport block (700 B at MCS 4 / 8 subslots),
 * so a packet shorter than that arrives padded with whatever the modem put
 * after it. Forwarding the padding would waste the VCOM1 link -- which at
 * 81 kB/s is the tightest bottleneck in the system -- and would force the PC
 * decoder to resynchronise after every short fragment.
 *
 * So read the length out of the header and forward exactly the real packet.
 * The header is validated first: a corrupted length must never be trusted.
 */
static void on_pdc(const uint8_t *data, size_t len)
{
	if (len < DECTCAM_HDR_LEN || data[0] != DECTCAM_MAGIC0 || data[1] != DECTCAM_MAGIC1 ||
	    (data[DECTCAM_OFF_VERTYPE] >> 4) != DECTCAM_VERSION) {
		s_malformed++;
		return;
	}

	const uint16_t plen = dectcam_get_u16(&data[DECTCAM_OFF_PAYLOAD_LEN]);
	const size_t total = (size_t)plen + DECTCAM_OVERHEAD;

	if (plen > DECTCAM_MAX_PAYLOAD || total > len) {
		s_malformed++;
		return;
	}

	if (uart_out_send(data, total) == 0) {
		s_forwarded++;
		s_padding_saved += (uint32_t)(len - total);
		if ((s_forwarded & 0x0f) == 0) {
			gpio_pin_toggle_dt(&led_rx);
		}
	}
}

static void leds_init(void)
{
	if (gpio_is_ready_dt(&led_up)) {
		gpio_pin_configure_dt(&led_up, GPIO_OUTPUT_INACTIVE);
	}
	if (gpio_is_ready_dt(&led_rx)) {
		gpio_pin_configure_dt(&led_rx, GPIO_OUTPUT_INACTIVE);
	}
}

#if CONFIG_DECTCAM_RX_STATS_PACKETS
static void send_stats_packet(void)
{
	struct radio_stats rs;
	struct uart_out_stats us;

	radio_get_stats(&rs);
	uart_out_get_stats(&us);

	struct dectcam_stats s = {
		.rssi_2 = rs.last_rssi_2,
		.snr = rs.last_snr,
		.pdc_ok = rs.pdc_ok,
		.pdc_crc_err = rs.pdc_crc_err,
		.frags_out = us.queued,
	};
	uint8_t body[DECTCAM_STATS_LEN];
	uint8_t pkt[DECTCAM_STATS_LEN + DECTCAM_OVERHEAD];

	dectcam_stats_pack(body, &s);

	const size_t n = dectcam_encode(pkt, sizeof(pkt), DECTCAM_TYPE_STATS, 0, 0, 0, body,
					sizeof(body));

	if (n) {
		uart_out_send(pkt, n);
	}
}
#endif

int main(void)
{
	LOG_INF("DECT camera receiver starting");

	leds_init();

	if (uart_out_init() != 0) {
		LOG_ERR("uart init failed; halting");
		return -EIO;
	}

	if (radio_init(on_pdc) != 0) {
		LOG_ERR("radio init failed; halting");
		return -EIO;
	}

	if (radio_start_rx() != 0) {
		LOG_ERR("could not start RX; halting");
		return -EIO;
	}
	gpio_pin_set_dt(&led_up, 1);

	LOG_INF("receiving; forwarding to VCOM1");

	int64_t last_stats = k_uptime_get();
	int64_t last_statpkt = k_uptime_get();
	uint32_t last_pdc_ok;
	uint64_t last_bytes;

	{
		struct radio_stats rs;

		radio_get_stats(&rs);
		last_pdc_ok = rs.pdc_ok;
		last_bytes = rs.rx_bytes;
	}

	for (;;) {
		k_msleep(100);

		const int64_t now = k_uptime_get();

#if CONFIG_DECTCAM_RX_STATS_PACKETS
		if (now - last_statpkt >= 1000) {
			send_stats_packet();
			last_statpkt = now;
		}
#endif

#if CONFIG_DECTCAM_STATS_INTERVAL_MS > 0
		if (now - last_stats >= CONFIG_DECTCAM_STATS_INTERVAL_MS) {
			const double secs = (now - last_stats) / 1000.0;
			struct radio_stats rs;
			struct uart_out_stats us;

			radio_get_stats(&rs);
			uart_out_get_stats(&us);

			LOG_INF("rx %.1f pkt/s, %.1f kB/s | rssi %d.%d dBm snr %d.%02d dB | "
				"pdc ok %u crc %u | pcc ok %u crc %u | "
				"uart q %u drop %u | malformed %u | pad saved %u kB | "
				"rx restarts %u",
				(rs.pdc_ok - last_pdc_ok) / secs,
				(rs.rx_bytes - last_bytes) / secs / 1000.0, rs.last_rssi_2 / 2,
				(rs.last_rssi_2 & 1) * 5, rs.last_snr / 4,
				(rs.last_snr % 4) * 25, rs.pdc_ok, rs.pdc_crc_err, rs.pcc_ok,
				rs.pcc_crc_err, us.queued, us.dropped, s_malformed,
				s_padding_saved / 1024, rs.rx_restarts);

			last_pdc_ok = rs.pdc_ok;
			last_bytes = rs.rx_bytes;
			last_stats = now;
		}
#endif
	}

	return 0;
}
