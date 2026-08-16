/*
 * console_cmds.c - runtime control over the ESP32's own UART0 console.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is the "runtime selectable" half of the resolution/quality
 * requirement. Driving it from the PC viewer instead would need a DECT
 * uplink, which is deliberately out of scope for the unidirectional design;
 * see docs/architecture.md.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "console_cmds.h"
#include "cam.h"
#include "link.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "console";
static int s_streaming = 1;

int console_streaming_enabled(void)
{
	return s_streaming;
}

static int cmd_cam(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: cam <res|quality|fps|vflip|hmirror|pause|resume|info> [value]\n");
		printf("  res      one of: %s\n", dcam_framesize_names());
		printf("  quality  4..63 (LOWER is better quality, bigger frames)\n");
		printf("  fps      1..30 target capture rate\n");
		return 1;
	}

	const char *sub = argv[1];

	if (strcmp(sub, "info") == 0) {
		printf("resolution : %ux%u\n", dcam_width(), dcam_height());
		printf("quality    : %u (4=best, 63=worst)\n", dcam_quality());
		printf("target fps : %u\n", dcam_target_fps());
		printf("fragment   : %u bytes payload\n", link_frag_payload());
		printf("streaming  : %s\n", s_streaming ? "yes" : "paused");
		return 0;
	}
	if (strcmp(sub, "pause") == 0) {
		s_streaming = 0;
		printf("streaming paused\n");
		return 0;
	}
	if (strcmp(sub, "resume") == 0) {
		s_streaming = 1;
		printf("streaming resumed\n");
		return 0;
	}

	if (argc < 3) {
		printf("error: '%s' needs a value\n", sub);
		return 1;
	}

	if (strcmp(sub, "res") == 0) {
		esp_err_t e = dcam_set_framesize_str(argv[2]);

		if (e == ESP_ERR_NOT_FOUND) {
			printf("error: unknown size '%s'; try one of: %s\n", argv[2],
			       dcam_framesize_names());
			return 1;
		}
		if (e != ESP_OK) {
			printf("error: sensor rejected the framesize\n");
			return 1;
		}
		printf("resolution now %ux%u\n", dcam_width(), dcam_height());
		link_send_meta(dcam_width(), dcam_height(), dcam_quality(), dcam_target_fps());
		return 0;
	}

	if (strcmp(sub, "quality") == 0) {
		int q = atoi(argv[2]);

		if (dcam_set_quality(q) != ESP_OK) {
			printf("error: quality must be 4..63\n");
			return 1;
		}
		printf("quality now %u\n", dcam_quality());
		link_send_meta(dcam_width(), dcam_height(), dcam_quality(), dcam_target_fps());
		return 0;
	}

	if (strcmp(sub, "fps") == 0) {
		dcam_set_target_fps(atoi(argv[2]));
		printf("target fps now %u\n", dcam_target_fps());
		link_send_meta(dcam_width(), dcam_height(), dcam_quality(), dcam_target_fps());
		return 0;
	}

	if (strcmp(sub, "vflip") == 0) {
		dcam_set_vflip(atoi(argv[2]) != 0);
		return 0;
	}
	if (strcmp(sub, "hmirror") == 0) {
		dcam_set_hmirror(atoi(argv[2]) != 0);
		return 0;
	}

	printf("error: unknown subcommand '%s'\n", sub);
	return 1;
}

static int cmd_stats(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	struct link_stats st;

	link_get_stats(&st);
	printf("frames sent    : %lu\n", (unsigned long)st.frames);
	printf("fragments      : %lu\n", (unsigned long)st.frags);
	printf("bytes          : %llu\n", (unsigned long long)st.bytes);
	printf("stalled frags  : %lu\n", (unsigned long)st.tx_timeouts);
	printf("frames dropped : %lu\n", (unsigned long)st.frames_dropped);
	printf("rx junk bytes  : %lu\n", (unsigned long)st.rx_junk);
	printf("free heap      : %lu\n", (unsigned long)esp_get_free_heap_size());
	return 0;
}

static int cmd_meta(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	link_send_meta(dcam_width(), dcam_height(), dcam_quality(), dcam_target_fps());
	printf("meta packet sent\n");
	return 0;
}

void console_exec_line(const char *line)
{
	/* Tokenise into argv in place; commands are short and space separated. */
	char work[96];
	char *argv[8];
	int argc = 0;

	strncpy(work, line, sizeof(work) - 1);
	work[sizeof(work) - 1] = '\0';

	char *p = work;

	while (*p && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}

	if (argc == 0) {
		return;
	}

	ESP_LOGI(TAG, "back-channel command: %s", line);

	if (strcmp(argv[0], "cam") == 0) {
		cmd_cam(argc, argv);
	} else if (strcmp(argv[0], "stats") == 0) {
		cmd_stats(argc, argv);
	} else if (strcmp(argv[0], "meta") == 0) {
		cmd_meta(argc, argv);
	} else {
		ESP_LOGW(TAG, "unknown back-channel command '%s'", argv[0]);
	}
}

