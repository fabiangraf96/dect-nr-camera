/*
 * camctl.c - shell command that forwards text to the camera board.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The uart link to the camera is full duplex: the camera sends video packets
 * on its TX line, and its RX line (our P0.03 -> ESP32-S3 GPIO21) would
 * otherwise be idle. This puts it to work as a control channel, so camera
 * parameters can be changed from the transmitter's own console.
 *
 * The camera board has a console of its own, so this is a convenience rather
 * than a necessity -- but a real one: it puts the camera controls on the same
 * terminal as the radio statistics, and opening the camera's own port resets
 * the board, which throws away whatever was set at runtime.
 *
 * No reply comes back: the return direction carries the video stream and must
 * not be polluted with text. Confirmation arrives instead as a FRAME_META
 * packet, which the PC viewer displays.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "radio.h"
#include "uart_in.h"
#include "camctl.h"

LOG_MODULE_DECLARE(dectcam_tx, CONFIG_DECTCAM_LOG_LEVEL);

/*
 * The camera's resolution table, in the order the wire protocol indexes it.
 * Must match s_sizes[] in the camera firmware and RESOLUTIONS in the viewer;
 * the protocol self-tests pin the order down on both of those.
 */
static const char *const res_names[] = {
	"qqvga", "qcif", "hqvga", "qvga", "cif", "hvga", "vga", "svga",
};

const char *camctl_res_name(uint8_t index)
{
	if (index >= ARRAY_SIZE(res_names)) {
		return NULL;
	}
	return res_names[index];
}

/*
 * Put one line on the wire.
 *
 * Delegated to uart_in, which owns the device and its callback. This used to
 * use uart_poll_out() directly, on the reasoning that a dozen bytes at 1 Mbaud
 * is too short to be worth DMA. That was wrong for a subtler reason: the
 * receive side holds the UART open with the async API, and poll_out alongside
 * it loses characters. "cam fps 15" reached the camera as "cam fps 1".
 */
static int write_line(const char *line)
{
	return uart_in_send_line(line);
}

static int send_line(const struct shell *sh, const char *line)
{
	int err = write_line(line);

	if (err == -ENODEV) {
		shell_error(sh, "link uart not ready");
	}
	return err;
}

int camctl_send_res(uint8_t index)
{
	const char *name = camctl_res_name(index);
	char line[32];

	if (name == NULL) {
		return -EINVAL;
	}
	snprintk(line, sizeof(line), "cam res %s", name);
	LOG_INF("camera: %s", line);
	return write_line(line);
}

int camctl_send_fps(uint8_t fps)
{
	char line[32];

	if (fps < 1 || fps > 30) {
		return -EINVAL;
	}
	snprintk(line, sizeof(line), "cam fps %u", fps);
	LOG_INF("camera: %s", line);
	return write_line(line);
}

int camctl_send_quality(uint8_t q)
{
	char line[32];

	if (q < 4 || q > 63) {
		return -EINVAL;
	}
	snprintk(line, sizeof(line), "cam quality %u", q);
	LOG_INF("camera: %s", line);
	return write_line(line);
}

static int cmd_cam(const struct shell *sh, size_t argc, char **argv)
{
	char line[96];
	size_t off = 0;

	if (argc < 2) {
		shell_print(sh, "usage: cam <res|quality|fps|vflip|hmirror|pause|resume|info> "
				"[value]");
		shell_print(sh, "  res      qqvga qcif hqvga qvga cif hvga vga svga");
		shell_print(sh, "  quality  4..63 (LOWER is better quality, bigger frames)");
		shell_print(sh, "  fps      1..30");
		shell_print(sh, "");
		shell_print(sh, "The camera does not reply on this wire; watch the viewer's");
		shell_print(sh, "FRAME_META readout to confirm the change took effect.");
		return -EINVAL;
	}

	/* Rebuild "cam <args...>" for the far end. */
	off = snprintk(line, sizeof(line), "cam");
	for (size_t i = 1; i < argc; i++) {
		int n = snprintk(&line[off], sizeof(line) - off, " %s", argv[i]);

		if (n < 0 || (size_t)n >= sizeof(line) - off) {
			shell_error(sh, "command too long");
			return -EINVAL;
		}
		off += n;
	}

	if (send_line(sh, line) != 0) {
		return -EIO;
	}
	shell_print(sh, "sent to camera: %s", line);
	return 0;
}

static int cmd_camraw(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "usage: camraw <text>   (sent verbatim, newline appended)");
		return -EINVAL;
	}

	char line[96];
	size_t off = 0;

	for (size_t i = 1; i < argc; i++) {
		int n = snprintk(&line[off], sizeof(line) - off, i == 1 ? "%s" : " %s", argv[i]);

		if (n < 0 || (size_t)n >= sizeof(line) - off) {
			shell_error(sh, "command too long");
			return -EINVAL;
		}
		off += n;
	}

	if (send_line(sh, line) != 0) {
		return -EIO;
	}
	shell_print(sh, "sent to camera: %s", line);
	return 0;
}

/*
 * On-demand status. Deliberately independent of the logging subsystem: when
 * logs are misconfigured or a capture is passive, this still answers.
 */
static int cmd_dectstat(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct radio_stats rs;
	struct uart_in_stats us;

	radio_get_stats(&rs);
	uart_in_get_stats(&us);

	shell_print(sh, "device id     : %u", radio_device_id());
	shell_print(sh, "-- uart from camera --");
	shell_print(sh, "  packets     : %u", us.packets);
	shell_print(sh, "  bytes       : %llu", (unsigned long long)us.bytes);
	shell_print(sh, "  crc errors  : %u", us.crc_err);
	shell_print(sh, "  resyncs     : %u", us.resyncs);
	shell_print(sh, "  overruns    : %u", us.overruns);
	shell_print(sh, "-- radio --");
	shell_print(sh, "  tx ok       : %u", rs.tx_ok);
	shell_print(sh, "  tx err      : %u", rs.tx_err);
	shell_print(sh, "  lbt busy    : %u", rs.tx_lbt_busy);
	shell_print(sh, "  tx bytes    : %llu", (unsigned long long)rs.tx_bytes);

	if (us.bytes == 0) {
		shell_warn(sh, "");
		shell_warn(sh, "No bytes at all from the camera. Check that it is powered");
		shell_warn(sh, "and that GPIO42 reaches P0.02 (Arduino D2).");
		shell_warn(sh, "If the camera reports 'link stalled', it is being held off");
		shell_warn(sh, "by flow control: check GPIO1 against P0.06 (Arduino D6).");
	}
	return 0;
}

SHELL_CMD_ARG_REGISTER(cam, NULL, "Control the camera over the uart back-channel",
		       cmd_cam, 1, 8);
SHELL_CMD_ARG_REGISTER(camraw, NULL, "Send a raw line to the camera", cmd_camraw, 1, 8);
SHELL_CMD_REGISTER(dectstat, NULL, "Transmitter link and radio counters", cmd_dectstat);
