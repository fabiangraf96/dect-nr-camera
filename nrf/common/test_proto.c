/*
 * Host-side self-test for dectcam_proto.h / dectcam_tbs.h.
 *
 * Build & run:  cc -Wall -Wextra -O2 -o /tmp/test_proto test_proto.c && /tmp/test_proto
 *
 * Also prints the CRC of a known vector so the Python mirror can be checked
 * against the C implementation.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "dectcam_proto.h"
#include "dectcam_tbs.h"

static int failures;

#define CHECK(cond, ...)                                                                 \
	do {                                                                             \
		if (!(cond)) {                                                           \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
			printf(__VA_ARGS__);                                             \
			printf("\n");                                                    \
			failures++;                                                      \
		}                                                                        \
	} while (0)

static void test_crc_vectors(void)
{
	/* CRC-16/CCITT-FALSE check value for "123456789" is 0x29B1. */
	const uint8_t v[] = "123456789";
	uint16_t crc = dectcam_crc16(v, 9);

	CHECK(crc == 0x29B1, "crc16(\"123456789\") = 0x%04X, want 0x29B1", crc);
	printf("  crc16(\"123456789\") = 0x%04X\n", crc);
}

static void test_roundtrip(void)
{
	uint8_t payload[687];
	uint8_t buf[DECTCAM_MAX_PACKET];

	for (size_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(i * 7 + 3);
	}

	size_t n = dectcam_encode(buf, sizeof(buf), DECTCAM_TYPE_JPEG_FRAG, 0x1234, 5, 22,
				  payload, sizeof(payload));

	CHECK(n == sizeof(payload) + DECTCAM_OVERHEAD, "encode len %zu", n);

	struct dectcam_pkt pkt;
	size_t consumed = 0;
	int r = dectcam_decode(buf, n, &pkt, &consumed);

	CHECK(r == DECTCAM_OK, "decode returned %d", r);
	CHECK(consumed == n, "consumed %zu != %zu", consumed, n);
	CHECK(pkt.version == DECTCAM_VERSION, "version %u", pkt.version);
	CHECK(pkt.type == DECTCAM_TYPE_JPEG_FRAG, "type %u", pkt.type);
	CHECK(pkt.frame_id == 0x1234, "frame_id %u", pkt.frame_id);
	CHECK(pkt.frag_idx == 5, "frag_idx %u", pkt.frag_idx);
	CHECK(pkt.frag_cnt == 22, "frag_cnt %u", pkt.frag_cnt);
	CHECK(pkt.payload_len == sizeof(payload), "payload_len %u", pkt.payload_len);
	CHECK(memcmp(pkt.payload, payload, sizeof(payload)) == 0, "payload mismatch");
}

static void test_zero_payload(void)
{
	uint8_t buf[DECTCAM_MAX_PACKET];
	size_t n = dectcam_encode(buf, sizeof(buf), DECTCAM_TYPE_STATS, 0, 0, 0, NULL, 0);

	CHECK(n == DECTCAM_OVERHEAD, "zero-payload len %zu", n);

	struct dectcam_pkt pkt;
	size_t consumed;

	CHECK(dectcam_decode(buf, n, &pkt, &consumed) == DECTCAM_OK, "zero-payload decode");
	CHECK(pkt.payload_len == 0, "zero-payload len field");
}

static void test_corruption_detected(void)
{
	uint8_t payload[64];
	uint8_t buf[DECTCAM_MAX_PACKET];

	memset(payload, 0xA5, sizeof(payload));
	size_t n = dectcam_encode(buf, sizeof(buf), DECTCAM_TYPE_JPEG_FRAG, 1, 0, 1, payload,
				  sizeof(payload));

	/* Flip one bit in every byte position and confirm the CRC catches it. */
	int missed = 0;

	for (size_t i = DECTCAM_OFF_VERTYPE; i < n; i++) {
		buf[i] ^= 0x01;
		struct dectcam_pkt pkt;
		size_t consumed;
		int r = dectcam_decode(buf, n, &pkt, &consumed);

		if (r == DECTCAM_OK) {
			missed++;
		}
		buf[i] ^= 0x01;
	}
	CHECK(missed == 0, "%d single-bit corruptions went undetected", missed);
}

