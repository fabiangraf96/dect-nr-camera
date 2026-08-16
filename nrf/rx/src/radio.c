/*
 * radio.c - DECT NR+ PHY receive wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Keeps the PHY in continuous receive and hands every good PDC to a callback.
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
static radio_pdc_cb_t s_pdc_cb;

static K_SEM_DEFINE(op_sem, 0, 1);
static volatile int s_op_err;
static bool s_fatal;

#define RX_HANDLE 1u

/*
 * Continuous RX still ends after `duration`, so it is restarted from a work
 * item when it completes. Using the modem's maximum keeps the restart rate --
 * and therefore the gap in coverage -- as low as possible.
 */
#define RX_DURATION_S 60

static void rx_restart_work_fn(struct k_work *work);
static K_WORK_DEFINE(rx_restart_work, rx_restart_work_fn);

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

	case NRF_MODEM_DECT_PHY_EVT_PCC:
		s_stats.pcc_ok++;
		s_stats.last_rssi_2 = evt->pcc.rssi_2;
		s_stats.last_snr = evt->pcc.snr;
		break;
	case NRF_MODEM_DECT_PHY_EVT_PCC_ERROR:
		s_stats.pcc_crc_err++;
		break;

	case NRF_MODEM_DECT_PHY_EVT_PDC:
		s_stats.pdc_ok++;
		s_stats.rx_bytes += evt->pdc.len;
		s_stats.last_rssi_2 = evt->pdc.rssi_2;
		s_stats.last_snr = evt->pdc.snr;
		if (s_pdc_cb) {
			s_pdc_cb((const uint8_t *)evt->pdc.data, evt->pdc.len);
		}
		break;
	case NRF_MODEM_DECT_PHY_EVT_PDC_ERROR:
		s_stats.pdc_crc_err++;
		break;

	case NRF_MODEM_DECT_PHY_EVT_COMPLETED:
		/*
		 * The RX window elapsed. Restart from a work item rather than
		 * here: calling back into the modem from its own callback is
		 * not allowed.
		 */
		k_work_submit(&rx_restart_work);
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		break;
	case NRF_MODEM_DECT_PHY_EVT_REJECT:
		LOG_WRN("operation rejected");
		break;
	default:
		break;
	}
}

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

int radio_start_rx(void)
{
	struct nrf_modem_dect_phy_rx_params rx = {
		.start_time = 0,
		.handle = RX_HANDLE,
		.network_id = CONFIG_DECTCAM_NETWORK_ID,
		.mode = NRF_MODEM_DECT_PHY_RX_MODE_CONTINUOUS,
		.rssi_interval = NRF_MODEM_DECT_PHY_RSSI_INTERVAL_OFF,
		.link_id = NRF_MODEM_DECT_PHY_LINK_UNSPECIFIED,
		.rssi_level = -60,
		.carrier = CONFIG_DECTCAM_CARRIER,
		.duration = (uint32_t)RX_DURATION_S * MSEC_PER_SEC *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_DECTCAM_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* Broadcast: accept from any transmitter on this network. */
		.filter.receiver_identity = 0,
	};

	int err = nrf_modem_dect_phy_rx(&rx);

	if (err) {
		LOG_ERR("nrf_modem_dect_phy_rx failed: %d", err);
	}
	return err;
}

static void rx_restart_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	s_stats.rx_restarts++;
	radio_start_rx();
}

int radio_init(radio_pdc_cb_t cb)
{
	int err;

	s_pdc_cb = cb;

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

	LOG_INF("DECT PHY up: device id %u, carrier %d, MCS %d, %d subslots (%d B TBS)",
		s_device_id, CONFIG_DECTCAM_CARRIER, CONFIG_DECTCAM_MCS,
		CONFIG_DECTCAM_PACKET_LEN_SUBSLOTS, DECTCAM_TBS_BYTES);

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
