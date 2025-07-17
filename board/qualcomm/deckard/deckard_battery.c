
#include <fs.h>
#include <command.h>
#include <linux/delay.h>
#include <led.h>

#include "pm8550b.h"
#include "pmk8550.h"
#include "smb1510.h"
#include "deckard.h"
#include "deckard_battery.h"

#define BATTERY_MIN_BOOT_VOLTAGE 7200
#define BATTERY_DEAD_VOLTAGE     6000

static void deckard_battery_enable_charging(struct udevice *pm8550b);

typedef int (*printf_func_t)(const char *, ...);
typedef void (*udelay_func_t)(unsigned long);
typedef int (*pmic_write_t)(struct udevice *, uint reg, uint value);
typedef int (*pmic_masked_write_t)(struct udevice *, uint reg, uint mask, uint value);
typedef int (*pmic_read_t)(struct udevice *, uint reg);

/// @brief  dead battery recovery pointer table that is shared with dead battery recovery program.
///         Hopefully it is obvious that they must match
typedef struct {
	printf_func_t       printf_func;
	udelay_func_t       udelay_func;
	struct udevice     *pmic_pmk8550;
	struct udevice     *pmic_pm8550b;
	pmic_write_t        spmi_write_func;
	pmic_masked_write_t spmi_masked_write_func;
	pmic_read_t         spmi_read_func;
	pmic_write_t        smb1510_write_func;
	pmic_masked_write_t smb1510_masked_write_func;
	pmic_read_t         smb1510_read_func;

} dbr_pointer_table_t;

static dbr_pointer_table_t s_pointer_table = {
    .printf_func               = printf,
    .udelay_func               = udelay,
    .pmic_pmk8550              = NULL,
    .pmic_pm8550b              = NULL,
    .spmi_write_func           = pmic_reg_write,
    .spmi_masked_write_func    = pmic_clrsetbits,
    .spmi_read_func            = pmic_reg_read,
    .smb1510_write_func        = smb1510_write,
    .smb1510_masked_write_func = smb1510_masked_write,
    .smb1510_read_func         = smb1510_read,
};

static int deckard_battery_load_recovery_program(void)
{
	#define DEVPARTMAXLEN 16
	#define LOADADDRMAXLEN 16

	int  argc = 5;
	char *argv[5];
	char arg0[] = "do_the_dew";
	char arg1[] = "scsi";
	char arg2[DEVPARTMAXLEN];  //e.g. "1:d"
	char arg3[LOADADDRMAXLEN]; //e.g. "0xa7000000"
	char arg4[] = "dead_battery_recovery.bin";

	strncpy(arg2, deckard_get_fw_part(), DEVPARTMAXLEN); //this returns 1:d for example
	snprintf(arg3, LOADADDRMAXLEN, "0x%x", CONFIG_SYS_LOAD_ADDR); //this returns 1:d for example

	argv[0]=arg0;
	argv[1]=arg1;
	argv[2]=arg2;
	argv[3]=arg3;
	argv[4]=arg4;

	printf("loading dead battery program with args: %s, %s, %s, %s, %s\n",argv[0],argv[1],argv[2],argv[3],argv[4]);
	return do_load(NULL, 0, argc, argv, FS_TYPE_EROFS);
}

static int deckard_battery_run_recovery_program(void)
{
	int   argc    = 1;
	char *argv[1] = {(char *)&s_pointer_table};

	return do_go_exec((void *)CONFIG_SYS_LOAD_ADDR, argc, argv);
}

