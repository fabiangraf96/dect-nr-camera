/*
 * radio.c - DECT NR+ PHY transmit wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thin layer over nrf_modem_dect_phy: bring-up sequence, then one blocking
 * transmit call. Structured after the NCS hello_dect sample, which is the
 * reference for what this modem firmware accepts.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/hwinfo.h>

#include <nrf_modem_dect_phy.h>
#include <modem/nrf_modem_lib.h>

#include "radio.h"
#include "dectcam_cfg.h"

LOG_MODULE_REGISTER(radio, CONFIG_DECTCAM_LOG_LEVEL);

BUILD_ASSERT(CONFIG_DECTCAM_CARRIER != 0,
	     "CONFIG_DECTCAM_CARRIER must be set according to local regulations");

static uint16_t s_device_id;
static struct radio_stats s_stats;
static radio_ctrl_cb_t s_ctrl_cb;

/*
 * Live radio parameters, seeded from Kconfig.
 *
 * They were compile-time constants until control from the receiver was added.
 * Keeping them in one struct makes it obvious that the transport block size
 * and the fragment size are derived quantities that must be recomputed
 * whenever the MCS moves -- getting that wrong means the modem silently
 * rejects every transmit.
 */
static struct radio_cfg s_cfg = {
	.carrier = CONFIG_DECTCAM_CARRIER,
	.mcs = CONFIG_DECTCAM_MCS,
	.tx_power = CONFIG_DECTCAM_TX_POWER,
	.subslots = CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS,
	.tbs_bytes = DECTCAM_TBS_BYTES,
	.frag_payload = DECTCAM_FRAG_PAYLOAD,
};

/* Signalled when an asynchronous PHY operation finishes. */
static K_SEM_DEFINE(op_sem, 0, 1);
/* Result of the operation that just completed. */
static volatile int s_op_err;

static bool s_fatal;

#define TX_HANDLE 0u
#define RX_HANDLE 1u
#define OP_TIMEOUT K_MSEC(500)

static void handle_common(int err)
{
	s_op_err = err;
	k_sem_give(&op_sem);
}

