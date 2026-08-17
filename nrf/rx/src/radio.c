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

/* Live carrier. Seeded from Kconfig, retuned at run time when the operator
 * moves the link -- see ctrl.c for the sequence that keeps both ends together.
 */
static uint16_t s_carrier = CONFIG_DECTCAM_CARRIER;

static K_SEM_DEFINE(op_sem, 0, 1);
static volatile int s_op_err;
static bool s_fatal;

/*
 * Whether reception should be running.
 *
 * Continuous RX ends on its own after `duration` and is restarted from a work
 * item. That auto-restart has to be suppressed while the radio is borrowed for
 * a transmit, or the restart races the transmit and one of the two is
 * rejected.
 */
static volatile bool s_rx_wanted;

#define RX_HANDLE 1u
#define TX_HANDLE 0u

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
		 * Either the RX window elapsed or a transmit finished. Release
		 * anyone waiting, then restart reception -- but only if it is
		 * still wanted, so a deliberate stop is not undone.
		 *
		 * Restarting happens from a work item because calling back into
		 * the modem from its own callback is not allowed.
		 */
		handle_common(evt->op_complete.err);
		if (s_rx_wanted) {
			k_work_submit(&rx_restart_work);
		}
		break;
	case NRF_MODEM_DECT_PHY_EVT_CANCELED:
		handle_common(evt->cancel.err);
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
		.carrier = s_carrier,
		.duration = (uint32_t)RX_DURATION_S * MSEC_PER_SEC *
			    NRF_MODEM_DECT_MODEM_TIME_TICK_RATE_KHZ,
		.filter.short_network_id = CONFIG_DECTCAM_NETWORK_ID & 0xff,
		.filter.is_short_network_id_used = 1,
		/* Broadcast: accept from any transmitter on this network. */
		.filter.receiver_identity = 0,
	};

	s_rx_wanted = true;
	k_sem_reset(&op_sem);

	int err = nrf_modem_dect_phy_rx(&rx);

	if (err) {
		LOG_ERR("nrf_modem_dect_phy_rx failed: %d", err);
	}
	return err;
}

int radio_stop_rx(void)
{
	if (!s_rx_wanted) {
		return 0;
	}
	s_rx_wanted = false;

	int err = nrf_modem_dect_phy_cancel(RX_HANDLE);

	if (err) {
		LOG_WRN("cancel returned %d", err);
		return err;
	}

	/*
	 * Wait for the operation to actually stop. Transmitting while the
	 * receiver still owns the radio gets the transmit rejected.
	 */
	if (k_sem_take(&op_sem, K_MSEC(500)) != 0) {
		LOG_WRN("receive did not stop in time");
		return -ETIMEDOUT;
	}
	return 0;
}

int radio_send_ctrl(const uint8_t *data, size_t len)
{
	if (len == 0 || len > (size_t)DECTCAM_TBS_BYTES) {
		return -EINVAL;
	}

	const bool was_receiving = s_rx_wanted;

	if (was_receiving) {
		radio_stop_rx();
	}

	struct dectcam_phy_hdr_type1 hdr = {
		.header_format = 0,
		.packet_length_type = 0,
		.packet_length = DECTCAM_PKT_LEN_FIELD,
		.short_network_id = (CONFIG_DECTCAM_NETWORK_ID & 0xff),
		.transmitter_id_hi = (s_device_id >> 8),
		.transmitter_id_lo = (s_device_id & 0xff),
		.transmit_power = CONFIG_DECTCAM_TX_POWER,
		.reserved = 0,
		.df_mcs = CONFIG_DECTCAM_MCS,
	};

	struct nrf_modem_dect_phy_tx_params tx = {
		.start_time = 0,
		.handle = TX_HANDLE,
		.network_id = CONFIG_DECTCAM_NETWORK_ID,
		.phy_type = 0,
		.carrier = s_carrier,
		.phy_header = (union nrf_modem_dect_phy_hdr *)&hdr,
		.data = (void *)data,
		.data_size = len,
		.lbt_rssi_threshold_max = 0,
		.lbt_period = 0,
	};

	k_sem_reset(&op_sem);

	int err = nrf_modem_dect_phy_tx(&tx);

	if (err == 0) {
		if (k_sem_take(&op_sem, K_MSEC(500)) != 0) {
			err = -ETIMEDOUT;
		} else {
			err = s_op_err;
		}
	}

	if (err) {
		s_stats.ctrl_tx_err++;
		LOG_WRN("control transmit failed: %d", err);
	} else {
		s_stats.ctrl_tx++;
	}

	if (was_receiving) {
		radio_start_rx();
	}
	return err;
}

int radio_set_carrier(uint16_t carrier)
{
	if (carrier == 0) {
		return -EINVAL;
	}
	if (carrier == s_carrier) {
		return 0;
	}

	const bool was_receiving = s_rx_wanted;

	if (was_receiving) {
		radio_stop_rx();
	}
	s_carrier = carrier;
	LOG_INF("carrier -> %u", carrier);

	if (was_receiving) {
		return radio_start_rx();
	}
	return 0;
}

uint16_t radio_carrier(void)
{
	return s_carrier;
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
