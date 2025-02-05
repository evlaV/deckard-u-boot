// SPDX-License-Identifier: GPL-2.0+
/*
 * Board init file for Dragonboard 410C
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 */

#include <common.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/pinctrl.h>
#include <env.h>
#include <init.h>
#include <mmc.h>
#include <net.h>
#include <usb.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/gpio.h>
#include <fdt_support.h>
#include <linux/delay.h>
#include <led.h>
#include <button.h>
#include <power/pmic.h>
#include <command.h>

#include "deckard_eeprom.h"
#include "deckard_battery.h"
#include "pm8550vs.h"
#include "smb1510.h"
#include <part.h>
#include "deckard.h"

DECLARE_GLOBAL_DATA_PTR;

static ulong boot_wrapper_version __section(".data");

int g_boot_wrapper_version;
/**
 * Save x0 register value, which should encode the boot wrapper version
 */
void save_boot_params(ulong r0)
{
	boot_wrapper_version = r0;
	save_boot_params_ret();
}

// #define CHECK_PMICS
#ifdef CHECK_PMICS
void check_pmic(const char *const pmic_dts, const char *const pmic_name)
{
	int             ret;
	struct udevice *pmic = NULL;

	ret = pmic_get(pmic_dts, &pmic);
	if (ret != 0) {
		printf("Failed to Find %s - %s, ret: %d\n", pmic_dts, pmic_name, ret);
		return;
	}

	ret = pmic_reg_read(pmic, 0x0105);
	if (ret < 0) {
		printf("%s failed: %d\n", pmic_name, ret);
	}
	// else
	// {
	// 	printf("%s subtype: 0x%02X\n", pmic_name, ret);
	// }
}

void check_pmics_comms(void)
{
	check_pmic("pmic@0", "PMK8550");
	check_pmic("pmic@1", "PM8550");
	check_pmic("pmic@2", "PM8550VS_c");
	check_pmic("pmic@3", "PM8550VS_d");
	check_pmic("pmic@4", "PM8550VS_e");
	check_pmic("pmic@6", "PM8550VS_g");
	check_pmic("pmic@7", "PM8550BHS");
	check_pmic("pmic@8", "PM8550VE");
	check_pmic("pmic@c", "PM8010_m");
	check_pmic("pmic@d", "PM8010_n");
}
#endif

static inline void write32(unsigned long addr, u32 value)
{
	*(volatile u32 *) addr = value;
}

static inline u32 read32(unsigned long addr)
{
	return *(volatile u32 *) addr;
}

// see BOOT.MXF.2.1/boot_images/boot/QcomPkg/SocPkg/Lanai/Include/HALbootHWIO.h
// for watchdog register defines
#define HWIO_APSS_WDT_TMR2_WDOG_CTL_ADDR  0x17411008
static void deckard_disable_watchdog(void)
{
	// disable watchdog 2
	write32(HWIO_APSS_WDT_TMR2_WDOG_CTL_ADDR, 0);
}

static void deckard_setup(void)
{
	int             ret;
	struct udevice *pmk8550;
	struct udevice *pm8550vse;
	struct udevice *pm8550b;

#if CHECK_PMICS
	check_pmics_comms();
#endif

	// Find the PMK8650
	ret = pmic_get("pmic@0", &pmk8550);
	if (ret != 0) {
		printf("Failed to Find PMK8650\n");
		return;
	}

	// Find the PM8550VS_e
	ret = pmic_get("pmic@4", &pm8550vse);
	if (ret != 0) {
		printf("Failed to Find PM8550VS_e\n");
		return;
	}

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pm8550b);
	if (ret != 0) {
		printf("Failed to Find PM8550B\n");
		return;
	}

	smb1510_check_subtype(pmk8550);

	/* Fix GPIO Levels that XBL continues to not let us */
	pm8550vs_set_gpio_vin_level(pm8550vse, PM8550VS_GPIO_3, PM8550VS_GPIO_VIN_1);
	pm8550vs_set_gpio_vin_level(pm8550vse, PM8550VS_GPIO_4, PM8550VS_GPIO_VIN_1);

	/* Ensure Battery is OK to proceed */
	deckard_battery_prep_for_boot(pmk8550, pm8550b);

	printf("smb1510: enable fixed frequency at 500khz mode\n");
	smb1510_set_switching_frequency(pmk8550, 0x0F);
}

#define DECKARD_SYSTEM_LUNIDX "0"
#define DECKARD_UBOOT_LUNIDX "2"
#define DECKARD_RESCUE_LUNIDX "3"
//M slot is the master slot / default
#define DECKARD_UBOOT_ENV_SLOT_M "2:5"
#define DECKARD_UBOOT_ENV_SLOT_A "2:2"
#define DECKARD_UBOOT_ENV_SLOT_B "2:4"
#define DECKARD_UBOOT_ENV_SLOT_R "3:2"

//returns an index that env_ext4_get_dev_part() can use (overrides weak ref func ptr)
//and you can also get the 
int deckard_get_boot(char * bootedslotref)
{
	struct blk_desc *desc;
	int ret;

	*bootedslotref = 'X';
	ret = blk_get_device_by_str("scsi", DECKARD_UBOOT_LUNIDX, &desc);
	if( ret >= 0 )
	{
		switch(part_get_booted_slot(desc))
		{
			case 'A':
				*bootedslotref = 'A';
				return 1;
			case 'B':
				*bootedslotref = 'B';
				return 2;
			case 'R':
				*bootedslotref = 'R';
				return 3;
			default:
				return 0;
		}
	}
	else {
		return 0;
	}
}

