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
#include "deckard_eeprom.h"
#include "smb1510.h"

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

#define CHGR_VBAT_STATUS			(0x2608)
#define CHGR_VBAT_STATUS_LT_2V		(0x01)
#define CHGR_VBAT_STATUS_GT_FULL_ON	(0x02)

#define CHGR_CHG_EN_ADDR  (0x2646)
#define CHGR_CHG_EN_CHARGING_ENABLE_CMD BIT(0)

#define CHGR_IPRE_CFG_ADDR (0x2650)
#define CHGR_ICHG_CFG_ADDR (0x2654)

// NOTE These are 1s currents
enum pre_charge_current {
	IPRE_100MA,
	IPRE_150MA,
	IPRE_200MA,
	IPRE_250MA,
	// note: PM8550B has 100mA step size above 300mA
	IPRE_300MA,
	IPRE_400MA,
	IPRE_500MA,
	IPRE_600MA,
};

#if 0
void check_pmic(const char *const pmic_dts, const char *const pmic_name)
{
	int             ret;
	struct udevice *pmic = NULL;

	ret = pmic_get(pmic_dts, &pmic);
	if (ret != 0)
	{
		printf("Failed to Find %s - %s, ret: %d\n", pmic_dts, pmic_name, ret);
		return;
	}

	ret = pmic_reg_read(pmic, 0x0105);
	if (ret < 0)
	{
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

static void deckard_setup(void)
{
	int ret;
	struct udevice *pmic;
	struct udevice *pmic_pmk;
	u32 voltage;

#if 0
	check_pmics_comms();
#endif

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pmic);
	if (ret != 0)
	{
		printf("Failed to Find PM8550B\n");
		return;
	}

	// Find the PMK8650
	ret = pmic_get("pmic@0", &pmic_pmk);
	if (ret != 0)
	{
		printf("Failed to Find PMK8650\n");
		return;
	}

	// Enable ADC
	pmic_reg_write(pmic, 0x3646, 0x80);
	// Enable FG
	pmic_reg_write(pmic, 0x4F46, 0x80); 

	printf("smb1510: enable fixed frequency at 500khz mode\n");

	smb1510_set_switching_frequency(pmic_pmk, 0x0F);
}

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice *btn,*led;
	int ret;
	enum button_state_t state;

	g_boot_wrapper_version = boot_wrapper_version;
	printf("Boot Wrapper version: %d\n", g_boot_wrapper_version);

	deckard_setup();

	ret = button_get_by_label("vol_down", &btn);
	if (ret < 0) {
			printf("Couldn't find Vol- button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			//env_set("BOOT_ORDER", "R");
			printf("Vol- button pressed - targeting rescue partition for boot.\n");
		}
	}

	ret = button_get_by_label("Volume Up", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol+ button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol+ button pressed - do nothing.\n");
		}
	}

	ret = button_get_by_label("pwrkey", &btn);
	if (ret < 0) {
		printf("Couldn't find power button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("PWR button pressed - do nothing.\n");
		}
	}

	ret  = led_get_by_label("blue", &led);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
		led_set_state(led, LEDST_ON);
	};

	// get board data from eeprom
	deckard_eeprom_init();

	return 0;
}
