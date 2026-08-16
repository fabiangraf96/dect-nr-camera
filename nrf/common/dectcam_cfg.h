/*
 * dectcam_cfg.h - derived link parameters, validated at build time.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Turns the Kconfig choices into the concrete numbers the radio and the
 * fragmenter need, and refuses to build if the combination is not one the
 * PHY actually supports. Catching that here is much better than watching the
 * modem return NRF_MODEM_DECT_PHY_ERR_UNSUPPORTED_TBS at runtime.
 */

#ifndef DECTCAM_CFG_H
#define DECTCAM_CFG_H

#include <zephyr/sys/util.h>

#include "dectcam_proto.h"
#include "dectcam_tbs.h"

/*
 * The 4-bit packet_length field is zero-based: a field value of N means N+1
 * subslots. Kconfig asks the user for the human number, so subtract one.
 */
#define DECTCAM_PKT_LEN_FIELD (CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS - 1)

BUILD_ASSERT(CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS >= 1 &&
		     CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS <= 16,
	     "CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS must be 1..16");

BUILD_ASSERT(CONFIG_DECTCAM_MCS >= 0 && CONFIG_DECTCAM_MCS <= 4,
	     "CONFIG_DECTCAM_MCS must be 0..4");

/*
 * Transport block size in bytes for the configured MCS / packet length.
 *
 * Uses the token-pasted macro table rather than the runtime array, because
 * this has to be a constant expression for the BUILD_ASSERTs below.
 */
#define DECTCAM_TBS_BYTES \
	DECTCAM_TBS_BYTES_OF(CONFIG_DECTCAM_MCS, CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS)

BUILD_ASSERT(DECTCAM_TBS_BYTES > 0,
	     "This CONFIG_DECTCAM_MCS / CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS "
	     "combination is not supported by the PHY. See the table in "
	     "common/Kconfig.dectcam; e.g. MCS 4 allows at most 8 subslots, "
	     "MCS 3 at most 11, MCS 2 at most 15.");

BUILD_ASSERT(DECTCAM_TBS_BYTES > (int)DECTCAM_OVERHEAD,
	     "Transport block is too small to hold even the 13-byte protocol "
	     "header; raise CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS or the MCS.");

/* Usable JPEG bytes per DECT transmission. */
#define DECTCAM_FRAG_PAYLOAD (DECTCAM_TBS_BYTES - (int)DECTCAM_OVERHEAD)

BUILD_ASSERT(DECTCAM_FRAG_PAYLOAD <= (int)DECTCAM_MAX_PAYLOAD,
	     "Fragment payload exceeds DECTCAM_MAX_PAYLOAD; raise the limit in "
	     "dectcam_proto.h.");

/* Air time of one transmission, microseconds (subslot = 208.33 us at mu=1). */
#define DECTCAM_AIRTIME_US (CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS * 625 / 3)

/*
 * DECT PHY header type 1.
 *
 * Field order differs from the specification because the modem expects the
 * struct in little-endian memory order; this layout is copied from the NCS
 * hello_dect sample, which is the reference for what the modem accepts.
 */
struct dectcam_phy_hdr_type1 {
	uint32_t packet_length: 4;
	uint32_t packet_length_type: 1;
	uint32_t header_format: 3;
	uint32_t short_network_id: 8;
	uint32_t transmitter_id_hi: 8;
	uint32_t transmitter_id_lo: 8;
	uint32_t df_mcs: 3;
	uint32_t reserved: 1;
	uint32_t transmit_power: 4;
	uint32_t pad: 24;
};

#endif /* DECTCAM_CFG_H */
