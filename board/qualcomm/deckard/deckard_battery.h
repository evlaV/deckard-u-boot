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

void deckard_battery_prep_for_boot(struct udevice *pmk8550, struct udevice *pm8550b);

int deckard_spl_battery_enable_charging(struct udevice *pm8550b);
int deckard_spl_battery_charging_setup(void);

#endif /* __DECKARD_BATTERY_H */