/*
 * cam.c - camera capture for the Freenove ESP32-S3 WROOM CAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The board ships with either an OV2640 or an OV3660; esp32-camera detects
 * which and nothing here depends on the answer. The unit in use is an OV3660.
 *
 * The pin map below is fixed by the module layout and is not configurable:
 * getting it wrong just means "no camera", and every one of these pins is
 * hard-wired on the PCB.
 */

#include <string.h>
#include <strings.h>

#include "cam.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "cam";

/* ---- Freenove ESP32-S3 WROOM CAM pin map (fixed by the module) ---------
 *
 * Taken from Freenove's own examples, which select CAMERA_MODEL_ESP32S3_EYE
 * (see Freenove_ESP32_S3_WROOM_Board-main/C/Sketches/Sketch_07.1_CameraWebServer/
 * camera_pins.h). The board is pin-identical to Espressif's ESP32-S3-EYE for
 * the camera interface.
 *
 * pin_dN below is data bit N, which the OV2640 datasheet calls Y(N+2).
 */
#define PIN_PWDN  -1 /* not connected */
#define PIN_RESET -1 /* not connected */
#define PIN_XCLK  15
#define PIN_SIOD   4
#define PIN_SIOC   5
#define PIN_D7    16 /* Y9 */
#define PIN_D6    17 /* Y8 */
#define PIN_D5    18 /* Y7 */
#define PIN_D4    12 /* Y6 */
#define PIN_D3    10 /* Y5 */
#define PIN_D2     8 /* Y4 */
#define PIN_D1     9 /* Y3 */
#define PIN_D0    11 /* Y2 */
#define PIN_VSYNC  6
#define PIN_HREF   7
#define PIN_PCLK  13

static uint8_t s_quality = CONFIG_DECTCAM_JPEG_QUALITY;
static uint8_t s_target_fps = CONFIG_DECTCAM_TARGET_FPS;
static uint16_t s_width, s_height;

/*
 * Orientation is held here rather than only in Kconfig because the sensor is
 * re-initialised on every resolution change, which resets it.
 *
 * Kconfig bools are undefined rather than 0 when disabled, hence the #ifdef.
 */
#ifdef CONFIG_DECTCAM_VFLIP
static uint8_t s_vflip = 1;
#else
static uint8_t s_vflip;
#endif
#ifdef CONFIG_DECTCAM_HMIRROR
static uint8_t s_hmirror = 1;
#else
static uint8_t s_hmirror;
#endif

static const struct {
	const char *name;
	framesize_t fs;
	uint16_t w, h;
} s_sizes[] = {
	{ "qqvga", FRAMESIZE_QQVGA, 160, 120 },
	{ "qcif",  FRAMESIZE_QCIF,  176, 144 },
	{ "hqvga", FRAMESIZE_HQVGA, 240, 176 },
	{ "qvga",  FRAMESIZE_QVGA,  320, 240 },
	{ "cif",   FRAMESIZE_CIF,   400, 296 },
	{ "hvga",  FRAMESIZE_HVGA,  480, 320 },
	{ "vga",   FRAMESIZE_VGA,   640, 480 },
	{ "svga",  FRAMESIZE_SVGA,  800, 600 },
};

static framesize_t initial_framesize(void)
{
#if CONFIG_DECTCAM_FRAMESIZE_QQVGA
	return FRAMESIZE_QQVGA;
#elif CONFIG_DECTCAM_FRAMESIZE_CIF
	return FRAMESIZE_CIF;
#elif CONFIG_DECTCAM_FRAMESIZE_VGA
	return FRAMESIZE_VGA;
#else
	return FRAMESIZE_QVGA;
#endif
}

static void note_size(framesize_t fs)
{
	for (size_t i = 0; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
		if (s_sizes[i].fs == fs) {
			s_width = s_sizes[i].w;
			s_height = s_sizes[i].h;
			return;
		}
	}
	s_width = s_height = 0;
}

/*
 * The driver configuration is kept so the sensor can be brought up again with
 * a different frame size.
 *
 * esp32-camera sizes its JPEG frame buffers at init time from frame_size.
 * Calling the sensor's own set_framesize() at runtime changes the picture but
 * not those buffers, so switching to a larger resolution overflows them: the
 * log shows "FB-OVF", then "NO-EOI - JPEG end marker missing", and captures
 * stop altogether with "Failed to get frame: timeout". Re-initialising is the
 * supported way to change resolution.
 */
static camera_config_t s_cfg;

