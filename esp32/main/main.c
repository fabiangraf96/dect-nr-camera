/*
 * main.c - DECT NR+ camera transmitter, camera side.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runs on a Freenove ESP32-S3 WROOM CAM. Captures JPEG frames from the sensor
 * and streams them as dectcam packets out of UART1 to the nRF9151 transmitter.
 *
 * See docs/architecture.md for the whole picture and
 * docs/wiring.md for how this board connects to the DK.
 */

#include <stdio.h>
#include <string.h>

#include "cam.h"
#include "link.h"
#include "console_cmds.h"
#include "dectcam_proto.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#if CONFIG_DECTCAM_TEST_PATTERN
/*
 * A tiny valid JPEG (solid grey 8x8) used to exercise the link before the
 * camera is wired. A frame counter is drawn into the comment segment so the
 * viewer can prove frames are advancing.
 */
static const uint8_t k_test_jpeg[] = {
	0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00,
	0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x28, 0x1C, 0x1E,
	0x23, 0x1E, 0x19, 0x28, 0x23, 0x21, 0x23, 0x2D, 0x2B, 0x28, 0x30, 0x3C, 0x64, 0x41,
	0x3C, 0x37, 0x37, 0x3C, 0x7B, 0x58, 0x5D, 0x49, 0x64, 0x91, 0x80, 0x99, 0x96, 0x8F,
	0x80, 0x8C, 0x8A, 0xA0, 0xB4, 0xE6, 0xC3, 0xA0, 0xAA, 0xDA, 0xAD, 0x8A, 0x8C, 0xC8,
	0xFF, 0xCB, 0xDA, 0xEE, 0xF5, 0xFF, 0xFF, 0xFF, 0x9B, 0xC1, 0xFF, 0xFF, 0xFF, 0xFA,
	0xFF, 0xE6, 0xFD, 0xFF, 0xF8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08,
	0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
	0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10,
	0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00,
	0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
	0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42,
	0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
	0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
	0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73,
	0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
	0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
	0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6,
	0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
	0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08,
	0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0xFB, 0xFE, 0x8A, 0x28, 0xA0, 0x0F, 0xFF, 0xD9
};
#endif

static void stream_task(void *arg)
{
	(void)arg;

	uint16_t frame_id = 0;
	int64_t last_meta_us = 0;
	int64_t last_stats_us = 0;
	uint32_t frames_since_stats = 0;
	uint64_t bytes_since_stats = 0;

	for (;;) {
		const int64_t t_start = esp_timer_get_time();

		/*
		 * Back-channel from the nRF9151. Polled here rather than in a
		 * task of its own: commands arrive at human speed, and this
		 * keeps them serialised with capture so a resolution change
		 * can never land in the middle of a frame.
		 */
		{
			char cmd[96];

			if (link_poll_command(cmd, sizeof(cmd)) > 0) {
				console_exec_line(cmd);
			}
		}

		if (!console_streaming_enabled()) {
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}

		/* Periodic metadata so a late-joining viewer learns the format. */
		if (t_start - last_meta_us >= (int64_t)CONFIG_DECTCAM_METAINTERVAL_MS * 1000) {
			link_send_meta(dcam_width(), dcam_height(), dcam_quality(),
				       dcam_target_fps());
			last_meta_us = t_start;
		}

#if CONFIG_DECTCAM_TEST_PATTERN
		const uint8_t *jpeg = k_test_jpeg;
		const size_t jlen = sizeof(k_test_jpeg);
#else
		struct dcam_frame f;

		if (dcam_grab(&f) != ESP_OK) {
			ESP_LOGW(TAG, "frame grab failed");
			vTaskDelay(pdMS_TO_TICKS(100));
			continue;
		}
		const uint8_t *jpeg = f.buf;
		const size_t jlen = f.len;
#endif

		const int nfrag = link_send_frame(frame_id, jpeg, jlen);

		if (nfrag > 0) {
			frames_since_stats++;
			bytes_since_stats += jlen;
			frame_id++;
		}

#if !CONFIG_DECTCAM_TEST_PATTERN
		/* Return the buffer promptly; the driver only has two. */
		dcam_release(&f);
#endif

#if CONFIG_DECTCAM_STATS_INTERVAL_MS > 0
		const int64_t now = esp_timer_get_time();

		if (now - last_stats_us >= (int64_t)CONFIG_DECTCAM_STATS_INTERVAL_MS * 1000) {
			const double secs = (double)(now - last_stats_us) / 1e6;

			ESP_LOGI(TAG, "%.1f fps, %.1f kB/s, %ux%u q=%u, heap %lu",
				 frames_since_stats / secs, bytes_since_stats / secs / 1000.0,
				 dcam_width(), dcam_height(), dcam_quality(),
				 (unsigned long)esp_get_free_heap_size());
			frames_since_stats = 0;
			bytes_since_stats = 0;
			last_stats_us = now;
		}
#endif

		/*
		 * Pace to the target frame rate. If the link was the slow part
		 * this evaluates to no delay at all and we simply run as fast
		 * as the UART drains.
		 */
		const int64_t budget_us = 1000000 / (dcam_target_fps() ? dcam_target_fps() : 1);
		const int64_t spent_us = esp_timer_get_time() - t_start;

		if (spent_us < budget_us) {
			const int64_t sleep_ms = (budget_us - spent_us) / 1000;

			if (sleep_ms > 0) {
				vTaskDelay(pdMS_TO_TICKS(sleep_ms));
			} else {
				taskYIELD();
			}
		} else {
			taskYIELD();
		}
	}
}

void app_main(void)
{
#if CONFIG_DECTCAM_BENCH_UART0
	/*
	 * Bench mode shares UART0 with the console. Silence every log source
	 * before the link comes up, otherwise text is interleaved into the
	 * binary packet stream and the decoder spends its life resynchronising.
	 */
	esp_log_level_set("*", ESP_LOG_NONE);
#endif

	ESP_LOGI(TAG, "DECT NR+ camera transmitter starting");

	esp_err_t err = nvs_flash_init();

	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	ESP_ERROR_CHECK(link_init());

#if CONFIG_DECTCAM_TEST_PATTERN
	ESP_LOGW(TAG, "TEST PATTERN mode: the camera is not used");
#else
	if (dcam_init() != ESP_OK) {
		ESP_LOGE(TAG, "camera init failed; not starting the stream");
#if !CONFIG_DECTCAM_BENCH_UART0
		console_start();
#endif
		return;
	}
#endif

	link_send_meta(dcam_width(), dcam_height(), dcam_quality(), dcam_target_fps());

	/*
	 * Priority 5: above the idle/console tasks so capture is not starved
	 * while someone is typing, but below the UART driver's own ISR work.
	 */
	xTaskCreate(stream_task, "stream", 4096, NULL, 5, NULL);

#if !CONFIG_DECTCAM_BENCH_UART0
	console_start();
#endif
}