//this overrides the built-in default function
const char *env_ext4_get_dev_part(void)
{
	static char *const dev_part[] = { 
										DECKARD_UBOOT_ENV_SLOT_M, 
										DECKARD_UBOOT_ENV_SLOT_A, 
										DECKARD_UBOOT_ENV_SLOT_B,
										DECKARD_UBOOT_ENV_SLOT_R
									};
	char bootedslot;
	int devpartidx = deckard_get_boot(&bootedslot);
	return dev_part[devpartidx];
}

const char *deckard_env_ext4_get_dev_part(void)
{
	return env_ext4_get_dev_part();
}

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice     *btn, *led;
	int                 ret;
	enum button_state_t state;
	struct blk_desc *desc;
	char * bootedslot;
	char * bootorder;
	//char * rebootneeded;
	int reboot_needed = 0;
	char bootedslotquery;
	int slotdiffers = 0;
	char * fastboot_envvar;
	int fastbootmode = 0;

	deckard_disable_watchdog();

	g_boot_wrapper_version = boot_wrapper_version;
	printf("Boot Wrapper version: %d\n", g_boot_wrapper_version);

	deckard_setup();

	//////////////////////////////////////////
	//1. check vars that are set before calling any set_env()
	//2. clear those variables by env_set(...,"")
	//3. then env_save() if the value is set

	fastboot_envvar = env_get("fastboot");
	if( fastboot_envvar && ( fastboot_envvar[0] == '1' ) ) 
	{
		//clearing fastboot var to avoid getting trapped in fastboot mode
		env_set("fastboot","");
		env_save();
		fastbootmode = 1;
	}
	//printf("done checking for auto-clear variables")
	//////////////////////////////////////////

	ret = button_get_by_label("vol_down", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol- button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol- button pressed\n");
		}
	}

	ret = button_get_by_label("Volume Up", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol+ button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol+ button pressed\n");

			env_set("stdin","usbacm");
			env_set("stderr","usbacm");
			env_set("stdout","usbacm");
			env_set("bootdelay","20");
		}
		else
		{
			env_set("bootdelay","2");
		}
	}
	
	ret = button_get_by_label("pwrkey", &btn);
	if (ret < 0) {
		printf("Couldn't find power button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("PWR button pressed\n");
		}
	}

	ret = led_get_by_label("blue", &led);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
		led_set_state(led, LEDST_ON);
	};

	// get board data from eeprom
	deckard_eeprom_init();

	ret = blk_get_device_by_str("scsi", DECKARD_UBOOT_LUNIDX, &desc);
	if( ret >= 0 )
	{
		//part_print_fstype_uuid(desc); // old way

		deckard_get_boot(&bootedslotquery);
		switch(bootedslotquery)
		{
			case 'A':
				env_set("booted_slot","A");
				break;
			case 'B':
				env_set("booted_slot","B");
				break;
			case 'R':
				env_set("booted_slot","R");
				break;				
			default:
				env_set("booted_slot","A");
				break;		
		}
	}
	else
	{
		printf("blk_get_device_by_str failed, ret = %d\n",ret);
		env_set("booted_slot","A");
	}

	env_set("UBOOTLUNIDX",DECKARD_UBOOT_LUNIDX);
	env_set("SYSTEMLUNIDX",DECKARD_SYSTEM_LUNIDX);
	env_set("RESCUELUNIDX",DECKARD_RESCUE_LUNIDX);

	bootorder = env_get("BOOT_ORDER");
	bootedslot = env_get("booted_slot");

	if(!bootorder)
	{
		//BOOT_ORDER is unset, so set to a default value
		printf("BOOT_ORDER was unset, setting to A\n");
		env_set("BOOT_ORDER","A");
		bootorder = env_get("BOOT_ORDER");
		env_save();
	}

	//compare first character of these env vars to check for A or B
	if( bootorder && bootedslot && ( bootorder[0] == bootedslot[0] ) ) 
	{
		//printf("booted_slot matches BOOT_ORDER\n");
	}
	else
	{
		//printf("booted_slot mismatch BOOT_ORDER\n");
		if(!strncmp( env_get("booted_slot"),"A",1))
		{
			gpt_set_xbl_partition_bootable_partfstype(desc,"ubootb");
		}
		else
		{
			gpt_set_xbl_partition_bootable_partfstype(desc,"uboot");
		}
		//env_set("rebootneeded","1");
		reboot_needed = 1;
		slotdiffers = 1;
	}

	printf("booted_slot: %s %s\n", env_get("booted_slot"), (slotdiffers ? "(changed)" : "") );

	if(reboot_needed) 
	{
		//this will make it so we get a countdown, it will fail to boot, fall back to altbootcmd, and reset
		printf("reboot needed\n");
		env_set_ulong("bootcount", 2);
		env_set_ulong("bootlimit", 1);
		env_set_ulong("upgrade_available", 1); //the bootcount_env stuff uses this as the hook to check the bootlimit
		env_set("altbootcmd","reset");
		//we could do a reset herewith do_reset(), but that would skip the user's ability to interrupt the reboot, which is not good
	}
	else
	{
		env_set_ulong("bootcount", 1);
		env_set_ulong("bootlimit", 10);
		env_set_ulong("upgrade_available", 0); 
		env_set("altbootcmd","");
	}

	//compare first character of these env vars to check for A or B
	if( fastbootmode ) 
	{
		printf("Starting fastboot mode...\n");
		env_set("kernel_addr_r","90000000");
		env_set("serial#","123456789"); //put dummy serial number here used by fastboot
		run_command("fastboot -l 0x880000000 -s 0x10000000 usb 0", 0);
	}

	printf("deckard init: done\n");
///////

	return 0;
}
