/*
 * link.c - framed UART link to the nRF9151 transmitter.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encodes JPEG frames as dectcam packets (see dectcam_proto.h) and pushes
 * them out of UART2. One packet is sized to fit exactly one DECT PDC, so the
 * nRF9151 never has to re-fragment: it reads a packet and transmits it.
 */

#include <string.h>

#include "link.h"
#include "dectcam_proto.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "link";

#if CONFIG_DECTCAM_BENCH_UART0
/*
 * Bench mode: send the stream out of UART0 (the USB console port) so the whole
 * camera -> JPEG -> fragment -> protocol path can be validated with no jumper
 * wires at all. The console REPL and logging are disabled in this mode,
 * otherwise log text would be interleaved with the binary stream.
 */
#define UART_PORT (UART_NUM_0)
#define UART_BAUD (CONFIG_DECTCAM_BENCH_UART0_BAUD)
#define UART_USE_FLOWCTRL 0
#else
#define UART_PORT (CONFIG_DECTCAM_UART_NUM)
#define UART_BAUD (CONFIG_DECTCAM_UART_BAUD)
#define UART_USE_FLOWCTRL CONFIG_DECTCAM_UART_FLOWCTRL
#endif

#define FRAG_PAYLOAD (CONFIG_DECTCAM_FRAG_PAYLOAD)

/*
 * How long to wait for room in the TX ring before giving up on a fragment.
 *
 * This must be bounded. With CTS flow control enabled and the nRF9151 absent
 * or unpowered, CTS floats deasserted and a blocking write never returns --
 * the capture loop stops dead with no diagnostic. Dropping is also simply the
 * right policy for live video: a frame delayed behind a stalled link is worth
 * less than the next fresh one.
 */
#define TX_SPACE_TIMEOUT_MS 50

/*
 * The driver's ring buffer. Sized to a few fragments so a burst does not
 * block the capture task, but small enough that we never build a deep queue
 * of stale video: latency matters more than smoothness here.
 */
#define TX_RING_SZ (4 * (FRAG_PAYLOAD + DECTCAM_OVERHEAD))

/* Refuse to build a configuration whose fragments cannot fit a DECT PDC. */
_Static_assert(FRAG_PAYLOAD + DECTCAM_OVERHEAD <= 700,
	       "fragment does not fit the largest DECT transport block (700 B "
	       "at MCS 4 / 8 subslots); reduce CONFIG_DECTCAM_FRAG_PAYLOAD");
_Static_assert(FRAG_PAYLOAD <= DECTCAM_MAX_PAYLOAD,
	       "CONFIG_DECTCAM_FRAG_PAYLOAD exceeds DECTCAM_MAX_PAYLOAD");

static uint8_t s_pkt[FRAG_PAYLOAD + DECTCAM_OVERHEAD];
static struct link_stats s_stats;

esp_err_t link_init(void)
{
	uart_config_t cfg = {
		.baud_rate = UART_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
#if UART_USE_FLOWCTRL
		/*
		 * Both directions. CTS is the one that matters for video: it
		 * lets the nRF stop us before its receive buffer overflows.
		 * RTS only ever throttles the tiny back-channel, but it has to
		 * be wired regardless -- the nRF9151 UARTE has a single
		 * hardware-flow-control bit covering RTS and CTS together and
		 * offers no RTS-only mode.
		 */
		.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS,
		/*
		 * Assert RTS once the 128-byte hardware FIFO is nearly full.
		 * 122 leaves room for the bytes already in flight when the
		 * sender sees the signal.
		 */
		.rx_flow_ctrl_thresh = 122,
#else
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#endif
		.source_clk = UART_SCLK_APB,
	};

	ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

#if !CONFIG_DECTCAM_BENCH_UART0
	ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
				     CONFIG_DECTCAM_UART_TX_GPIO,
				     CONFIG_DECTCAM_UART_RX_GPIO,
#if CONFIG_DECTCAM_UART_FLOWCTRL
				     CONFIG_DECTCAM_UART_RTS_GPIO,
				     CONFIG_DECTCAM_UART_CTS_GPIO
#else
				     UART_PIN_NO_CHANGE,
				     UART_PIN_NO_CHANGE
#endif
				     ));
