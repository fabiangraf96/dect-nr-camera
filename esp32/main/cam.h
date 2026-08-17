/*
 * cam.h - camera capture for the Freenove ESP32-S3 WROOM CAM.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_DCAM_H
#define DECTCAM_DCAM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct dcam_frame {
	const uint8_t *buf;
	size_t len;
	uint16_t width;
	uint16_t height;
	void *handle; /* opaque; pass back to dcam_release() */

	/*
	 * When the sensor finished this frame, on esp_timer_get_time()'s clock.
	 *
	 * Compared against the time the last fragment leaves the UART, this
	 * gives the delay the transmitter is responsible for: exposure, JPEG
	 * encoding, any wait in the driver's buffer queue, and fragmenting
	 * onto the wire. It is the only part of the end to end delay that can
	 * be measured without a common clock at the two ends.
	 */
	int64_t capture_us;
};

/* Bring up the camera with the Kconfig defaults. */
esp_err_t dcam_init(void);

/*
 * Grab the next JPEG frame. Blocks until one is available.
 * The caller MUST call dcam_release() for every successful grab, otherwise
 * the driver runs out of frame buffers within a few frames.
 */
esp_err_t dcam_grab(struct dcam_frame *out);
void dcam_release(struct dcam_frame *f);

/* Runtime controls; all are safe to call while streaming. */
esp_err_t dcam_set_framesize_str(const char *name);
esp_err_t dcam_set_quality(int q);
void dcam_set_target_fps(int fps);
esp_err_t dcam_set_vflip(bool on);
esp_err_t dcam_set_hmirror(bool on);

uint16_t dcam_width(void);
uint16_t dcam_height(void);
uint8_t dcam_quality(void);
uint8_t dcam_target_fps(void);

/* Human-readable list of accepted framesize names, for the console help. */
const char *dcam_framesize_names(void);

#ifdef __cplusplus
}
#endif

#endif /* DECTCAM_DCAM_H */
