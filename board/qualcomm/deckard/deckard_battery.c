#include <fs.h>
#include <command.h>
#include <dm.h>
#include <i2c.h>
#include <linux/delay.h>
#include <led.h>

#include "pm8550b.h"
#include "pmk8550.h"
#include "smb1510.h"
#include <power/max17320_fg.h>
#include <power/northgate_hotswap.h>
#include <sysreset.h>

#include "deckard.h"
#include "deckard_battery.h"
#include "deckard_log.h"

#define BATTERY_MIN_BOOT_VOLTAGE 7200
#define BATTERY_DEAD_VOLTAGE     6000

#define BOULDER_SWITCH_SETTLE_MS    100

typedef enum
{
    BATTERY_FG_READ_ERROR,
    BATTERY_OK_TO_BOOT,
    BATTERY_NOT_OK_TO_BOOT,
} battery_state_t;

/**
 * sign_extend32 - sign extend a 32-bit value using specified bit as sign-bit
 * @value: value to sign extend
 * @index: 0 based bit index (0<=index<32) to sign bit
 *
 * This is safe to use for 16- and 8-bit types as well.
 */
static __always_inline __s32 sign_extend32(__u32 value, int index)
{
	__u8 shift = 31 - index;
	return (__s32)(value << shift) >> shift;
}

static void deckard_battery_enable_charging(struct udevice *pm8550b);

#if 0
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
	#define LOADADDRMAXLEN 20

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

	deckard_log("loading dead battery program with args: %s, %s, %s, %s, %s\n",argv[0],argv[1],argv[2],argv[3],argv[4]);
	return do_load(NULL, 0, argc, argv, FS_TYPE_EROFS);
}

static int deckard_battery_run_recovery_program(void)
{
	int   argc    = 1;
	char *argv[1] = {(char *)&s_pointer_table};

	return do_go_exec((void *)CONFIG_SYS_LOAD_ADDR, argc, argv);
}
#endif

void deckard_battery_prep_for_boot(struct udevice *pmk8550, struct udevice *pm8550b, struct udevice *max17320)
{
	int             ret;
	uint16_t        battery_mv;
	// int				soc;
	// struct udevice *led_red = NULL;
	// struct udevice *led_green = NULL;
	// struct udevice *led_blue = NULL;

	/* Get Battery Voltage to Check for UVLO */
	ret = pmk8550_get_battery_voltage(pmk8550, &battery_mv);
	if (ret < 0) {
		deckard_log("Failed to read battery voltage\n");
		deckard_log("Attempting to boot anyway!\n");
		goto complete;
	}

	deckard_log("Battery: %d mV\n", battery_mv);
#if 0
	soc = deckard_battery_get_capacity(max17320);
	if ( soc >= 0 ) {
		deckard_log("Battery: %d %%\n", soc);
		if (soc >= BATTERY_MIN_BOOT_SOC) {
			// OK to boot
			// Enable Charging
			goto complete;
		}
	} else {
		// fall back to voltage if capacity read fails
		if (battery_mv >= BATTERY_MIN_BOOT_VOLTAGE) {
			// OK to boot
			// Enable Charging
			goto complete;
		}
	}

	// Check for UVLO (or charging and (trickle or precharge))
	ret = deckard_battery_load_recovery_program();
	if (ret != 0) {
		deckard_log("Dead Battery Recovery load failed on error: %d\n", ret);
		deckard_log("Attempting to boot anyway!\n");
		goto complete;
	}

	s_pointer_table.pmic_pmk8550 = pmk8550;
	s_pointer_table.pmic_pm8550b = pm8550b;

	ret = deckard_battery_run_recovery_program();
	if (ret != 0) {
		deckard_log("Dead Battery Recovery Exec Failed on Error: %d\n", ret);
		deckard_log("Attempting to boot anyway!\n");
		goto complete;
	}
#endif

complete:
	// Enable Charging
	deckard_battery_enable_charging(pm8550b);
}

static void deckard_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 1,200 mA (QPH1 path limit - exp. tested) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 1000);

	/* Set Max Fast Charge Current to 1,200 mA (QPH1 path limit - exp. tested)) */
	ret |= pm8550b_set_max_fast_charge_current(pm8550b, 1000);

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

	deckard_log("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");

	//pm8550b_test(pm8550b, true);
}

int deckard_spl_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 1,200 mA (QPH1 path limit - exp. tested) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 1000);

	/* Set Max Fast Charge Current to 1,200 mA (QPH1 path limit - exp. tested)) */
	ret |= pm8550b_set_max_fast_charge_current(pm8550b, 1000);

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

	deckard_log("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");

	return ret;
}

bool deckard_battery_is_charging_enabled(struct udevice *pm8550b)
{
	return pm8550b_is_charging_enabled(pm8550b);
}

