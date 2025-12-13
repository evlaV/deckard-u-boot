/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650P S2 (Only) Battery Interface for Deckard
 *  - Dead Battery Recovery
 *  - Charging Enablement
 */

#ifndef __DECKARD_BATTERY_H
#define __DECKARD_BATTERY_H

// #include <stdint.h>

#include <power/pmic.h>

#define SPL_BATTERY_MIN_BOOT_VOLTAGE 7200
#define BATTERY_MIN_BOOT_SOC     2

// Deckard uses MAX17320 Smart Empty
// UOCVP - Undervoltage Open Circuit Voltage Protection
// This is a value from the MAX17320 Fuel Gauge Model.
// It is set at 3280 * 2 = 6560 mV to maintain shelf life of the pack.
// Deckard in practice only reaches this modeled voltage after the SOC is < 0%
// and operation during usage is still controlled by UVP at 2.8 * 2 = 5.6 Volts
// When leaving DBR, this model is slow and seems to have
// a fixed time constant of 3.46 mV/sec. The implication of this time constant
// is that we must allow for the model to drop while PD negotiation is happening
// and/or the charger is toggled. This has been seen to take up to 10 seconds.
// A safety factor of 15 mV is also warranted from testing.
// aDSP (and Kernel) boot voltage is therefore UOCVP + 3.46 * 10 + 15 = 6610 mV
#define UOCVP (3280 * 2)
#define OCV_MARGIN 50
#define OCV_BOOT_VOLTAGE (UOCVP + OCV_MARGIN)

void deckard_battery_prep_for_boot(struct udevice *pmk8550, struct udevice *pm8550b, struct udevice *max17320);
bool deckard_battery_is_charging_enabled(struct udevice *pm8550b);
int deckard_battery_ready_to_boot_to_userspace( struct udevice *max17320 );

int deckard_spl_battery_enable_charging(struct udevice *pm8550b);
int deckard_spl_battery_charging_setup(uint16_t * battery_voltage);

int deckard_battery_get_capacity(struct udevice *max17320);
int deckard_battery_get_vfocv(struct udevice *max17320);
int deckard_battery_get_avg_vcell(struct udevice *max17320);
int deckard_battery_get_avg_current(struct udevice *max17320);

#endif /* __DECKARD_BATTERY_H */
