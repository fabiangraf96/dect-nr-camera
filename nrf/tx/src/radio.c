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

/* Signalled when an asynchronous PHY operation finishes. */
static K_SEM_DEFINE(op_sem, 0, 1);
/* Result of the operation that just completed. */
static volatile int s_op_err;

static bool s_fatal;

#define TX_HANDLE 0u
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
	case NRF_MODEM_DECT_PHY_EVT_REJECT:
		LOG_WRN("operation rejected");
		break;
	default:
		/* Capability/band/time/RSSI queries and RX events are not used
		 * by the transmitter. */
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
	if (len == 0 || len > (size_t)DECTCAM_TBS_BYTES) {
		return -EINVAL;
	}

	struct dectcam_phy_hdr_type1 hdr = {
		.header_format = 0,
		.packet_length_type = 0, /* subslots */
		.packet_length = DECTCAM_PKT_LEN_FIELD,
		.short_network_id = (CONFIG_DECTCAM_NETWORK_ID & 0xff),
		.transmitter_id_hi = (s_device_id >> 8),
		.transmitter_id_lo = (s_device_id & 0xff),
		.transmit_power = CONFIG_DECTCAM_TX_POWER,
		.reserved = 0,
		.df_mcs = CONFIG_DECTCAM_MCS,
	};

	struct nrf_modem_dect_phy_tx_params tx = {
		.start_time = 0, /* as soon as possible */
		.handle = TX_HANDLE,
		.network_id = CONFIG_DECTCAM_NETWORK_ID,
		.phy_type = 0,
		.carrier = CONFIG_DECTCAM_CARRIER,
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

uint16_t radio_device_id(void)
{
	return s_device_id;
}

void radio_get_stats(struct radio_stats *out)
{
	if (out) {
		*out = s_stats;
	}
}