/*
 * Sample the CTS pin (GPIO1), which carries the nRF9151 DK's RTS output
 * from D6, and report whether it is switching, stuck, or absent.
 *
 * Digital, not analogue, and deliberately so. The earlier version of this
 * command drove an ADC on the same pin, which meant:
 *
 *   - it stole the pin from the UART and never gave it back, so running the
 *     command silently disabled flow control until the next reboot. That
 *     produced an hour of chasing a "loose CTS wire" that was never loose;
 *   - one-shot ADC conversions take tens of microseconds each, far too slow
 *     to characterise a signal that toggles at UART rates, so the numbers it
 *     produced were aliasing artefacts.
 *
 * gpio_get_level() reads the pad through the GPIO_IN register regardless of
 * what the pin is routed to, so this observes the live signal without
 * touching the UART's ownership of it.
 *
 * Reading the result:
 *
 *   transitions > 0     the wire is good and the nRF is throttling us
 *                       normally; this is the healthy case under load
 *   always low, 0 trans the nRF is permanently ready (its buffer is empty,
 *                       which is normal when little is flowing), or the pin
 *                       is shorted to ground
 *   always high, 0 tr.  the nRF is not ready, is not powered, or the wire is
 *                       missing and the UART's internal pull-up is winning
 */
/* Sample the pad and report how many of n reads were high, and how many
 * times it changed. Returns the high count; *transitions gets the rest. */
static int sample_pin(int pin, int n, int *transitions)
{
	int highs = 0;
	int trans = 0;
	int prev = gpio_get_level(pin);

	for (int i = 0; i < n; i++) {
		const int lvl = gpio_get_level(pin);

		if (lvl) {
			highs++;
		}
		if (lvl != prev) {
			trans++;
			prev = lvl;
		}
	}
	*transitions = trans;
	return highs;
}

static int cmd_cts(int argc, char **argv)
{
	(void)argc;
	(void)argv;

#if CONFIG_DECTCAM_UART_FLOWCTRL
	const int pin = CONFIG_DECTCAM_UART_CTS_GPIO;
	const bool uart_owns = true;
#else
	const int pin = CONFIG_DECTCAM_UART_CTS_GPIO;
	const bool uart_owns = false;

	/* Not claimed by the UART in this build, so make it a plain input. */
	gpio_config_t cfg = {
		.pin_bit_mask = 1ULL << pin,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	gpio_config(&cfg);
#endif

	const int n = 20000;
	int t_up = 0, t_down = 0;

	/*
	 * The pull is a pad-level setting, independent of whether the pin is
	 * routed to the UART through the GPIO matrix, so this can be changed
	 * and put back without disturbing the link.
	 *
	 * Reading the pad under both pulls is what makes the answer definitive.
	 * A single reading cannot tell "held high by the peer" from "held high
	 * by our own pull-up because nothing is connected" -- which is exactly
	 * the confusion that made an earlier version of this diagnosis wrong.
	 */
	gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
	vTaskDelay(pdMS_TO_TICKS(5));
	const int high_down = sample_pin(pin, n, &t_down);

	gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
	vTaskDelay(pdMS_TO_TICKS(5));
	const int high_up = sample_pin(pin, n, &t_up);

	printf("GPIO%d (nRF D6 / RTS), %d samples per pull:\n", pin, n);
	printf("  with pull-down : high %5.1f %%   transitions %d\n",
	       100.0 * high_down / n, t_down);
	printf("  with pull-up   : high %5.1f %%   transitions %d\n",
	       100.0 * high_up / n, t_up);
	printf("  uart owns pin  : %s\n", uart_owns ? "yes" : "no (flow control off)");
	printf("\n");

	const bool down_low = high_down < n / 10;
	const bool up_high = high_up > (9 * n) / 10;

	if (t_up > 0 || t_down > 0) {
		printf("  VERDICT: driven and switching. The wire is good and the\n");
		printf("  nRF is throttling us, which is flow control working.\n");
	} else if (down_low && up_high) {
		printf("  VERDICT: NOTHING IS DRIVING THIS PIN.\n");
		printf("  It simply follows whichever pull is applied, so the D6\n");
		printf("  wire is not making contact. Check the jumper at both ends\n");
		printf("  and, on a breadboard, that both ends share a row.\n");
	} else if (!down_low) {
		printf("  VERDICT: driven high. The pin beats a pull-down, so the\n");
		printf("  nRF is holding RTS deasserted -- it is not ready for data.\n");
	} else {
		printf("  VERDICT: driven low. The pin beats a pull-up, so the nRF\n");
		printf("  is asserting RTS: the wire is good and it is ready.\n");
	}

	/* ESP-IDF enables a pull-up on CTS in uart_set_pin(); leave it that way. */
	gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
	return 0;
}

void console_start(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

	repl_cfg.prompt = "dectcam>";
	repl_cfg.max_cmdline_length = 128;

	esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

	if (esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl) != ESP_OK) {
		ESP_LOGE(TAG, "failed to create console REPL");
		return;
	}

	const esp_console_cmd_t cmds[] = {
		{ .command = "cam",
		  .help = "camera control: res/quality/fps/vflip/hmirror/pause/resume/info",
		  .hint = NULL,
		  .func = cmd_cam },
		{ .command = "stats", .help = "link statistics", .hint = NULL, .func = cmd_stats },
		{ .command = "meta",
		  .help = "resend the FRAME_META packet",
		  .hint = NULL,
		  .func = cmd_meta },
		{ .command = "cts",
		  .help = "sample the CTS pin: is the nRF's RTS switching, stuck, or absent",
		  .hint = NULL,
		  .func = cmd_cts },
	};

	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
	}
	ESP_ERROR_CHECK(esp_console_register_help_command());
	ESP_ERROR_CHECK(esp_console_start_repl(repl));

	ESP_LOGI(TAG, "console ready; type 'help'");
}
