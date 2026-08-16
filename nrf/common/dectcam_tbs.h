/*
 * dectcam_tbs.h - DECT NR+ transport block sizes.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Transport block size lookup for mu=1, beta=1, single antenna, derived from
 * ETSI TS 103 636-3 section 5.3. The numbers are taken from the NCS dect_shell
 * sample (nrf/samples/dect/dect_phy/dect_shell/src/dect/common/
 * dect_common_utils.c) so that our fragment size always matches what the modem
 * will actually accept.
 *
 * The data appears TWICE, deliberately:
 *
 *   1. DECTCAM_TBS_BITS_<mcs>_<subslots> object-like macros, so the size can
 *      be used in BUILD_ASSERT. A `static const` array cannot: reading an
 *      array element is not a constant expression in C.
 *
 *   2. dectcam_tbs_bits[][], for runtime lookup where the MCS is a variable.
 *
 * test_proto.c cross-checks the two against each other, so they cannot drift.
 *
 * Combinations the PHY does not support are 0 here (the dect_shell table
 * writes them as -1). Asking the modem for one yields
 * NRF_MODEM_DECT_PHY_ERR_UNSUPPORTED_TBS.
 */

#ifndef DECTCAM_TBS_H
#define DECTCAM_TBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time table: bits, indexed by [MCS]_[subslot count 1..16]    */
/* ------------------------------------------------------------------ */

/* MCS 0 */
#define DECTCAM_TBS_BITS_0_1     0
#define DECTCAM_TBS_BITS_0_2   136
#define DECTCAM_TBS_BITS_0_3   264
#define DECTCAM_TBS_BITS_0_4   400
#define DECTCAM_TBS_BITS_0_5   536
#define DECTCAM_TBS_BITS_0_6   664
#define DECTCAM_TBS_BITS_0_7   792
#define DECTCAM_TBS_BITS_0_8   920
#define DECTCAM_TBS_BITS_0_9  1064
#define DECTCAM_TBS_BITS_0_10 1192
#define DECTCAM_TBS_BITS_0_11 1320
#define DECTCAM_TBS_BITS_0_12 1448
#define DECTCAM_TBS_BITS_0_13 1576
#define DECTCAM_TBS_BITS_0_14 1704
#define DECTCAM_TBS_BITS_0_15 1864
#define DECTCAM_TBS_BITS_0_16 1992

/* MCS 1 */
#define DECTCAM_TBS_BITS_1_1    32
#define DECTCAM_TBS_BITS_1_2   296
#define DECTCAM_TBS_BITS_1_3   552
#define DECTCAM_TBS_BITS_1_4   824
#define DECTCAM_TBS_BITS_1_5  1096
#define DECTCAM_TBS_BITS_1_6  1352
#define DECTCAM_TBS_BITS_1_7  1608
#define DECTCAM_TBS_BITS_1_8  1864
#define DECTCAM_TBS_BITS_1_9  2104
#define DECTCAM_TBS_BITS_1_10 2360
#define DECTCAM_TBS_BITS_1_11 2616
#define DECTCAM_TBS_BITS_1_12 2872
#define DECTCAM_TBS_BITS_1_13 3128
#define DECTCAM_TBS_BITS_1_14 3384
#define DECTCAM_TBS_BITS_1_15 3704
#define DECTCAM_TBS_BITS_1_16 3960

/* MCS 2 */
#define DECTCAM_TBS_BITS_2_1    56
#define DECTCAM_TBS_BITS_2_2   456
#define DECTCAM_TBS_BITS_2_3   856
#define DECTCAM_TBS_BITS_2_4  1256
#define DECTCAM_TBS_BITS_2_5  1640
#define DECTCAM_TBS_BITS_2_6  2024
#define DECTCAM_TBS_BITS_2_7  2360
#define DECTCAM_TBS_BITS_2_8  2744
#define DECTCAM_TBS_BITS_2_9  3192
#define DECTCAM_TBS_BITS_2_10 3576
#define DECTCAM_TBS_BITS_2_11 3960
#define DECTCAM_TBS_BITS_2_12 4320
#define DECTCAM_TBS_BITS_2_13 4768
#define DECTCAM_TBS_BITS_2_14 5152
#define DECTCAM_TBS_BITS_2_15 5536
#define DECTCAM_TBS_BITS_2_16    0 /* unsupported */