static void test_short_and_magic(void)
{
	uint8_t buf[DECTCAM_MAX_PACKET];
	size_t n = dectcam_encode(buf, sizeof(buf), DECTCAM_TYPE_JPEG_FRAG, 1, 0, 1,
				  (const uint8_t *)"hello", 5);

	struct dectcam_pkt pkt;
	size_t consumed;

	CHECK(dectcam_decode(buf, 3, &pkt, &consumed) == DECTCAM_ERR_SHORT, "short hdr");
	CHECK(dectcam_decode(buf, n - 1, &pkt, &consumed) == DECTCAM_ERR_SHORT, "short body");

	buf[0] = 0x00;
	CHECK(dectcam_decode(buf, n, &pkt, &consumed) == DECTCAM_ERR_MAGIC, "bad magic");
}

static void test_bad_length_rejected(void)
{
	uint8_t buf[DECTCAM_MAX_PACKET];

	memset(buf, 0, sizeof(buf));
	buf[0] = DECTCAM_MAGIC0;
	buf[1] = DECTCAM_MAGIC1;
	buf[DECTCAM_OFF_VERTYPE] = (DECTCAM_VERSION << 4) | DECTCAM_TYPE_JPEG_FRAG;
	dectcam_put_u16(&buf[DECTCAM_OFF_PAYLOAD_LEN], 60000);

	struct dectcam_pkt pkt;
	size_t consumed;

	CHECK(dectcam_decode(buf, sizeof(buf), &pkt, &consumed) == DECTCAM_ERR_LENGTH,
	      "implausible length not rejected");
}

static void test_meta_stats(void)
{
	struct dectcam_meta m = { 320, 240, 12, 10, 687 }, m2;
	uint8_t b[DECTCAM_META_LEN];

	dectcam_meta_pack(b, &m);
	dectcam_meta_unpack(b, &m2);
	CHECK(memcmp(&m, &m2, sizeof(m)) == 0, "meta roundtrip");

	/* -37 half-dBm = -18.5 dBm, which is what the DK link actually shows. */
	struct dectcam_stats s = { -37, 40, 1234, 5, 999 }, s2;
	uint8_t c[DECTCAM_STATS_LEN];

	dectcam_stats_pack(c, &s);
	dectcam_stats_unpack(c, &s2);
	CHECK(memcmp(&s, &s2, sizeof(s)) == 0, "stats roundtrip");
	CHECK(s2.rssi_2 == -37, "negative rssi survived: %d", s2.rssi_2);
	CHECK(s2.snr == 40, "snr survived: %d", s2.snr);
	printf("  stats: rssi=%.1f dBm snr=%.2f dB\n", s2.rssi_2 / 2.0, s2.snr / 4.0);

	struct dectcam_stats ns = { DECTCAM_RSSI2_NOT_MEASURED, DECTCAM_SNR_NOT_MEASURED,
				    0, 0, 0 }, ns2;
	uint8_t d[DECTCAM_STATS_LEN];

	dectcam_stats_pack(d, &ns);
	dectcam_stats_unpack(d, &ns2);
	CHECK(ns2.rssi_2 == DECTCAM_RSSI2_NOT_MEASURED, "rssi sentinel survived");
	CHECK(ns2.snr == DECTCAM_SNR_NOT_MEASURED, "snr sentinel survived");
}

/* Every entry of the token-pasted macro table, for cross-checking. */
static const int16_t macro_table[DECTCAM_N_MCS][DECTCAM_N_SUBSLOT_LENGTHS] = {
#define ROW(m)                                                                           \
	{ DECTCAM_TBS_BITS_OF(m, 1),  DECTCAM_TBS_BITS_OF(m, 2),                         \
	  DECTCAM_TBS_BITS_OF(m, 3),  DECTCAM_TBS_BITS_OF(m, 4),                         \
	  DECTCAM_TBS_BITS_OF(m, 5),  DECTCAM_TBS_BITS_OF(m, 6),                         \
	  DECTCAM_TBS_BITS_OF(m, 7),  DECTCAM_TBS_BITS_OF(m, 8),                         \
	  DECTCAM_TBS_BITS_OF(m, 9),  DECTCAM_TBS_BITS_OF(m, 10),                        \
	  DECTCAM_TBS_BITS_OF(m, 11), DECTCAM_TBS_BITS_OF(m, 12),                        \
	  DECTCAM_TBS_BITS_OF(m, 13), DECTCAM_TBS_BITS_OF(m, 14),                        \
	  DECTCAM_TBS_BITS_OF(m, 15), DECTCAM_TBS_BITS_OF(m, 16) }
	ROW(0), ROW(1), ROW(2), ROW(3), ROW(4),
#undef ROW
};