#endif

#if CONFIG_DECTCAM_UART_FLOWCTRL && CONFIG_DECTCAM_UART_CTS_INVERT
	/*
	 * Do not enable this. See the Kconfig help: it does not fix a polarity
	 * problem, it defeats flow control by turning a floating pull-up into a
	 * permanent "clear to send", and in doing so hides a CTS wire that is
	 * not making contact.
	 */
	ESP_ERROR_CHECK(uart_set_line_inverse(UART_PORT, UART_SIGNAL_CTS_INV));
#endif

	/* TX-buffered so uart_write_bytes() returns as soon as it is queued. */
	if (!uart_is_driver_installed(UART_PORT)) {
		ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, TX_RING_SZ, 0, NULL, 0));
	}

#if CONFIG_DECTCAM_BENCH_UART0
	ESP_LOGW(TAG, "BENCH MODE: stream on uart0 @%d baud, console/log disabled",
		 UART_BAUD);
#else
#if CONFIG_DECTCAM_UART_FLOWCTRL
	const int cts_gpio = CONFIG_DECTCAM_UART_CTS_GPIO;
	const int rts_gpio = CONFIG_DECTCAM_UART_RTS_GPIO;
#else
	const int cts_gpio = -1;
	const int rts_gpio = -1;
#endif
	ESP_LOGI(TAG, "uart%d up: tx=gpio%d rx=gpio%d rts=%d cts=%d %d baud, %d B fragments",
		 UART_PORT, CONFIG_DECTCAM_UART_TX_GPIO, CONFIG_DECTCAM_UART_RX_GPIO, rts_gpio,
		 cts_gpio, UART_BAUD, FRAG_PAYLOAD);
#endif

	return ESP_OK;
}

/*
 * Write one packet, giving up after a bounded wait.
 * Returns the byte count, or -1 if the link is stalled.
 *
 * uart_tx_chars() rather than uart_write_bytes(): the latter blocks until the
 * whole buffer has been queued, and checking the ring's free space beforehand
 * is not enough of a guard. During bring-up, with the nRF9151 not yet wired,
 * CTS floated deasserted, the hardware never drained the ring, and the capture
 * task hung inside uart_write_bytes() after the first frame -- with the
 * timeout counter still reading zero, so the symptom looked like a camera
 * fault rather than a link one.
 *
 * uart_tx_chars() queues only what currently fits and returns immediately,
 * which makes the deadline below actually apply.
 */
static int write_pkt(const uint8_t *buf, size_t len)
{
	size_t off = 0;
	int waited_ms = 0;

	while (off < len) {
		const int n = uart_tx_chars(UART_PORT, (const char *)buf + off, len - off);

		if (n < 0) {
			return -1;
		}
		off += (size_t)n;

		if (off == len) {
			break;
		}
		if (waited_ms >= TX_SPACE_TIMEOUT_MS) {
			/*
			 * A partial packet is worse than none: the receiver
			 * would have to resynchronise on the next magic. The
			 * caller abandons the frame, so the bytes already
			 * queued are simply the tail of a frame that will be
			 * dropped at the far end.
			 */
			return -1;
		}
		vTaskDelay(pdMS_TO_TICKS(2));
		waited_ms += 2;
	}

	s_stats.bytes += (uint64_t)len;
	return (int)len;
}

