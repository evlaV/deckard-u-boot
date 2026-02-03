/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Deckard Boulder Interface
 */

#ifndef __DECKARD_BOULDER_H
#define __DECKARD_BOULDER_H

#include <stdbool.h>

#include <power/pmic.h>

enum deckard_boulder_battery_t {
	BOULDER_BATTERY_INTERNAL = 0,
	BOULDER_BATTERY_EXTERNAL = 1,
};

bool deckard_boulder_detected(struct udevice *slg4ad48669m);
bool deckard_boulder_enable_charging(struct udevice *slg4ad48669m, enum deckard_boulder_battery_t target_battery);
bool deckard_boulder_disable_charging(struct udevice *slg4ad48669m, enum deckard_boulder_battery_t target_battery);
bool deckard_boulder_external_battery_is_installed(struct udevice *slg4ad48669m);
bool deckard_boulder_on_external_battery(struct udevice *slg4ad48669m);
bool deckard_boulder_charger_enabled(struct udevice *slg4ad48669m);
#endif /* __DECKARD_BOULDER_H */