static void test_tbs(void)
{
	/*
	 * The macro table (used by BUILD_ASSERT) and the runtime array must
	 * agree; they are separate transcriptions of the same spec data.
	 */
	int mismatches = 0;

	for (int m = 0; m < DECTCAM_N_MCS; m++) {
		for (int s = 0; s < DECTCAM_N_SUBSLOT_LENGTHS; s++) {
			if (macro_table[m][s] != dectcam_tbs_bits[m][s]) {
				printf("  TBS mismatch MCS%d/%d subslots: macro=%d array=%d\n",
				       m, s + 1, macro_table[m][s], dectcam_tbs_bits[m][s]);
				mismatches++;
			}
		}
	}
	CHECK(mismatches == 0, "%d macro/array TBS mismatches", mismatches);

	/* Values quoted in docs/architecture.md. Subslot counts are 1-based. */
	CHECK(dectcam_tbs_bytes_subslots(4, 8) == 700, "MCS4/8subslots = %d",
	      dectcam_tbs_bytes_subslots(4, 8));
	CHECK(dectcam_tbs_bytes_subslots(2, 15) == 692, "MCS2/15subslots = %d",
	      dectcam_tbs_bytes_subslots(2, 15));
	CHECK(dectcam_tbs_bytes_subslots(3, 11) == 660, "MCS3/11subslots = %d",
	      dectcam_tbs_bytes_subslots(3, 11));
	CHECK(dectcam_tbs_bytes_subslots(4, 9) == -1, "MCS4/9subslots should be unsupported");
	CHECK(dectcam_tbs_bytes_subslots(3, 12) == -1, "MCS3/12subslots should be unsupported");
	CHECK(dectcam_tbs_bytes_subslots(5, 1) == -1, "MCS5 should not exist");
	CHECK(dectcam_tbs_bytes_subslots(4, 0) == -1, "0 subslots is not valid");

	/* The constant-expression path must give the same answer. */
	CHECK(DECTCAM_TBS_BYTES_OF(4, 8) == 700, "macro MCS4/8 = %d",
	      DECTCAM_TBS_BYTES_OF(4, 8));

	printf("  macro/array TBS tables agree (%d entries)\n",
	       DECTCAM_N_MCS * DECTCAM_N_SUBSLOT_LENGTHS);
	printf("  max payload MCS4/8subslots = %d bytes\n",
	       dectcam_tbs_bytes_subslots(4, 8) - (int)DECTCAM_OVERHEAD);
	printf("  air time 8 subslots        = %u us\n", dectcam_airtime_us_subslots(8));
}

static void print_reference_vector(void)
{
	uint8_t buf[DECTCAM_MAX_PACKET];
	const uint8_t payload[] = { 0xFF, 0xD8, 0xAA, 0xBB, 0xFF, 0xD9 };
	size_t n = dectcam_encode(buf, sizeof(buf), DECTCAM_TYPE_JPEG_FRAG, 0x0102, 0x0003,
				  0x0004, payload, sizeof(payload));

	printf("\n  reference vector (%zu bytes): ", n);
	for (size_t i = 0; i < n; i++) {
		printf("%02X", buf[i]);
	}
	printf("\n");
}

int main(void)
{
	printf("dectcam_proto self-test\n");
	test_crc_vectors();
	test_roundtrip();
	test_zero_payload();
	test_corruption_detected();
	test_short_and_magic();
	test_bad_length_rejected();
	test_meta_stats();
	test_tbs();
	print_reference_vector();

	printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
	return failures ? 1 : 0;
}