void deckard_battery_prep_for_boot(struct udevice *pmk8550, struct udevice *pm8550b)
{
	int             ret;
	uint16_t        battery_mv;
	struct udevice *led = NULL;

	/* Get Battery Voltage to Check for UVLO */
	ret = pmk8550_get_battery_voltage(pmk8550, &battery_mv);
	if (ret < 0) {
		printf("Failed to read battery voltage\n");
		printf("Attempting to boot anyway!\n");
		return;
	}

	printf("Battery: %d mV\n", battery_mv);

	if (battery_mv >= BATTERY_MIN_BOOT_VOLTAGE) {
		// OK to boot

		// Enable Charging
		deckard_battery_enable_charging(pm8550b);
		return;
	}

	bool battery_was_dead;

	ret = led_get_by_label("red", &led);
	if (ret < 0) {
		printf("Couldn't find red led!\n");
	}

	if (led) {
		printf("Enabling Red LED\n");
		led_set_state(led, LEDST_ON);
	}

	if (battery_mv < BATTERY_DEAD_VOLTAGE) {
		// Fast Pulse Red LED
		// TODO
		battery_was_dead = true;
	} else {
		// Slow Pulse Red LED
		// TODO
		battery_was_dead = false;
	}

	// Check for UVLO (or charging and (trickle or precharge))
	ret = deckard_battery_load_recovery_program();
	if (ret != 0) {
		printf("Dead Battery Recovery load failed on error: %d\n", ret);
		printf("Attempting to boot anyway!\n");
		goto complete;
	}

	s_pointer_table.pmic_pmk8550 = pmk8550;
	s_pointer_table.pmic_pm8550b = pm8550b;

	ret = deckard_battery_run_recovery_program();
	if (ret != 0) {
		printf("Dead Battery Recovery Exec Failed on Error: %d\n", ret);
		printf("Attempting to boot anyway!\n");
		goto complete;
	}

	// Check for <2%
	if (battery_was_dead == true) {
		// Slow Pulse Red LED
		// TODO
	}

	// Enable Charging
	deckard_battery_enable_charging(pm8550b);

	// Charge to 2% here... (by voltage)
	do {
		ret = pmk8550_get_battery_voltage(pmk8550, &battery_mv);
		if (ret < 0) {
			printf("Failed to read battery voltage\n");
			printf("Attempting to boot anyway!\n");
			return;
		}

		mdelay(1000);
		printf("Battery: %d mV\n", battery_mv);

	} while (battery_mv < BATTERY_MIN_BOOT_VOLTAGE);

complete:

	if (led) {
		printf("Disabling Red LED\n");
		led_set_state(led, LEDST_OFF);
	}
}

static void deckard_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 2040);

	/* Set Max Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_max_fast_charge_current(pm8550b, 2040);

	/* Set Float Voltage to 8,900 mV */
	ret |= pm8550b_set_float_voltage(pm8550b, 8900);

	/* Set Max Float Voltage to 8,900 mV */
	ret |= pm8550b_set_max_float_voltage(pm8550b, 8900);

	/* Set Termination Current to 150 mA */
	ret |= pm8550b_set_termination_current(pm8550b, 150);

	/* Ensure Charging Watchdog is Off */
	ret |= pm8550b_enable_watchdog(pm8550b, false);

	/* Enable Charging */
	ret |= pm8550b_enable_charging(pm8550b, true);

	printf("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");

	//pm8550b_test(pm8550b, true);
}

int deckard_spl_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 2040);

	/* Set Max Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_max_fast_charge_current(pm8550b, 2040);

	/* Set Float Voltage to 8,900 mV */
	ret |= pm8550b_set_float_voltage(pm8550b, 8900);

	/* Set Max Float Voltage to 8,900 mV */
	ret |= pm8550b_set_max_float_voltage(pm8550b, 8900);

	/* Set Termination Current to 150 mA */
	ret |= pm8550b_set_termination_current(pm8550b, 150);

	/* Ensure Charging Watchdog is Off */
	ret |= pm8550b_enable_watchdog(pm8550b, false);

	/* Enable Charging */
	ret |= pm8550b_enable_charging(pm8550b, true);

	printf("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");

	return ret;
}

int deckard_spl_battery_charging_setup(uint16_t * battery_voltage)
{
	int             ret;
	struct udevice *pmk8550;
	struct udevice *pm8550b;
	uint16_t		battery_mv;

	// Find the PMK8650
	ret = pmic_get("pmic@0", &pmk8550);
	if (ret != 0) {
		printf("Failed to Find PMK8650\n");
		return ret;
	}

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pm8550b);
	if (ret != 0) {
		printf("Failed to Find PM8550B\n");
		return ret;
	}

	// Find the smb1510
	ret = smb1510_read_revid(pmk8550);
	if (ret != 0) {
		printf("Failed to Find SMB1510\n");
		return ret;
	}

	/* Get Battery Voltage to Check for UVLO */
	ret = pmk8550_get_battery_voltage(pmk8550, &battery_mv);
	if (ret < 0) {
		printf("Failed to read battery voltage\n");
		printf("Attempting to boot anyway!\n");
		*battery_voltage = SPL_BATTERY_MIN_BOOT_VOLTAGE;
		return ret;
	}

	*battery_voltage = battery_mv;
	printf("Battery: %d mV\n", battery_mv);

	// Enable Charging
	deckard_spl_battery_enable_charging(pm8550b);

	return 0;
}