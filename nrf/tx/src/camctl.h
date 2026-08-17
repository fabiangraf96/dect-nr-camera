/*
 * camctl.h - forward commands to the camera over the UART back-channel.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The same wire that carries video also carries text the other way. These
 * entry points exist so that control telegrams arriving over the air can be
 * passed on without going through the shell.
 *
 * Nothing comes back: the return direction is the video stream and must stay
 * clean. Confirmation reaches the operator as a FRAME_META packet announcing
 * the camera's new settings, which the viewer displays.
 */

#ifndef DECTCAM_TX_CAMCTL_H
#define DECTCAM_TX_CAMCTL_H

#include <stdint.h>

/*
 * Resolution by index into the camera's own table. An index is used rather
 * than a name because it is what travels on the wire; the order is fixed and
 * is checked by the protocol self-tests on both sides.
 */
int camctl_send_res(uint8_t index);

/* 1..30. */
int camctl_send_fps(uint8_t fps);

/* 4..63, lower is better quality and larger frames. */
int camctl_send_quality(uint8_t q);

/* The resolution names, in wire order. NULL if the index is out of range. */
const char *camctl_res_name(uint8_t index);

#endif /* DECTCAM_TX_CAMCTL_H */