/* Start the driver from s_cfg and re-apply everything held on the sensor. */
static esp_err_t bring_up(void)
{
	esp_err_t err = esp_camera_init(&s_cfg);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
		ESP_LOGE(TAG, "check the ribbon cable seating and that the board is powered "
			      "from its own USB-C socket");
		return err;
	}

	sensor_t *s = esp_camera_sensor_get();

	if (s == NULL) {
		ESP_LOGE(TAG, "no sensor handle after init");
		return ESP_FAIL;
	}

	s->set_quality(s, s_quality);
	s->set_vflip(s, s_vflip);
	s->set_hmirror(s, s_hmirror);

	return ESP_OK;
}

esp_err_t dcam_init(void)
{
	const framesize_t fs = initial_framesize();

	s_cfg = (camera_config_t){
		.pin_pwdn = PIN_PWDN,
		.pin_reset = PIN_RESET,
		.pin_xclk = PIN_XCLK,
		.pin_sccb_sda = PIN_SIOD,
		.pin_sccb_scl = PIN_SIOC,
		.pin_d7 = PIN_D7,
		.pin_d6 = PIN_D6,
		.pin_d5 = PIN_D5,
		.pin_d4 = PIN_D4,
		.pin_d3 = PIN_D3,
		.pin_d2 = PIN_D2,
		.pin_d1 = PIN_D1,
		.pin_d0 = PIN_D0,
		.pin_vsync = PIN_VSYNC,
		.pin_href = PIN_HREF,
		.pin_pclk = PIN_PCLK,

		.xclk_freq_hz = CONFIG_DECTCAM_XCLK_HZ,
		.ledc_timer = LEDC_TIMER_0,
		.ledc_channel = LEDC_CHANNEL_0,

		.pixel_format = PIXFORMAT_JPEG,
		.frame_size = fs,
		.jpeg_quality = CONFIG_DECTCAM_JPEG_QUALITY,

/*
 * Frame buffering.
 *
 * With 8 MB of octal PSRAM there is no reason to economise: two buffers let
 * the sensor fill one frame while the previous one is still being fragmented
 * onto the UART, so capture and transmit overlap instead of alternating.
 *
 * The DRAM fallback exists only so the firmware still builds and runs if PSRAM
 * is ever turned off; a single buffer serialises capture and transmit and
 * costs frame rate.
 *
 * GRAB_LATEST keeps latency low: when the link is the slow part, a fresh frame
 * always beats a queued one.
 */
#if CONFIG_SPIRAM
		/*
		 * Two buffers, so the sensor can fill one frame while the
		 * previous one is still being fragmented onto the UART.
		 *
		 * This was briefly reduced to one while chasing a stall that
		 * delivered a single frame and then stopped. The cause turned
		 * out to be the CTS input, not buffering (see
		 * CONFIG_DECTCAM_UART_CTS_INVERT), and with one buffer capture
		 * and transmit serialise: ~30 ms of exposure plus ~28 ms of
		 * UART puts the ceiling near 10 fps.
		 */
		.fb_count = 2,
		.fb_location = CAMERA_FB_IN_PSRAM,
#else
		.fb_count = 1,
		.fb_location = CAMERA_FB_IN_DRAM,
#endif
		/*
		 * GRAB_WHEN_EMPTY, not GRAB_LATEST.
		 *
		 * GRAB_LATEST is the tempting choice for live video -- it drops
		 * stale frames rather than queueing them -- but on this board
		 * it delivers exactly one frame and then esp_camera_fb_get()
		 * blocks forever. Freenove's own example for this module uses
		 * GRAB_WHEN_EMPTY, and with it the stream runs.
		 *
		 * The latency cost is bounded: with two buffers at most one
		 * frame can be waiting, and the capture loop drains them as
		 * fast as the link allows.
		 */
		.grab_mode = CAMERA_GRAB_WHEN_EMPTY,
	};

	esp_err_t err = bring_up();

	if (err != ESP_OK) {
		return err;
	}

	sensor_t *s = esp_camera_sensor_get();

	ESP_LOGI(TAG, "sensor PID 0x%04x", s->id.PID);

	note_size(fs);
	ESP_LOGI(TAG, "camera up: %ux%u JPEG q=%u xclk=%d Hz", s_width, s_height, s_quality,
		 CONFIG_DECTCAM_XCLK_HZ);
	return ESP_OK;
}

