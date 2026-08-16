/*
 * pinprobe - decide electrically what is on the camera interface pins.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Flash this to the TRANSMITTER board in place of the normal firmware when the
 * link is silent and it is unclear whether the fault is the ESP32, the wiring,
 * or the radio.
 *
 * Each pin is sampled twice, once with an internal pull-down and once with a
 * pull-up. A weak internal pull (~13 kOhm) cannot fight a real push-pull
 * output, so the comparison is unambiguous:
 *
 *   pulldown LOW  and pullup HIGH -> nothing is driving it. The ESP32 is off,
 *                                    or the wire is not connected.
 *   pulldown HIGH                 -> actively driven high. For a UART TX line
 *                                    that means an initialised, idle
 *                                    transmitter, so the far end is alive.
 *   edges counted                 -> data is actually flowing.
 *
 * Remember to flash the real transmitter firmware again afterwards.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(pinprobe, LOG_LEVEL_INF);

#define PIN_RX  2 /* P0.02 - Arduino D2 <- ESP32 IO13 (its UART TX) */
#define PIN_TX  3 /* P0.03 - Arduino D3 -> ESP32 IO14 */
#define PIN_RTS 6 /* P0.06 - Arduino D6 -> ESP32 IO15 (its CTS)     */
#define PIN_CTS 7 /* P0.07 - Arduino D7, must stay unconnected      */

static void sample(uint32_t pin, nrf_gpio_pin_pull_t pull, int ms, int *pct_high, int *edges)
{
	nrf_gpio_cfg_input(pin, pull);
	k_busy_wait(2000);

	int hi = 0, lo = 0, prev = -1;

	*edges = 0;

	const int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		for (int i = 0; i < 200; i++) {
			const int v = nrf_gpio_pin_read(pin);

			if (v) {
				hi++;
			} else {
				lo++;
			}
			if (prev >= 0 && v != prev) {
				(*edges)++;
			}
			prev = v;
		}
	}
	*pct_high = (hi * 100) / (hi + lo);
}

static void report(const char *name, uint32_t pin)
{
	int pd, pu, epd, epu;

	sample(pin, NRF_GPIO_PIN_PULLDOWN, 100, &pd, &epd);
	sample(pin, NRF_GPIO_PIN_PULLUP, 100, &pu, &epu);

	const char *verdict;

	if (epd + epu > 10) {
		verdict = "DATA FLOWING";
	} else if (pd > 50) {
		verdict = "DRIVEN HIGH  (far end alive, wire OK)";
	} else if (pu < 50) {
		verdict = "DRIVEN LOW   (shorted to GND?)";
	} else {
		verdict = "FLOATING     (not connected, or far end unpowered)";
	}

	LOG_INF("%-14s pulldown %3d%% high, pullup %3d%% high, edges %d -> %s", name, pd, pu,
		epd + epu, verdict);
}

int main(void)
{
	LOG_INF("=== pin probe on the transmitter board ===");

	/*
	 * Assert CTS towards the camera by hand.
	 *
	 * The ESP32 will not transmit until its CTS input is low. Driving it
	 * from here, independently of the UARTE's own RTS logic, separates two
	 * possibilities: a wire that never carries the signal, versus a UARTE
	 * that is not asserting RTS when we think it is.
	 *
	 * If data starts flowing on P0.02 after this, the wire is fine.
	 */
	nrf_gpio_cfg_output(PIN_RTS);
	nrf_gpio_pin_clear(PIN_RTS);
	LOG_INF("P0.06 (D6 -> camera CTS) forced LOW = clear to send");

	for (int round = 0; round < 1000; round++) {
		report("P0.02 D2 data", PIN_RX);
		report("P0.07 D7 free", PIN_CTS);
		LOG_INF("---");
		k_msleep(1500);
	}
	return 0;
}