static battery_state_t deckard_get_battery_state( struct udevice *max17320, bool check_soc )
{
    if ( !max17320 )
    {
        return BATTERY_FG_READ_ERROR;
    }

	int ocv = deckard_battery_get_vfocv( max17320 );
	int soc = check_soc ? deckard_battery_get_capacity( max17320 ) : BATTERY_MIN_BOOT_SOC;

	if ( ocv < 0 || soc < 0 )
		return BATTERY_FG_READ_ERROR;

	return ( soc >= BATTERY_MIN_BOOT_SOC && ocv > OCV_BOOT_VOLTAGE ) ? BATTERY_OK_TO_BOOT : BATTERY_NOT_OK_TO_BOOT;
}

/*
 * The boot decision follows the battery that is actually powering the system.
 *
 * Boulder's battery selection is left alone unless the active battery can't boot the system
 * and the other one can, in which case the system is switched over.
 *
 * Similarly, the charger will only charge the active battery as well, no cross charging
 * 
 *  > No external pack, internal OK                            -  Boot
 *  > No external pack, internal gauge missing or unreadable   -  Boot (fail open)
 *  > No external pack, internal too low                       -  Don’t boot
 *  > On internal, internal gauge error, external OK           -  Switch to external; boot, a refused switch falls back to fail-open
 *  > On internal, too low, external OK                        -  Switch to external; boot only if the switch took
 *  > On external, too low, internal OK                        -  Switch to internal; boot only if the switch took
 *  > On external, too low, internal gauge error               -  Don’t boot
 *  > External gauge present, active battery unknown           -  Boot if either battery is known OK
*/
static bool deckard_ok_for_boot( deckard_devices_t *p_devices, bool check_soc )
{
    struct udevice *max17320_primary = p_devices->max17320;
    struct udevice *max17320_external = p_devices->max17320_external;
    struct udevice *northgate = p_devices->boulder_northgate;
    struct udevice *pm8550b = p_devices->pm8550b;
    
	// Check existing OK states
	battery_state_t internal_state = deckard_get_battery_state( max17320_primary, check_soc );
	battery_state_t external_state = deckard_get_battery_state( max17320_external, check_soc );

	// Identify which battery is powering the system
	battery_t active_battery = BATTERY_UNKNOWN;
	if ( northgate )
		active_battery = northgate_hotswap_get_active_battery( northgate );
	else if ( !max17320_external )
		active_battery = BATTERY_INTERNAL;  // If there's no northgate and no external fuel gauge, assume internal

	// See if the system needs to switch batteries
	battery_t battery_to_switch_to = BATTERY_NONE;
	if ( ( active_battery == BATTERY_INTERNAL ) && ( internal_state != BATTERY_OK_TO_BOOT ) && ( external_state == BATTERY_OK_TO_BOOT ) )
		battery_to_switch_to = BATTERY_EXTERNAL;

	if ( ( active_battery == BATTERY_EXTERNAL ) && ( internal_state == BATTERY_OK_TO_BOOT ) && ( external_state != BATTERY_OK_TO_BOOT ) )
		battery_to_switch_to = BATTERY_INTERNAL;

	if ( ( battery_to_switch_to != BATTERY_NONE ) && ( northgate ) )
	{
        // Switch batteries, pause charging across the switch
        if ( pm8550b_pause_charging( pm8550b, true ) )
        {
            deckard_log( "Boulder: failed to pause charging, not switching battery\n" );
            return -EIO;
        }
        else
        {
            mdelay( BOULDER_SWITCH_SETTLE_MS );
            int retv = northgate_hotswap_select_battery( northgate, battery_to_switch_to );
            pm8550b_pause_charging( pm8550b, false );

            deckard_log( "Boulder: switch to %s battery %s\n",
                         ( battery_to_switch_to == BATTERY_INTERNAL ) ? "internal" : "external",
                         ( retv ) ? "failed" : "done" );
        }        
        
		active_battery = northgate_hotswap_get_active_battery( northgate );
	}

	bool ok_to_boot = false;
	ok_to_boot |= ( active_battery == BATTERY_INTERNAL ) && ( ( internal_state == BATTERY_OK_TO_BOOT ) || ( internal_state == BATTERY_FG_READ_ERROR ) ); // Internal fg read error, default to booting
	ok_to_boot |= ( active_battery == BATTERY_EXTERNAL ) && ( external_state == BATTERY_OK_TO_BOOT );
	ok_to_boot |= ( active_battery == BATTERY_UNKNOWN ) && ( ( internal_state == BATTERY_OK_TO_BOOT ) || ( external_state == BATTERY_OK_TO_BOOT ) ); // Error case: external pack present but can't talk to northgate

	return ok_to_boot;
}

bool deckard_battery_ok_for_userspace( deckard_devices_t *p_devices )
{
	// state of charge and VF OCV of the active battery
	return deckard_ok_for_boot( p_devices, true );
}

