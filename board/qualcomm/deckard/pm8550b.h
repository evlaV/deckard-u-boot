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
int pm8550b_get_pmic_die_temp(struct udevice *pmic, int *temp_mc);
int pm8550b_get_vref_1p25(struct udevice *pmic, int *uv, uint16_t *raw_code);

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

int pm8550b_get_board_therm_temp(struct udevice *pmic, enum pm8550b_board_therm therm, int *temp_mc,
				 uint16_t *raw_code);

/**
 * struct pm8550b_temp_status - charger temperature protection and input status
 * @temp_range: MISC_TEMP_RANGE_STATUS (0x2C08), see PM8550B_TEMP_RANGE_*
 * @die_temp: MISC_DIE_TEMP_STATUS (0x2C09), see PM8550B_DIE_TEMP_*
 * @die_temp_shdn_cfg: MISC_DIE_TEMP_SHDN_CFG (0x2C61), reset and shutdown thresholds
 * @die_temp_ch_id: MISC_DIE_TEMP_CH_ID (0x2CA2), ADC channel the protection uses (0x10 CHG_TEMP)
 * @chg_rt: CHGR_CHG_RT_STS (0x26B0), see PM8550B_CHG_STS_*
 * @chg_lt: CHGR_CHG_LT_STS (0x26B1), latched copy of @chg_rt
 * @usb_rt: CHGR_USB_RT_STS (0x26B2), see PM8550B_USB_STS_*
 * @usb_lt: CHGR_USB_LT_STS (0x26B3), latched copy of @usb_rt
 * @temp_rt: CHGR_TEMP_RT_STS (0x26B8), see PM8550B_TEMP_STS_*
 * @temp_lt: CHGR_TEMP_LT_STS (0x26B9), latched copy of @temp_rt
 */
struct pm8550b_temp_status {
	uint8_t temp_range;
	uint8_t die_temp;
	uint8_t die_temp_shdn_cfg;
	uint8_t die_temp_ch_id;
	uint8_t chg_rt;
	uint8_t chg_lt;
	uint8_t usb_rt;
	uint8_t usb_lt;
	uint8_t temp_rt;
	uint8_t temp_lt;
};

#define PM8550B_TEMP_RANGE_ALERT_LEVEL      0x10 // above the reset threshold
#define PM8550B_TEMP_RANGE_TLIM             0x20 // above the shutdown threshold
#define PM8550B_TEMP_RANGE_THERM_REG_ACTIVE 0x40 // thermal regulation lowering ICL
#define PM8550B_DIE_TEMP_RST                0x04
#define PM8550B_DIE_TEMP_SHDN               0x08
#define PM8550B_CHG_STS_INPUT_OK            0x01
#define PM8550B_CHG_STS_BUCK_OK             0x02
#define PM8550B_CHG_STS_TEMP_OK             0x04
#define PM8550B_USB_STS_UV                  0x01
#define PM8550B_USB_STS_OV                  0x02
#define PM8550B_USB_STS_SUSPEND             0x04
#define PM8550B_TEMP_STS_CHG_TEMP_SHDN      0x01
#define PM8550B_TEMP_STS_USB_THERM_SHDN     0x08

int pm8550b_get_temp_status(struct udevice *pmic, struct pm8550b_temp_status *sts);
#endif /* __PM8550B_H */
