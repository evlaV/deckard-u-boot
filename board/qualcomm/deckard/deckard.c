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
#include "deckard_battery.h"
#include "pm8550vs.h"
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

	/* Fix GPIO Levels that XBL continues to not let us */
	pm8550vs_set_gpio_vin_level(pm8550vse, PM8550VS_GPIO_3, PM8550VS_GPIO_VIN_1);
	pm8550vs_set_gpio_vin_level(pm8550vse, PM8550VS_GPIO_4, PM8550VS_GPIO_VIN_1);

	/* Ensure Battery is OK to proceed */
	deckard_battery_recover_uvlo(pmk8550, pm8550b);

	/* Setup Charging */
	deckard_battery_enable_charging(pm8550b);

	printf("smb1510: enable fixed frequency at 500khz mode\n");
	smb1510_set_switching_frequency(pmk8550, 0x0F);
}

#if 0
static void deckard_dead_battery_recovery(void)
{

#if 0
	// Why isn't this alway working?
	// Check Battery Status
	ret = pmic_reg_read(pmic, CHGR_VBAT_STATUS);
	if (ret < 0)
	{
		printf("Failed to Read Battery Status, skipping Dead Battery Check\n");
		return;
	}

	if (ret & CHGR_VBAT_STATUS_GT_FULL_ON)
	{
		// Battery is OK, just return
		return;
	}
#else
	ret = pmic_reg_read(pmic, CHGR_VBAT_STATUS);
	printf("PM8550B 0x2608: 0x%02X\n", ret);
	if (voltage > 6600 && !(ret & CHGR_VBAT_STATUS_LT_2V)) {
		// Battery is OK, just return
		return;
	}

#endif

	// return;

	// //D2 WIN_OV (0x307A[5])
	// err_flag |= pm_smb_bridge_write_mask(SMB_SID, SMB_1510_SCHG_PERPH0_CFG_SDCDC, MASK_1510_CFG_EN_WIN_OV, enable ? MASK_1510_CFG_EN_WIN_OV : 0);
	// //X2 WIN_OV (0x30C8[3])
	// err_flag |= pm_smb_bridge_write_mask(SMB_SID, SMB_1510_SCHG_PERPH0_ENG_SDCDC_3LVL_B_LOOP_ERR_CFG, MASK_1510_ENG_X2_WIN_OV, enable ? MASK_1510_ENG_X2_WIN_OV : 0);
	// //VBAT2_PD (0x309E[5])
	// err_flag |= pm_smb_bridge_write_mask(SMB_SID, SMB_1510_SCHG_PERPH0_LOCK_SPARE, MASK_1510_CFG_EN_VBAT2_PD, 0);
	// //VBAT2_UV (0x3080[3])
	// err_flag |= pm_smb_bridge_write_mask(SMB_SID, SMB_1510_SCHG_PERPH0_SOVP_CFG0, MASK_1510_CFG_EN_UV_RISE_DEB, enable ? MASK_1510_CFG_EN_UV_RISE_DEB : 0);
	// //VBAT2_UVLO (0x31C1[4:3]) set to 4.5V
	// err_flag |= pm_smb_bridge_write_mask(SMB_SID, SMB_1510_SCHG_PERPH1_ENG_SSUPPLY1, MASK_1510_CFG_VBAT2_UVLO, (VBAT2_UVLO_4_5V << 3));

	// Sequeunce Seen on CHGR_VBAT_STATUS is 0x08, 0x08, 0x09, 0x09, ...
	// Sequeunce Seen on 0x2607 is 0x00, 0x13, 0x13, ...

	ret = pmic_reg_read(pmic, CHGR_VBAT_STATUS);
	printf("PM8550B 0x2608: 0x%02X\n", ret);

	// Setup the Charger
	printf("Dead Battery Detected, Attempting Recovery\n");
	pmic_reg_write(pmic, CHGR_IPRE_CFG_ADDR, IPRE_500MA);
	pmic_reg_write(pmic, CHGR_ICHG_CFG_ADDR, 1000/60);
	pmic_reg_write(pmic, CHGR_CHG_EN_ADDR, CHGR_CHG_EN_CHARGING_ENABLE_CMD);

	// 4FB0 and 4FB1
	// 0x59DB is ~ 9 Volts

	// TODO Maybe Pulse the LED Here?
	// Re-add LED to indicate dead battery?

	// Change GPIO12 to Output
	// pmic_reg_write(pmic, 0x9340, 0x01);
	// pmic_reg_write(pmic, 0x9344, 0x80);
	// pmic_reg_write(pmic, 0x9346, 0x80);

	// Wait till there is life
	// while (ret & CHGR_VBAT_STATUS_LT_2V) {
	while (voltage < 6600 || (ret & CHGR_VBAT_STATUS_LT_2V)) {
		voltage = print_regs(pmic);
		read_adc(pmic_pmk);
		// print_smb1510_regs(pmic_pmk);

		// // Toggle GPIO 12 X2D2 Enable Line
		// if ((i % 4) ==0) {
		// 	printf("Toggling GPIO 12\n");
		// 	pmic_reg_write(pmic, 0x9344, 0x00);
		// 	mdelay(200);
		// 	pmic_reg_write(pmic, 0x9344, 0x80);
		// }
		// i++;

		//pmic_reg_write(pmic, 0x2643, 0x01);
		//pmic_reg_write(pmic, 0x2563, 0x01);
		//pmic_reg_write(pmic, 0x2573, 0x01);

		ret = pmic_reg_read(pmic, CHGR_VBAT_STATUS);
		printf("PM8550B 0x2608: 0x%02X\n", ret);

		mdelay(2000);
	}
}
#endif

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice     *btn, *led;
	int                 ret;
	enum button_state_t state;

	deckard_disable_watchdog();

	g_boot_wrapper_version = boot_wrapper_version;
	printf("Boot Wrapper version: %d\n", g_boot_wrapper_version);

	deckard_setup();

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

	//put code here for figuring out A/B/R partition that was booted

	return 0;
}