bool deckard_battery_ok_for_dsp( deckard_devices_t *p_devices )
{
	// VF OCV must be above boot voltage to survive charging pause and PD Negotiation.
	// ADSP controls high charging mode. prior to ADSP being initialized, it's still
	// capable of charging in lower (typically 5V@1A) mode.
	return deckard_ok_for_boot( p_devices, false );
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
		deckard_log("Failed to Find PMK8650\n");
		return ret;
	}

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pm8550b);
	if (ret != 0) {
		deckard_log("Failed to Find PM8550B\n");
		return ret;
	}

	// Find the smb1510
	ret = smb1510_read_revid(pmk8550);
	if (ret != 0) {
		deckard_log("Failed to Find SMB1510\n");
		return ret;
	}

	/* Get Battery Voltage to Check for UVLO */
	ret = pmk8550_get_battery_voltage(pmk8550, &battery_mv);
	if (ret < 0) {
		deckard_log("Failed to read battery voltage\n");
		deckard_log("Attempting to boot anyway!\n");
		*battery_voltage = SPL_BATTERY_MIN_BOOT_VOLTAGE;
		return ret;
	}

	*battery_voltage = battery_mv;
	deckard_log("Battery: %d mV\n", battery_mv);

	// Enable Charging
	deckard_spl_battery_enable_charging(pm8550b);

	return 0;
}

int deckard_battery_get_capacity(struct udevice *max17320)
{
	uint8_t reg[2];
	if ( pmic_read(max17320, REPSOC_REG, reg, 2) ) {
		deckard_log("failed to get battery capacity\n");
		return -EIO;
	}

	// RepSOC LSB: 1/256 %
	// return SOC >> 8;
	const int soc = reg[1];
	// const int soc_frac = (int)reg[0] * 10 / 256;
	// deckard_log("SOC: %02d.%1d\n", soc, soc_frac);

	return soc;
}

int deckard_battery_get_vfocv(struct udevice *max17320)
{
	uint8_t reg[2];
	if ( pmic_read(max17320, VFOCV_REG, reg, 2) ) {
		deckard_log("failed to get battery OCV\n");
		return -EIO;
	}

	// Scale to 2S, 0.078125 mV/lsb
	const int ocv = (int32_t)((((uint32_t)reg[1]) << 8) + reg[0])*2*7813/1000/100;
	// deckard_log("OCV: %d mV\n", ocv);

	return ocv;
}

int deckard_battery_get_avg_vcell(struct udevice *max17320)
{
	uint8_t reg[2];
	if ( pmic_read(max17320, AVGVCELL_REG, reg, 2) ) {
		deckard_log("failed to get battery Average Cell Voltage\n");
		return -EIO;
	}

	// Scale to 2S, 0.078125 mV/lsb
	const int avg_vcell = (int32_t)((((uint32_t)reg[1]) << 8) + reg[0])*2*7813/1000/100;
	// deckard_log("Avg. VCell: %d mV\n", avg_vcell);

	return avg_vcell;
}

int deckard_battery_get_avg_current(struct udevice *max17320)
{
	uint8_t reg[2];
	if ( pmic_read(max17320, AVGCURRENT_REG, reg, 2) ) {
		deckard_log("failed to get battery Average Current\n");
		return -EIO;
	}

	// resolution = 625 uA / lsb
	uint16_t raw_val = (((uint16_t)reg[1]) << 8) + reg[0];
	const int avg_current = sign_extend32(raw_val, 15) * 625 / 1000;
	// deckard_log("Avg. Current: %d mA\n", avg_current);

	return avg_current;
}


int deckard_battery_set_ship_mode(struct udevice *max17320, struct udevice *pm8550b)
{
	uint8_t reg[2];
	int ret = 0;
	int len;

	if(!max17320 || !pm8550b)
		return -EINVAL;

	len = 2;
	if ( pmic_read(max17320, CONFIG_REG, reg, len) ) {
		deckard_log("failed to get fg ship mode\n");
		return -EIO;
	}

	if ( pm8550b_pause_charging(pm8550b, true) ) {
		deckard_log("failed to pause charging\n");
		return -EIO;
	}

	reg[0] |= MAX17330_CONFIG_SHIP;
	if ( pmic_write(max17320, CONFIG_REG, reg, len) ) {
		deckard_log("failed to set fg ship mode\n");
		ret = -EIO;
		goto cleanup;
	}

	if ( pm8550b_set_ship_mode(pm8550b, true) ) {
		deckard_log("failed to set pm8550b ship mode\n");
		goto cleanup;
	}

	return 0;

cleanup:
	//unpause charging
	pm8550b_pause_charging(pm8550b, false);
	//take max17320 out of ship mode
	reg[0] &= ~MAX17330_CONFIG_SHIP;
	pmic_write(max17320, CONFIG_REG, reg, 2);
	return ret;
}