static void dect_phy_event_handler(const struct nrf_modem_dect_phy_event *evt)
{
	switch (evt->id) {
	case NRF_MODEM_DECT_PHY_EVT_INIT:
		if (evt->init.err) {
			LOG_ERR("PHY init failed: %d", evt->init.err);
			s_fatal = true;
		}
		handle_common(evt->init.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CONFIGURE:
		if (evt->configure.err) {
			LOG_ERR("PHY configure failed: %d", evt->configure.err);
			s_fatal = true;
		}
		handle_common(evt->configure.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_ACTIVATE:
		if (evt->activate.err) {
			LOG_ERR("PHY activate failed: %d", evt->activate.err);
			s_fatal = true;
		}
		handle_common(evt->activate.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		handle_common(evt->op_complete.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		handle_common(evt->cancel.err);
		break;
	case NRF_MODEM_DECT_PHY_EVT_DEACTIVATE:
	case NRF_MODEM_DECT_PHY_EVT_DEINIT:
		handle_common(0);
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC:
		/*
		 * Only ever arrives during a listen window, and the only thing
		 * anyone sends this way is control. Hand it straight over; the
		 * callback must not call back into the modem.
		 */
		s_stats.ctrl_rx++;
		if (s_ctrl_cb) {
			s_ctrl_cb((const uint8_t *)evt->pdc.data, evt->pdc.len);
		}
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC:
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		/* Nothing to do: a missed control frame is simply repeated. */
		break;

	case NRF_MODEM_DECT_PHY_EVT_REJECT:
		LOG_WRN("operation rejected");
		break;
	default:
		/* Capability/band/time/RSSI queries are not used here. */
		break;
	}
}

/* Run one bring-up step and wait for its completion event. */
static int step(const char *what, int err)
{
	if (err) {
		LOG_ERR("%s returned %d", what, err);
		return err;
	}
	if (k_sem_take(&op_sem, K_SECONDS(5)) != 0) {
		LOG_ERR("%s timed out waiting for its event", what);
		return -ETIMEDOUT;
	}
	if (s_fatal) {
		return -EIO;
	}
	return s_op_err;
}

int radio_init(void)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed: %d", err);
		return err;
	}

	err = nrf_modem_dect_phy_event_handler_set(dect_phy_event_handler);
	if (err) {
		LOG_ERR("event_handler_set failed: %d", err);
		return err;
	}

	err = step("dect_phy_init", nrf_modem_dect_phy_init());
	if (err) {
		return err;
	}

	struct nrf_modem_dect_phy_config_params cfg = {
		.band_group_index = CONFIG_DECTCAM_BAND_GROUP_INDEX,
		.harq_rx_process_count = 4,
		.harq_rx_expiry_time_us = 5000000,
	};

	err = step("dect_phy_configure", nrf_modem_dect_phy_configure(&cfg));
	if (err) {
		return err;
	}

	err = step("dect_phy_activate",
		   nrf_modem_dect_phy_activate(NRF_MODEM_DECT_PHY_RADIO_MODE_LOW_LATENCY));
	if (err) {
		return err;
	}

	hwinfo_get_device_id((void *)&s_device_id, sizeof(s_device_id));

	LOG_INF("DECT PHY up: device id %u, carrier %d, MCS %d, %d subslots "
		"(%d B TBS, %d B payload, %d us air time), TX power idx %d",
		s_device_id, CONFIG_DECTCAM_CARRIER, CONFIG_DECTCAM_MCS,
		CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS, DECTCAM_TBS_BYTES, DECTCAM_FRAG_PAYLOAD,
		DECTCAM_AIRTIME_US, CONFIG_DECTCAM_TX_POWER);

	return 0;
}

int radio_tx(const uint8_t *data, size_t len)
{
	if (len == 0 || len > (size_t)s_cfg.tbs_bytes) {
		return -EINVAL;
	}

	struct dectcam_phy_hdr_type1 hdr = {
		.header_format = 0,
		.packet_length_type = 0, /* subslots */
		.packet_length = (uint8_t)(s_cfg.subslots - 1),
		.short_network_id = (CONFIG_DECTCAM_NETWORK_ID & 0xff),
		.transmitter_id_hi = (s_device_id >> 8),
		.transmitter_id_lo = (s_device_id & 0xff),
		.transmit_power = s_cfg.tx_power,
		.reserved = 0,
		.df_mcs = s_cfg.mcs,
	};

	struct nrf_modem_dect_phy_tx_params tx = {
		.start_time = 0, /* as soon as possible */
		.handle = TX_HANDLE,
		.network_id = CONFIG_DECTCAM_NETWORK_ID,
		.phy_type = 0,
		.carrier = s_cfg.carrier,
		.phy_header = (union nrf_modem_dect_phy_hdr *)&hdr,
		.data = (void *)data,
		.data_size = len,
#if CONFIG_DECTCAM_TX_LBT
		.lbt_rssi_threshold_max = CONFIG_DECTCAM_TX_LBT_THRESHOLD,
		.lbt_period = NRF_MODEM_DECT_LBT_PERIOD_MAX,
#else
		/* lbt_period 0 disables listen-before-talk entirely. */
		.lbt_rssi_threshold_max = 0,
		.lbt_period = 0,
#endif
	};

	int err = nrf_modem_dect_phy_tx(&tx);

	if (err) {
		s_stats.tx_err++;
		return err;
	}

	/*
	 * Wait for completion. Without this the next call could be issued
	 * while the modem still owns `data`, and the modem would reject the
	 * operation anyway once its queue filled.
	 */
	if (k_sem_take(&op_sem, OP_TIMEOUT) != 0) {
		s_stats.tx_err++;
		LOG_WRN("TX completion timed out");
		return -ETIMEDOUT;
	}

	if (s_op_err) {
		if (s_op_err == NRF_MODEM_DECT_PHY_ERR_LBT_CHANNEL_BUSY) {
			s_stats.tx_lbt_busy++;
		} else {
			s_stats.tx_err++;
			LOG_WRN("TX completed with error %d", s_op_err);
		}
		return -EIO;
	}

	s_stats.tx_ok++;
	s_stats.tx_bytes += len;
	return 0;
}

int radio_listen(uint32_t ms)
{
	struct nrf_modem_dect_phy_rx_params rx = {
		.start_time = 0,
		.handle = RX_HANDLE,
		.network_id = CONFIG_DECTCAM_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = s_cfg.carrier,
		.duration = ms * NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_DECTCAM_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		.filter.receiver_identity = 0,
	};

	int err = nrf_modem_dect_phy_rx(&rx);

	if (err) {
		LOG_WRN("listen failed to start: %d", err);
		return err;
	}

	s_stats.listen_windows++;

	/*
	 * Wait for the window to elapse. The generous margin is because the
	 * modem reports completion only after the whole duration, and a video
	 * fragment must not be queued while the receiver still owns the radio.
	 */
	if (k_sem_take(&op_sem, K_MSEC(ms + 200)) != 0) {
		LOG_WRN("listen window did not complete");
		return -ETIMEDOUT;
	}
	return 0;
}

void radio_set_ctrl_cb(radio_ctrl_cb_t cb)
{
	s_ctrl_cb = cb;
}

uint16_t radio_device_id(void)
{
	return s_device_id;
}

void radio_get_cfg(struct radio_cfg *out)
{
	if (out) {
		*out = s_cfg;
	}
}

int radio_set_tx_power(uint8_t idx)
{
	/*
	 * The header field is 4 bits. Which indices map to which dBm depends
	 * on the band and the modem firmware, so the only check worth making
	 * here is that it fits.
	 */
	if (idx > 15) {
		return -EINVAL;
	}
	s_cfg.tx_power = idx;
	LOG_INF("tx power index -> %u", idx);
	return 0;
}

int radio_set_mcs(uint8_t mcs)
{
	const int tbs = dectcam_tbs_bytes_subslots(mcs, s_cfg.subslots);

	if (tbs < 0) {
		LOG_WRN("MCS %u is not valid with %u subslots", mcs, s_cfg.subslots);
		return -EINVAL;
	}
	if (tbs <= (int)DECTCAM_OVERHEAD) {
		return -EINVAL;
	}

	/*
	 * Refuse a transport block that cannot carry the fragments the camera
	 * is currently producing. Accepting it would leave every transmit
	 * failing with -EINVAL and the link silently dead; the caller can lower
	 * the fragment size first and try again.
	 */
	if (tbs < (int)(s_cfg.frag_payload + DECTCAM_OVERHEAD)) {
		LOG_WRN("MCS %u gives a %d B block, too small for %u B fragments",
			mcs, tbs, s_cfg.frag_payload);
		return -EINVAL;
	}

	s_cfg.mcs = mcs;
	s_cfg.tbs_bytes = (uint16_t)tbs;
	LOG_INF("MCS -> %u (%d B transport block)", mcs, tbs);
	return 0;
}

int radio_set_carrier(uint16_t carrier)
{
	if (carrier == 0) {
		return -EINVAL;
	}
	s_cfg.carrier = carrier;
	LOG_INF("carrier -> %u", carrier);
	return 0;
}

void radio_get_stats(struct radio_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
