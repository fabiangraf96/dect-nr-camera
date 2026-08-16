/*
 * console_cmds.h - runtime control over the ESP32's own UART0 console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DECTCAM_CONSOLE_CMDS_H
#define DECTCAM_CONSOLE_CMDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the `cam`, `stats` and `meta` commands and starts the REPL. */
void console_start(void);

/*
 * Execute one command line, as if typed at the console.
 *
 * Used by the UART2 back-channel so the camera can be controlled from the
 * nRF9151 shell when the ESP32's own USB console is not available.
 *
 * Any resulting parameter change is announced with a FRAME_META packet, which
 * is how the operator sees that it took effect -- no reply is sent on UART2,
 * because that wire carries the video stream.
 */
void console_exec_line(const char *line);

/* Set by the `cam pause` / `cam resume` commands. */
int console_streaming_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* DECTCAM_CONSOLE_CMDS_H */
