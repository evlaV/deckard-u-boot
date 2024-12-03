
#include "pm8550b.h"
#include "smb1510.h"

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

	printf("Battery: %d mV\n", battery_mv);
}

void deckard_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 2040);

	/* Set Float Voltage to 8,900 mV */
	ret |= pm8550b_set_float_voltage(pm8550b, 8900);

	/* Set Termination Current to 150 mA */
	ret |= pm8550b_set_termination_current(pm8550b, 150);

	/* Ensure Charging Watchdog is Off */
	ret |= pm8550b_enable_watchdog(pm8550b, false);

	/* Enable Charging */
	ret |= pm8550b_enable_charging(pm8550b, true);

	printf("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");
}
