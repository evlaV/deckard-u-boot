/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PM8550B Interface
 *
 */

#ifndef __PM8550B_H
#define __PM8550B_H

#include <stdint.h>
#include <stdbool.h>

#include <power/pmic.h>

#if 0
// NOTE These are 1s currents
enum pre_charge_current
{
	IPRE_100MA,
	IPRE_150MA,
	IPRE_200MA,
	IPRE_250MA,
	// note: PM8550B has 100mA step size above 300mA
	IPRE_300MA,
	IPRE_400MA,
	IPRE_500MA,
	IPRE_600MA,
};
#endif

int pm8550b_enable_fuel_guage(struct udevice *pmic, bool enable);
int pm8550b_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv);

#endif /* __PM8550B_H */