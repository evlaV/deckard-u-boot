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
#include <part.h>

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

#define MAX_WAIT_COUNT                                      (5)
#define SPMI_BRIDGE_DELAY                                   (30)
#define SMB_SID          		 	(0xD)
#define CMD_WRITE        		 	(0x20)
#define CMD_READ          			(0x10)
#define COMPLETE_STATUS_MASK        (0x40)
#define SDAM_STATUS_IN_PROCESS      (0x00)
#define SDAM_STATUS_COMPLETED       (0x40)
#define PID(reg)          			((u32)(((reg) & 0xFF00) >> 8))
#define OFFSET(reg)       			((u32)((reg) & 0x00FF))

static int read_smb1510(struct udevice *pmic, uint16_t reg)
{
  	int cnt = 0;
	int ret;

  	//Set register SID/PID/Offset and data
	pmic_reg_write(pmic, 0x7E4B, SMB_SID | CMD_READ);
	pmic_reg_write(pmic, 0x7E4C, PID(reg));
	pmic_reg_write(pmic, 0x7E4D, OFFSET(reg));

	//Trigger PBS
	pmic_reg_write(pmic, 0x2142, 0x10);

	//Wait for read cmd completion
	while(cnt++ < MAX_WAIT_COUNT)
	{
		udelay(SPMI_BRIDGE_DELAY);
		ret = pmic_reg_read(pmic, 0x7E4B);
		if((ret & COMPLETE_STATUS_MASK))
		{
			break;
		}
	}

	if(!(ret & SDAM_STATUS_COMPLETED))
	{
		printf("bridge read not completed, cmd_status = 0x%02X, count = %d", ret, cnt);
		return -1;
	}

  	ret = pmic_reg_read(pmic, 0x7E4E);

  	return ret;
}

static int write_smb1510(struct udevice *pmic, uint16_t reg, uint8_t value)
{
  	int cnt = 0;
	int ret;

  	//Set register SID/PID/Offset and data
	pmic_reg_write(pmic, 0x7E4B, SMB_SID | CMD_WRITE);
	pmic_reg_write(pmic, 0x7E4C, PID(reg));
	pmic_reg_write(pmic, 0x7E4D, OFFSET(reg));
	pmic_reg_write(pmic, 0x7E4E, value);

  	//Trigger PBS
	pmic_reg_write(pmic, 0x2142, 0x10);

	//Wait for read cmd completion
	while(cnt++ < MAX_WAIT_COUNT)
	{
		udelay(SPMI_BRIDGE_DELAY);
		ret = pmic_reg_read(pmic, 0x7E4B);
		if((ret & COMPLETE_STATUS_MASK))
		{
			break;
		}
	}

	if(!(ret & SDAM_STATUS_COMPLETED))
	{
		printf("bridge read not completed, cmd_status = 0x%02X, count = %d", ret, cnt);
		return -1;
	}

  	return 0;
}

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
	int ret;
	struct udevice *pmic;
	struct udevice *pmic_pmk;
	u32 voltage;

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
	write_smb1510(pmic_pmk,0x30C9,0x0f);
}

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice *btn,*led;
	int ret;
	enum button_state_t state;
	struct blk_desc *desc;

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
			// env_set("BOOT_ORDER", "R");
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

	ret  = led_get_by_label("blue", &led);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
		led_set_state(led, LEDST_ON);
	};

	// get board data from eeprom
	deckard_eeprom_init();

	//put code here for figuring out A/B/R partition that was booted
//////
	ret = blk_get_device_by_str("scsi", "2", &desc);
	if( ret >= 0 )
	{
		part_print_fstype_uuid(desc);
	}
	else
	{
		printf("blk_get_device_by_str failed, ret = %d\n",ret);
	}

	printf("booted_slot: %s\n", env_get("booted_slot"));

	printf("deckard init: done\n");
///////

	return 0;
}
