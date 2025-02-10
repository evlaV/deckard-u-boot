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
#include <fastboot.h>
#include <init.h>
#include <mmc.h>
#include <net.h>
#include <usb.h>
#include <asm/cache.h>
#include <sysreset.h>
#include <asm/io.h>
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

#define DECKARD_DUMMY_SERIAL_NUMB "123456789"
#define TCSR_BOOT_MISC_DETECT 0x01fd9000

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

static void deckard_reboot_mode_clean(void)
{
	if (!env_get("reboot_mode"))
		return;

	env_set("reboot_mode", "");
	env_save();
	mdelay(1000);
}

static void deckard_reboot_mode_edl(void)
{
	/*
	 * Setup the boot_misc to 0x1 for EDL, 0x10 is minidump,
	 * 0x20 is dump. We may need to disalble PMIC watchdog
	 * also to avoid trigger during EDL transfers, but
	 * our platform has it disable by default.
	 */
	writel(0x1, TCSR_BOOT_MISC_DETECT);
	sysreset_walk(SYSRESET_WARM);
}

#if IS_ENABLED(CONFIG_FASTBOOT) && !CONFIG_IS_ENABLED(ENV_IS_NOWHERE)
int fastboot_set_reboot_flag(enum fastboot_reboot_reason reason)
{
	if ((reason != FASTBOOT_REBOOT_REASON_FASTBOOTD) &&
			(reason != FASTBOOT_REBOOT_REASON_OEM_EDL) &&
			(reason != FASTBOOT_REBOOT_REASON_BOOTLOADER))
		return -ENOTSUPP;

	if (reason == FASTBOOT_REBOOT_REASON_FASTBOOTD) {
		printf("Setting reboot to fastboot flag ...\n");
		env_set("reboot_mode", "f");
		env_save();
	} else if (reason == FASTBOOT_REBOOT_REASON_BOOTLOADER) {
		printf("Setting reboot to bootloader flag ...\n");
		env_set("reboot_mode", "b");
		env_save();
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_EDL) {
		printf("Rebooting to EDL mode ...\n");
		deckard_reboot_mode_edl();
	}

	return 0;
}
#endif

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
	char *reboot_mode;
	int fastbootmode = 0;
	int bootloadermode = 0;
	int edlmode = 0;

	deckard_disable_watchdog();

	g_boot_wrapper_version = boot_wrapper_version;
	printf("Boot Wrapper version: %d\n", g_boot_wrapper_version);

	deckard_setup();

	/*
	 * Check if we have a special boot command
	 * f - for fastboot and b - to stay in bootloader
	 * mode, i.e, interrupt autoboot
	 */
	reboot_mode = env_get("reboot_mode");
	if (reboot_mode && (reboot_mode[0] == 'f' ))
		fastbootmode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'b' ))
		bootloadermode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'e' ))
		edlmode = 1;
	/* cleanup all reboot_mode setups if any */
	deckard_reboot_mode_clean();

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

	/* Check if special boot mode (fastboot, bootloader, edl) was setup */
	if(fastbootmode) {
		printf("Starting fastboot mode...\n");
		env_set("kernel_addr_r","90000000");

		/*
		 * serial# is a special var, like ethaddr, they are defined as
		 * write-once as a protection, we want to maintain that protection
		 * writing it only once here, if not before, and avoid error after.
		 */
		if (!env_get("serial#")) {
			printf("setting dummy serial number: %s\n", DECKARD_DUMMY_SERIAL_NUMB);
			env_set("serial#", DECKARD_DUMMY_SERIAL_NUMB);
			env_save();
		}
		run_command("fastboot -l 0x880000000 -s 0x10000000 usb 0", 0);
	}

	if (bootloadermode) {
		printf("Starting bootloader mode...\n");
		env_set("bootdelay", "-1");
	}

	if (edlmode) {
		printf("Rebooting to EDL mode...\n");
		deckard_reboot_mode_edl();
	}

	printf("deckard init: done\n");
///////

	return 0;
}
