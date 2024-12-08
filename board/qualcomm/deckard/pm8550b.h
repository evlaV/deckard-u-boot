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

int pm8550b_enable_fuel_guage(struct udevice *pmic, bool enable);
int pm8550b_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv);

int pm8550b_set_fast_charge_current(struct udevice *pmic, uint16_t current_ma);
int pm8550b_set_float_voltage(struct udevice *pmic, uint16_t voltage_mv);
int pm8550b_set_termination_current(struct udevice *pmic, uint16_t current_ma);
int pm8550b_enable_watchdog(struct udevice *pmic, bool enable);
int pm8550b_enable_charging(struct udevice *pmic, bool enable);

#endif /* __PM8550B_H */