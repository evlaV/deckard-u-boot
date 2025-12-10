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

void deckard_battery_prep_for_boot(struct udevice *pmk8550, struct udevice *pm8550b, struct udevice *max17320);
bool deckard_battery_is_charging_enabled(struct udevice *pm8550b);

int deckard_spl_battery_enable_charging(struct udevice *pm8550b);
int deckard_spl_battery_charging_setup(uint16_t * battery_voltage);

int deckard_battery_get_capacity(struct udevice *max17320);
int deckard_battery_get_vfocv(struct udevice *max17320);
int deckard_battery_get_avg_vcell(struct udevice *max17320);

#endif /* __DECKARD_BATTERY_H */
