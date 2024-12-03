
#include "pm8550b.h"

#include <linux/delay.h>

#define BATT_2S_UV_OC_PROT_TH_4P4V 4400

void deckard_battery_recover_uvlo(struct udevice *pmk8550, struct udevice *pm8550b)
{
	int      ret;
	uint16_t battery_mv;

	/* Ensure Qualcomm Fuel Gauge is enabled */
	pm8550b_enable_fuel_guage(pm8550b, true);

	/* Get Battery Voltage to Check for UVLO */
	ret = pm8550b_get_battery_voltage(pm8550b, &battery_mv);
	if (ret < 0)
		return;

	printf("Battery Voltage: %d\n", battery_mv);
}

void deckard_battery_enable_charging(struct udevice *pm8550b)
{
	return;
}
