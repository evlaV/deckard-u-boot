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
// int pm8550b_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv);

int pm8550b_set_fast_charge_current(struct udevice *pmic, uint16_t current_ma);
int pm8550b_set_max_fast_charge_current(struct udevice *pmic, uint16_t current_ma);
int pm8550b_set_float_voltage(struct udevice *pmic, uint16_t voltage_mv);
int pm8550b_set_max_float_voltage(struct udevice *pmic, uint16_t voltage_mv);
int pm8550b_set_termination_current(struct udevice *pmic, uint16_t current_ma);
int pm8550b_enable_watchdog(struct udevice *pmic, bool enable);
bool pm8550b_is_charging_enabled(struct udevice *pmic);
int pm8550b_enable_charging(struct udevice *pmic, bool enable);
int pm8550b_test(struct udevice *pmic, bool enable);
int pm8550b_disable_typec_charging(struct udevice *pmic);
int pm8550b_pause_charging(struct udevice *pmic, bool pause);
int pm8550b_set_ship_mode(struct udevice *pmic, bool shipmode);
int pm8550b_get_die_temp(struct udevice *pmic, int *temp_mc);

/**
 * enum pm8550b_board_therm - board thermistors read through the PMK8550 ADC
 * @PM8550B_BOARD_THERM_40PIN_CONNECTOR: PM8550 AMUX_THM2, pcba_at_40pin_connector (SDM skin, RT507)
 * @PM8550B_BOARD_THERM_HEAT_SINK_FINS: PM8550B USB_THERM, pcba_at_heat_sink_fins
 * @PM8550B_BOARD_THERM_COUNT: number of thermistors
 */
enum pm8550b_board_therm {
	PM8550B_BOARD_THERM_40PIN_CONNECTOR,
	PM8550B_BOARD_THERM_HEAT_SINK_FINS,
	PM8550B_BOARD_THERM_COUNT,
};

int pm8550b_get_board_therm_temp(struct udevice *pmic, enum pm8550b_board_therm therm, int *temp_mc);
#endif /* __PM8550B_H */