/* MCS 3 */
#define DECTCAM_TBS_BITS_3_1    88
#define DECTCAM_TBS_BITS_3_2   616
#define DECTCAM_TBS_BITS_3_3  1128
#define DECTCAM_TBS_BITS_3_4  1672
#define DECTCAM_TBS_BITS_3_5  2168
#define DECTCAM_TBS_BITS_3_6  2680
#define DECTCAM_TBS_BITS_3_7  3192
#define DECTCAM_TBS_BITS_3_8  3704
#define DECTCAM_TBS_BITS_3_9  4256
#define DECTCAM_TBS_BITS_3_10 4768
#define DECTCAM_TBS_BITS_3_11 5280
#define DECTCAM_TBS_BITS_3_12    0 /* unsupported */
#define DECTCAM_TBS_BITS_3_13    0
#define DECTCAM_TBS_BITS_3_14    0
#define DECTCAM_TBS_BITS_3_15    0
#define DECTCAM_TBS_BITS_3_16    0

/* MCS 4 */
#define DECTCAM_TBS_BITS_4_1   144
#define DECTCAM_TBS_BITS_4_2   936
#define DECTCAM_TBS_BITS_4_3  1736
#define DECTCAM_TBS_BITS_4_4  2488
#define DECTCAM_TBS_BITS_4_5  3256
#define DECTCAM_TBS_BITS_4_6  4024
#define DECTCAM_TBS_BITS_4_7  4832
#define DECTCAM_TBS_BITS_4_8  5600
#define DECTCAM_TBS_BITS_4_9     0 /* unsupported */
#define DECTCAM_TBS_BITS_4_10    0
#define DECTCAM_TBS_BITS_4_11    0
#define DECTCAM_TBS_BITS_4_12    0
#define DECTCAM_TBS_BITS_4_13    0
#define DECTCAM_TBS_BITS_4_14    0
#define DECTCAM_TBS_BITS_4_15    0
#define DECTCAM_TBS_BITS_4_16    0

/*
 * Constant-expression lookup. `mcs` and `subslots` must expand to plain
 * integer literals (which Kconfig values do), because they are token-pasted.
 */
#define DECTCAM_TBS_CAT_(mcs, subslots) DECTCAM_TBS_BITS_##mcs##_##subslots
#define DECTCAM_TBS_BITS_OF(mcs, subslots) DECTCAM_TBS_CAT_(mcs, subslots)

/* Bytes; 0 means unsupported. */
#define DECTCAM_TBS_BYTES_OF(mcs, subslots) (DECTCAM_TBS_BITS_OF(mcs, subslots) / 8)

/* ------------------------------------------------------------------ */
/* Runtime table                                                       */
/* ------------------------------------------------------------------ */

#define DECTCAM_N_MCS             5
#define DECTCAM_N_SUBSLOT_LENGTHS 16

/* Indexed [mcs][subslots - 1]; 0 means unsupported. */
static const int16_t dectcam_tbs_bits[DECTCAM_N_MCS][DECTCAM_N_SUBSLOT_LENGTHS] = {
	{ 0, 136, 264, 400, 536, 664, 792, 920,
	  1064, 1192, 1320, 1448, 1576, 1704, 1864, 1992 },
	{ 32, 296, 552, 824, 1096, 1352, 1608, 1864,
	  2104, 2360, 2616, 2872, 3128, 3384, 3704, 3960 },
	{ 56, 456, 856, 1256, 1640, 2024, 2360, 2744,
	  3192, 3576, 3960, 4320, 4768, 5152, 5536, 0 },
	{ 88, 616, 1128, 1672, 2168, 2680, 3192, 3704,
	  4256, 4768, 5280, 0, 0, 0, 0, 0 },
	{ 144, 936, 1736, 2488, 3256, 4024, 4832, 5600,
	  0, 0, 0, 0, 0, 0, 0, 0 },
};

/*
 * Bytes for a packet length given in subslots (1..16).
 * Returns -1 if the combination is unsupported.
 */
static inline int dectcam_tbs_bytes_subslots(uint8_t mcs, uint8_t subslots)
{
	if (mcs >= DECTCAM_N_MCS || subslots < 1 || subslots > DECTCAM_N_SUBSLOT_LENGTHS) {
		return -1;
	}
	const int16_t bits = dectcam_tbs_bits[mcs][subslots - 1];

	return (bits == 0) ? -1 : (bits / 8);
}

/*
 * Air time in microseconds, mu=1.
 * Frame = 10 ms / 24 slots => slot = 416.667 us, subslot = 208.333 us.
 */
static inline uint32_t dectcam_airtime_us_subslots(uint8_t subslots)
{
	return ((uint32_t)subslots * 625u) / 3u;
}

#ifdef __cplusplus
}
#endif

#endif /* DECTCAM_TBS_H */
