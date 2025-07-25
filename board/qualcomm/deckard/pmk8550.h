/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PMK8550 Interface
 *
 */

#ifndef __PMK8550_H
#define __PMK8550_H

#include <stdint.h>
#include <stdbool.h>

#include <power/pmic.h>

enum pon_reset_type {
	PON_RESET_TYPE_UNKNOWN = 0,
	PON_RESET_TYPE_PON,
	PON_RESET_TYPE_WARM,
	PON_RESET_TYPE_SHUTDOWN,
	PON_RESET_TYPE_HARD_RESET,
	PON_RESET_TYPE_COUNT
};

enum pon_reset_trigger {
	PON_RESET_TRIGGER_UNKNOWN = 0,
	PON_RESET_TRIGGER_SOFT,		/* Software trigger */
	PON_RESET_TRIGGER_PS_HOLD,	/* PS_HOLD trigger */
	PON_RESET_TRIGGER_PMIC_WD,	/* PMIC Watchdog trigger */
	PON_RESET_TRIGGER_KPDPWR_AND_RESIN,	 /* KPDPWR_N and RESIN_N trigger */
	PON_RESET_TRIGGER_RESIN,	/* RESIN_N trigger */
	PON_RESET_TRIGGER_KPDPWR,	/* KPDPWR_N trigger */
	PON_RESET_TRIGGER_OTHER,	/* TBD */
	PON_RESET_TRIGGER_COUNT
};

struct pon_on_off_reason {
	enum pon_reset_type type;
	enum pon_reset_trigger trigger;
	struct {
		uint32_t sid;                /* slave id */
		uint32_t pid;                /* peripheral id */
		uint32_t irq;                /* interrupt */
	} trigger_data;
};

const char *pmk8550_get_pon_trigger_str(enum pon_reset_trigger trigger);
const char *pmk8550_get_pon_type_str(enum pon_reset_type type);
int pmk8550_get_on_off_reason(struct pon_on_off_reason *reason);

int pmk8550_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv);

int pmk8550_get_pon_reason(struct udevice *pmic);

#endif /* __PMK8550_H */