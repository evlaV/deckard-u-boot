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

DECLARE_GLOBAL_DATA_PTR;

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

void read_adc(struct udevice *pmic)
{
	int ret;
	int data[2];

	int channels[5] = { 0x8E, 0x8F, 0x94, 0x96, 0x9D };
	int channel_sf[5] = { 3, 6, 6, 3, 3 };
	const char channel_str[5][11] = {
		"VPH_PWR",
		"VBAT_SNS_Q",
		"VBAT_SNS_C",
		"VBAT_MID_Q",
		"VBAT_MID_C",		

	};

	// Enable ADC
	//pmic_reg_write(pmic, 0x3646, 0x01);

	printf("Reading ADC\n");

	pmic_reg_write(pmic, 0x904A, 0x00);
	pmic_reg_write(pmic, 0x9050, 0x00);
	pmic_reg_write(pmic, 0x90E5, 0x01);

	for (int i =0 ; i < 5; i++)
	{
		// pmic_reg_write(pmic, 0x904B, 0x01);
		pmic_reg_write(pmic, 0x904F, 0x07);
		pmic_reg_write(pmic, 0x9050, 0x80);
		pmic_reg_write(pmic, 0x9051, 0x01);
		ret = pmic_reg_read(pmic, 0x9052);
		pmic_reg_write(pmic, 0x9052, (ret & ~0x3C) | 0x28);
		pmic_reg_write(pmic, 0x9053, 0x81);
		pmic_reg_write(pmic, 0x9054, channels[i]);
		pmic_reg_write(pmic, 0x9055, 0x05);
		pmic_reg_write(pmic, 0x90e5, 0x01);

		int loops = 0;
		while (loops < 10)
		{
			// ret = pmic_reg_read(pmic, 0x9010);
			// printf("0x9010: 0x%02X\n", ret);
			// ret = pmic_reg_read(pmic, 0x9046);
			// printf("0x9046: 0x%02X\n", ret);
			// ret = pmic_reg_read(pmic, 0x90e5);
			// printf("0x90e5: 0x%02X\n", ret);
			// ret = pmic_reg_read(pmic, 0x904B);
			// printf("0x904B: 0x%02X\n", ret);
			ret = pmic_reg_read(pmic, 0x904A);
			// printf("ret: 0x%02X\n", ret);
			// TODO Fix this so we have a valid return in here every loop
			if (ret & 0x01)
			{
				u32 voltage;

				data[0] = pmic_reg_read(pmic, 0x905C);
				data[1] = pmic_reg_read(pmic, 0x905D);
				voltage = data[1] << 8 | data[0];
				if (voltage > 28900) {
					voltage = 0;
				}

				// printf("PM8550B 0x%02X: 0x%02X%02X\n", channels[i], data[1], data[0]);
				voltage *= 1875 * channel_sf[i];
				voltage /= 28900;
				printf("ADC: %s %d mV\n", channel_str[i], voltage);

				break;
			}
			loops++;
			mdelay(10);
		}
	}
}


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
	pmic_reg_write(pmic, 0x7E4B, SMB_SID | CMD_READ);
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