int link_send_frame(uint16_t frame_id, const uint8_t *jpeg, size_t len)
{
	if (jpeg == NULL || len == 0) {
		return -1;
	}

	const uint16_t frag_payload = FRAG_PAYLOAD;
	const size_t nfrag_sz = (len + frag_payload - 1) / frag_payload;

	if (nfrag_sz == 0 || nfrag_sz > UINT16_MAX) {
		ESP_LOGW(TAG, "frame of %u bytes needs %u fragments; dropping", (unsigned)len,
			 (unsigned)nfrag_sz);
		return -1;
	}
	const uint16_t nfrag = (uint16_t)nfrag_sz;

	size_t off = 0;

	for (uint16_t i = 0; i < nfrag; i++) {
		const uint16_t chunk =
			(uint16_t)((len - off) > frag_payload ? frag_payload : (len - off));

		const size_t plen = dectcam_encode(s_pkt, sizeof(s_pkt), DECTCAM_TYPE_JPEG_FRAG,
						   frame_id, i, nfrag, &jpeg[off], chunk);

		if (plen == 0) {
			ESP_LOGE(TAG, "encode failed (chunk %u)", chunk);
			return -1;
		}
		if (write_pkt(s_pkt, plen) < 0) {
			/*
			 * Abandon the rest of the frame: a partial frame is
			 * useless to the receiver, which needs every fragment.
			 * Warn sparsely so a long stall does not flood the log.
			 */
			s_stats.tx_timeouts++;
			s_stats.frames_dropped++;
			if ((s_stats.tx_timeouts % 32) == 1) {
				ESP_LOGW(TAG,
					 "link stalled at fragment %u/%u (%lu drops); "
					 "is the nRF9151 powered and CTS wired?",
					 i, nfrag, (unsigned long)s_stats.frames_dropped);
			}
			return -1;
		}
		off += chunk;
	}

	s_stats.frames++;
	s_stats.frags += nfrag;
	return nfrag;
}

esp_err_t link_send_meta(uint16_t width, uint16_t height, uint8_t quality, uint8_t fps)
{
	struct dectcam_meta m = {
		.width = width,
		.height = height,
		.jpeg_quality = quality,
		.target_fps = fps,
		.max_payload = FRAG_PAYLOAD,
	};
	uint8_t body[DECTCAM_META_LEN];

	dectcam_meta_pack(body, &m);

	const size_t plen = dectcam_encode(s_pkt, sizeof(s_pkt), DECTCAM_TYPE_FRAME_META, 0, 0, 0,
					   body, sizeof(body));

	if (plen == 0) {
		return ESP_FAIL;
	}
	return write_pkt(s_pkt, plen) < 0 ? ESP_FAIL : ESP_OK;
}

int link_poll_command(char *buf, size_t cap)
{
	static char line[96];
	static size_t len;

	uint8_t c;

	/*
	 * Drain whatever is waiting, one byte at a time with a zero timeout.
	 * Commands are rare and tiny, so this costs nothing, and it keeps the
	 * caller free of a second buffer.
	 */
	while (uart_read_bytes(UART_PORT, &c, 1, 0) == 1) {
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (len == 0) {
				continue;
			}
			line[len] = '\0';
			const size_t n = len < cap - 1 ? len : cap - 1;

			memcpy(buf, line, n);
			buf[n] = '\0';
			len = 0;
			return (int)n;
		}
		/*
		 * Drop anything that is not printable ASCII.
		 *
		 * When the nRF9151 first configures its TX pin the line moves
		 * from floating to driven, and the receiver latches that edge
		 * as a spurious byte. It arrives once, at start-up, ahead of
		 * the first real command -- which was enough to turn
		 * "cam quality 6" into the unknown command "\xef cam" and have
		 * it rejected, while the transmitter reported it as sent.
		 */
		if (c < 0x20 || c > 0x7E) {
			s_stats.rx_junk++;
			continue;
		}
		if (len < sizeof(line) - 1) {
			line[len++] = (char)c;
		} else {
			/* Overlong garbage: drop it rather than truncating into
			 * a command that was never sent. */
			len = 0;
		}
	}
	return 0;
}

void link_get_stats(struct link_stats *out)
{
	if (out != NULL) {
		*out = s_stats;
	}
}

uint16_t link_frag_payload(void)
{
	return FRAG_PAYLOAD;
}