esp_err_t dcam_grab(struct dcam_frame *out)
{
	camera_fb_t *fb = esp_camera_fb_get();

	if (fb == NULL) {
		return ESP_FAIL;
	}
	if (fb->format != PIXFORMAT_JPEG) {
		/* Should be impossible given the init config, but never ship
		 * a non-JPEG buffer to something that will treat it as JPEG. */
		ESP_LOGE(TAG, "unexpected pixel format %d", fb->format);
		esp_camera_fb_return(fb);
		return ESP_ERR_INVALID_STATE;
	}

	out->buf = fb->buf;
	out->len = fb->len;
	out->width = fb->width;
	out->height = fb->height;
	out->handle = fb;

	/*
	 * esp32-camera stamps each buffer as the sensor completes it, on the
	 * same clock as esp_timer_get_time(), so the two are directly
	 * comparable.
	 */
	out->capture_us = (int64_t)fb->timestamp.tv_sec * 1000000
			  + fb->timestamp.tv_usec;
	return ESP_OK;
}

void dcam_release(struct dcam_frame *f)
{
	if (f != NULL && f->handle != NULL) {
		esp_camera_fb_return((camera_fb_t *)f->handle);
		f->handle = NULL;
		f->buf = NULL;
		f->len = 0;
	}
}

esp_err_t dcam_set_framesize_str(const char *name)
{
	for (size_t i = 0; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
		if (strcasecmp(name, s_sizes[i].name) != 0) {
			continue;
		}

		if (s_cfg.frame_size == s_sizes[i].fs) {
			return ESP_OK;
		}

		/*
		 * Full re-initialisation rather than the sensor's own
		 * set_framesize(): the driver's JPEG frame buffers are sized
		 * from frame_size when the driver starts, and leaving them at
		 * the old size makes a larger picture overflow them. See the
		 * comment on s_cfg.
		 */
		const framesize_t previous = s_cfg.frame_size;

		esp_camera_deinit();
		s_cfg.frame_size = s_sizes[i].fs;

		esp_err_t err = bring_up();

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "could not switch to %s; restoring the previous size",
				 s_sizes[i].name);
			s_cfg.frame_size = previous;
			if (bring_up() != ESP_OK) {
				ESP_LOGE(TAG, "the camera did not come back up");
			}
			return err;
		}

		note_size(s_sizes[i].fs);
		/*
		 * The sensor needs a few frames to settle after a resolution
		 * change; the first one or two are often torn or wrongly
		 * exposed.
		 */
		vTaskDelay(pdMS_TO_TICKS(150));
		ESP_LOGI(TAG, "framesize -> %s (%ux%u)", s_sizes[i].name, s_width, s_height);
		return ESP_OK;
	}
	return ESP_ERR_NOT_FOUND;
}

esp_err_t dcam_set_quality(int q)
{
	sensor_t *s = esp_camera_sensor_get();

	if (s == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (q < 4 || q > 63) {
		return ESP_ERR_INVALID_ARG;
	}
	if (s->set_quality(s, q) != 0) {
		return ESP_FAIL;
	}
	s_quality = (uint8_t)q;
	ESP_LOGI(TAG, "jpeg quality -> %d", q);
	return ESP_OK;
}

void dcam_set_target_fps(int fps)
{
	if (fps < 1) {
		fps = 1;
	}
	if (fps > 30) {
		fps = 30;
	}
	s_target_fps = (uint8_t)fps;
	ESP_LOGI(TAG, "target fps -> %d", fps);
}

esp_err_t dcam_set_vflip(bool on)
{
	sensor_t *s = esp_camera_sensor_get();

	if (s == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (s->set_vflip(s, on ? 1 : 0) != 0) {
		return ESP_FAIL;
	}
	/* Remembered so a later resolution change, which re-initialises the
	 * sensor, does not silently undo it. */
	s_vflip = on ? 1 : 0;
	return ESP_OK;
}

esp_err_t dcam_set_hmirror(bool on)
{
	sensor_t *s = esp_camera_sensor_get();

	if (s == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	if (s->set_hmirror(s, on ? 1 : 0) != 0) {
		return ESP_FAIL;
	}
	s_hmirror = on ? 1 : 0;
	return ESP_OK;
}

uint16_t dcam_width(void)
{
	return s_width;
}
uint16_t dcam_height(void)
{
	return s_height;
}
uint8_t dcam_quality(void)
{
	return s_quality;
}
uint8_t dcam_target_fps(void)
{
	return s_target_fps;
}

const char *dcam_framesize_names(void)
{
	return "qqvga qcif hqvga qvga cif hvga vga svga";
}