static u32 print_regs(struct udevice *pmic)
{
	int ret;
	u32 voltage;

	ret = pmic_reg_read(pmic, 0x4FB1);
	voltage = ret << 8;
	voltage |= pmic_reg_read(pmic, 0x4FB0) & 0xFF;
	if (voltage > 28900) {
		voltage = 0;
	}
	voltage *= 1875 * 6;
	voltage /= 28900;
	printf("PM8550B VBAT: %d mV\n", voltage);

	ret = pmic_reg_read(pmic, 0x4FB3);
	voltage = ret << 8;
	voltage |= pmic_reg_read(pmic, 0x4FB2) & 0xFF;
	if (voltage > 28900) {
		voltage = 0;
	}
	voltage *= 1875 * 6;
	voltage /= 28900;
	printf("PM8550B VBAT: %d mV\n", voltage);

	ret = pmic_reg_read(pmic, 0x4FC3);
	voltage = ret << 8;
	voltage |= pmic_reg_read(pmic, 0x4FC2) & 0xFF;
	if (voltage > 28900) {
		voltage = 0;
	}
	voltage *= 1875 * 6;
	voltage /= 28900;
	printf("PM8550B VBAT: %d mV\n", voltage);

	ret = pmic_reg_read(pmic, 0x2810);
	// printf("PM8550B 0x2810: 0x%02X\n", ret);
	if (ret & 0x04) {
		printf("\tBATIF: Low Batt\n");
	}

	ret = pmic_reg_read(pmic, 0x2850);
	// printf("PM8550B 0x2850: 0x%02X\n", ret);
	if (ret & 0x01) {
		printf("\tBATIF: BAT FET Body Diode On\n");
	}
	if (ret & 0x02) {
		printf("\tBATIF: BAT FET On\n");
	}


	// ret = pmic_reg_read(pmic, 0x2862);
	// printf("PM8550B 0x2862: 0x%02X\n", ret);

	// TODO This should be enable once battery voltage is above Fast Charge Threshold to 0x01
	ret = pmic_reg_read(pmic, 0x2868);
	printf("PM8550B 0x2868: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2950);
	// printf("PM8550B 0x2950: 0x%02X\n", ret);
	printf("\tUSB: High Current Mode %s\n", ret & 0x04 ? "On" : "Off");

	// ret = pmic_reg_read(pmic, 0x2951);
	// printf("PM8550B 0x2951: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2952);
	// printf("PM8550B 0x2952: 0x%02X\n", ret);
	printf("\tUSB: Input Current Limit %d mA\n", ret * 50);

	ret = pmic_reg_read(pmic, 0x2C10);
	printf("PM8550B 0x2C10: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2C40);
	printf("\tMISC: Charger Block %s\n", ret & 0x01 ? "Shutdown" : "Active");
	// printf("PM8550B 0x2C40: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2C4E);
	printf("PM8550B 0x2C4E: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2C4F);
	// printf("PM8550B 0x2C4F: 0x%02X\n", ret);
	printf("\tMISC: X2D2 %s\n", ret & 0x02 ? "Enabled" : "Disabled");

	ret = pmic_reg_read(pmic, 0x2606);
	// printf("PM8550B 0x2606: 0x%02X\n", ret);
	const char charger_modes[8][16] = {
		"Inhibit",
		"Trickle",
		"Precharge",
		"Full",
		"Taper",
		"Termination",
		"Paused",
		"Disabled"
	};
	printf("\tCharger: %s\n", charger_modes[ret & 0x07]);

	// ret = pmic_reg_read(pmic, 0x2607);
	// printf("PM8550B 0x2607: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2608);
	printf("PM8550B 0x2608: 0x%02X\n", ret);
	if (ret & 0x01) {
		printf("\tCharger: Batt < 2 VDC\n");
	}
	printf("\tCharger: Batt %s VSysMin\n", ret & 0x02 ? ">" : "<");

	// ret = pmic_reg_read(pmic, 0x2609);
	// printf("PM8550B 0x2609: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x260A);
	// printf("PM8550B 0x260A: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x260B);
	// printf("PM8550B 0x260B: 0x%02X\n", ret);
	printf("\tCharger: Float Voltage %d V\n", ret * 20 + 7200);

	ret = pmic_reg_read(pmic, 0x260C);
	printf("PM8550B 0x260C: 0x%02X\n", ret);

	// TODO REVISIT
	ret = pmic_reg_read(pmic, 0x260D);
	printf("PM8550B 0x260D: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x260E);
	// printf("PM8550B 0x260E: 0x%02X\n", ret);
	printf("\tCharger: Fast Current %d mA\n", ret * 60);

	// ret = pmic_reg_read(pmic, 0x260F);
	// printf("PM8550B 0x260F: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2610);
	printf("PM8550B 0x2610: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2640);
	// printf("PM8550B 0x2640: 0x%02X\n", ret);
	printf("\tCharger: VSysMin %d V\n", (ret & 0x07) * 200 + 2800);

	// ret = pmic_reg_read(pmic, 0x2641);
	// printf("PM8550B 0x2641: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2650);
	printf("PM8550B 0x2650: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x2654);
	// printf("PM8550B 0x2654: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x2658);
	// printf("PM8550B 0x2658: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2678);
	// printf("PM8550B 0x2678: 0x%02X\n", ret);
	printf("\tCharger: Inhibit %s\n", ret & 0x80 ? "Disabled" : "Enabled");

	// ret = pmic_reg_read(pmic, 0x2680);
	// printf("PM8550B 0x2680: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x26B0);
	printf("PM8550B 0x26B0: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x26B2);
	// printf("PM8550B 0x26B2: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x26B4);
	// printf("PM8550B 0x26B4: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x26B6);
	printf("PM8550B 0x26B6: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x26B8);
	printf("PM8550B 0x26B8: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x26BA);
	printf("PM8550B 0x26BA: 0x%02X ..\n", ret);

	ret = pmic_reg_read(pmic, 0x26BC);
	printf("PM8550B 0x26BC: 0x%02X ..\n", ret);

	ret = pmic_reg_read(pmic, 0x2540);
	printf("PM8550B 0x2540: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2550);
	printf("PM8550B 0x2550: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2560);
	printf("PM8550B 0x2560: 0x%02X\n", ret);

	ret = pmic_reg_read(pmic, 0x2570);
	printf("PM8550B 0x2570: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x2580);
	// printf("PM8550B 0x2580: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9305);
	// printf("PM8550B 0x9305: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9308);
	// printf("PM8550B 0x9308: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9340);
	// printf("PM8550B 0x9340: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9342);
	// printf("PM8550B 0x9342: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9344);
	// printf("PM8550B 0x9344: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9345);
	// printf("PM8550B 0x9345: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x9346);
	// printf("PM8550B 0x9346: 0x%02X\n", ret);

	return voltage;
}

void print_smb1510_regs(struct udevice *pmic)
{
	int ret;

	// ret = pmic_reg_read(pmic, 0xFF04);
	// printf("PMK8550 0x1208: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x1208);
	// printf("PMK8550 0x1208: 0x%02X\n", ret);

	// ret = pmic_reg_read(pmic, 0x1246);
	// printf("PMK8550 0x1242: 0x%02X\n", ret);

	// pmic_reg_write(pmic, 0x1246, 0x88);

	// ret = pmic_reg_read(pmic, 0x1246);
	// printf("PMK8550 0x1242: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0xFF04);
	printf("SMB1510 0xFF04: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3010);
	printf("SMB1510 0x3010: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3025);
	printf("SMB1510 0x3025: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3040);
	printf("SMB1510 0x3040: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3041);
	printf("SMB1510 0x3041: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3043);
	printf("SMB1510 0x3043: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3045);
	printf("SMB1510 0x3045: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3046);
	printf("SMB1510 0x3046: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x304B);
	printf("SMB1510 0x304B: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x304C);
	printf("SMB1510 0x304C: 0x%02X\n", ret);

	ret = read_smb1510(pmic, 0x3110);
	printf("SMB1510 0x3110: 0x%02X\n", ret);
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

	//voltage = print_regs(pmic);
	//read_adc(pmic_pmk);
	//print_smb1510_regs(pmic_pmk);

	printf("smb1510: enable fixed frequency at 500khz mode\n");
	write_smb1510(pmic_pmk,0x30C9,0x0f);


}

static void deckard_dead_battery_recovery(void)
{
	int ret;
	struct udevice *pmic;
	struct udevice *pmic_pmk;
	u32 voltage;

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pmic);
	if (ret != 0)
	{
		printf("Failed to Find PM8550B, skipping Dead Battery Check\n");
		return;
	}

	// Find the PMK8650
	ret = pmic_get("pmic@0", &pmic_pmk);
	if (ret != 0)
	{
		printf("Failed to Find PMK8650, skipping Dead Battery Check\n");
		return;
	}

	// Enable ADC
	pmic_reg_write(pmic, 0x3646, 0x80);
	// Enable FG
	pmic_reg_write(pmic, 0x4F46, 0x80); 

	voltage = print_regs(pmic);
	read_adc(pmic_pmk);
	print_smb1510_regs(pmic_pmk);

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

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice *btn,*led;
	int ret;
	enum button_state_t state;

	//deckard_dead_battery_recovery();

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
