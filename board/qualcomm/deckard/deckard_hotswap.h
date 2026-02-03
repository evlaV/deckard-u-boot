/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Boulder Hotswap Interface for Deckard
 */

#ifndef __DECKARD_HOTSWAP_H
#define __DECKARD_HOTSWAP_H

#include <stdbool.h>

#include <power/pmic.h>

bool deckard_hotswap_supported(struct udevice *slg4ad48669m);

int deckard_hotswap_enable_charging(struct udevice *slg4ad48669m, bool enable,
                                    bool charge_internal);
bool deckard_hotswap_get_external_battery_installed(
    struct udevice *slg4ad48669m);
bool deckard_hotswap_on_external_battery(struct udevice *slg4ad48669m);
bool deckard_hotswap_charger_enabled(struct udevice *slg4ad48669m);

#endif /* __DECKARD_HOTSWAP_H */
