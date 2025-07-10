// SPDX-License-Identifier: GPL-2.0+
/*
 * Board init file for Dragonboard 410C
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 */

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
#include <fs.h>
#include <image.h>

#include "deckard_eeprom.h"
#include "deckard_battery.h"
#include "pm8550vs.h"
#include "smb1510.h"
#include <part.h>
#include "deckard.h"

DECLARE_GLOBAL_DATA_PTR;

#define DECKARD_SYSTEM_LUNIDX "0"
#define DECKARD_UBOOT_LUNIDX "2"
#define DECKARD_RESCUE_LUNIDX "3"
//M slot is the master slot / default
#define DECKARD_UBOOT_ENV_SLOT_A "2:4"
#define DECKARD_UBOOT_FW_SLOT_A "2:5"
#define DECKARD_UBOOT_ENV_SLOT_B "2:7"
#define DECKARD_UBOOT_FW_SLOT_B "2:8"
#define DECKARD_UBOOT_ENV_SLOT_R "3:2"
#define DECKARD_UBOOT_FW_SLOT_R "3:3"
#define DECKARD_SLOT_A_PARTITION "0:4"
#define DECKARD_SLOT_B_PARTITION "0:5"
#define DECKARD_SLOT_R_PARTITION "3:4"

//these are set as a bit-mask typically when we detect a button is enabled
#define VOLUP_BUTTON_MASK 0b100
#define VOLDN_BUTTON_MASK 0b010
#define SELECT_BUTTON_MASK 0b001
enum combined_button_state_t {
	VOLUP_OFF__VOLDN_OFF__SELECT_OFF = 0,
	VOLUP_OFF__VOLDN_OFF__SELECT_ON  = 1,
	VOLUP_OFF__VOLDN_ON__SELECT_OFF  = 2,
	VOLUP_OFF__VOLDN_ON__SELECT_ON   = 3,
	VOLUP_ON__VOLDN_OFF__SELECT_OFF  = 4,
	VOLUP_ON__VOLDN_OFF__SELECT_ON   = 5,
	VOLUP_ON__VOLDN_ON__SELECT_OFF   = 6,
	VOLUP_ON__VOLDN_ON__SELECT_ON    = 7,
};

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
#define HWIO_APSS_WDT_TMR1_WDOG_CTL_ADDR  0x17410008
#define HWIO_APSS_WDT_TMR2_WDOG_CTL_ADDR  0x17411008
static void deckard_disable_watchdog(void)
{
	// disable watchdog 1
	write32(HWIO_APSS_WDT_TMR1_WDOG_CTL_ADDR, 0);

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

int enable_usb_serial_console(void) 
{
	env_set("stdin","usbacm");
	env_set("stderr","usbacm");
	env_set("stdout","usbacm");
	return 0;
}

int button_state_handler(enum combined_button_state_t total_button_state, int power_button_state) 
{
	printf("total button state = %d, power button = %d\n", total_button_state, power_button_state);
	//note power button is handled separately since holding power button might be required for other button combo's 
	//to get the device to reboot, so we don't want it to be part of the possible button combos
	//otherwise it would require people to be holding power button down for a long period, 
	//or removing it at just the right time to avoid the power button being part of a combo or not

	switch(total_button_state)
	{
		case VOLUP_OFF__VOLDN_OFF__SELECT_OFF:
			break;
		case VOLUP_OFF__VOLDN_OFF__SELECT_ON:
			break;
		case VOLUP_OFF__VOLDN_ON__SELECT_OFF:
			break;
		case VOLUP_OFF__VOLDN_ON__SELECT_ON:
			break;
		case VOLUP_ON__VOLDN_OFF__SELECT_OFF:
			//enable_usb_serial_console();
			//env_set("bootdelay","20");
			break;
		case VOLUP_ON__VOLDN_OFF__SELECT_ON:
			break;
		case VOLUP_ON__VOLDN_ON__SELECT_OFF:
			break;
		case VOLUP_ON__VOLDN_ON__SELECT_ON:
			break;
		default:
			break;
	}
	return 0;
}

const char *env_scsi_get_dev_part(void)
{
	ofnode node;
	const char *bootdev;

	node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			printf("No u-boot,spl-boot-device, considering rescue slot\n");
			return DECKARD_UBOOT_ENV_SLOT_R;
		} else {
			printf("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
			case 'A':
				return DECKARD_UBOOT_ENV_SLOT_A;
				break;
			case 'B':
				return DECKARD_UBOOT_ENV_SLOT_B;
				break;
			case 'R':
				return DECKARD_UBOOT_ENV_SLOT_R;
				break;
			default:
				printf("%s: Invalid booted_slot, defaulting to Rescue env\n", __func__);
				return DECKARD_UBOOT_ENV_SLOT_R;
				break;
			}
		}
	} else {
		printf("No chosen node default to rescue!\n");
		return DECKARD_UBOOT_ENV_SLOT_R;
	}
}

const char *deckard_get_fw_part(void)
{
	char * bootedslot;
	// Get partition and label from booted slot
	// booted_slot set after parsing DTS modified by SPL
	bootedslot = env_get("booted_slot");
	switch(bootedslot[0]){
		case 'A':
			return DECKARD_UBOOT_FW_SLOT_A;
			break;
		case 'B':
			return DECKARD_UBOOT_FW_SLOT_B;
			break;
		case 'R':
		default:
			return DECKARD_UBOOT_FW_SLOT_R;
			break;
	}
}

const char *fs_firmware_get_partition(void)
{
	char *bootedslot;

	bootedslot = env_get("booted_slot");
	switch(bootedslot[0]){
		case 'A':
			return DECKARD_SLOT_A_PARTITION;
			break;
		case 'B':
			return DECKARD_SLOT_B_PARTITION;
			break;
		default:
			return DECKARD_SLOT_A_PARTITION;
			break;
	}
}


/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice     *btn, *led;
	int                 ret;
	enum button_state_t state;
	char * bootedslot;
	char * fastboot_envvar;
	int fastbootmode = 0;
	ofnode node;
	enum combined_button_state_t total_button_state = 0;
	int power_button_state = 0;
	
	node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		const char *bootdev;
		u32 boot_wrapper_version;

		bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			printf("No u-boot,spl-boot-device, considering rescue slot\n");
			env_set("booted_slot","R");
		} else {
			printf("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
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

		ret = ofnode_read_u32(node, "boot-wrapper-version", &boot_wrapper_version);
		if (ret)
			printf("No boot wrapper version\n");
		else {
			printf("Boot Wrapper version: %d\n", boot_wrapper_version);
			env_set_ulong("boot_wrapper_version", boot_wrapper_version);
		}

	} else {
		panic("No chosen node !\n");
	}

	printf("watchdog disable!!!!!!!!!!!!\n");
	deckard_disable_watchdog();

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

	ret = button_get_by_label("Volume Down", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol- button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol- button pressed\n");
			total_button_state |= VOLUP_BUTTON_MASK;
		}
	}

	ret = button_get_by_label("Volume Up", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol+ button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol+ button pressed\n");
			total_button_state |= VOLDN_BUTTON_MASK;
		}
		else
		{
			env_set("bootdelay","2");
		}
	}
	
	ret = button_get_by_label("Power Button", &btn);
	if (ret < 0) {
		printf("Couldn't find power button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("PWR button pressed\n");
			power_button_state = 1;
		}
	}

	ret = button_get_by_label("select", &btn);
	if (ret < 0) {
		printf("Couldn't find select button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Select button pressed\n");
			total_button_state |= SELECT_BUTTON_MASK;
		}
	}

	button_state_handler(total_button_state,power_button_state);

	ret = led_get_by_label("blue", &led);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
		led_set_state(led, LEDST_ON);
	};

	// get board data from eeprom
	deckard_eeprom_init();

	env_set("UBOOTLUNIDX", DECKARD_UBOOT_LUNIDX);
	env_set("SYSTEMLUNIDX", DECKARD_SYSTEM_LUNIDX);
	env_set("RESCUELUNIDX", DECKARD_RESCUE_LUNIDX);

	// do we still need this?
	bootedslot = env_get("booted_slot");
	env_set("BOOT_ORDER", bootedslot);
	printf("booted_slot: %s\n", env_get("booted_slot"));

	if( fastbootmode ) 
	{
		printf("Starting fastboot mode...\n");
		env_set("kernel_addr_r","90000000");
		env_set("serial#","123456789"); //put dummy serial number here used by fastboot
		run_command("fastboot -l 0x880000000 -s 0x10000000 usb 0", 0);
	}

	// if requested, use the default env boot variables
	// otherwise override with deckard custom boot command
	if ( !env_get("use_custom_boot") ){
		env_set("bootcmd", "deckard_boot");
	}

	printf("deckard init: done\n");
///////

	return 0;
}

int ft_board_setup(void *fdt, struct bd_info *bd)
{
	char *boot_wrapper_version;
	int nodeoffset;
	int ret;

	boot_wrapper_version = env_get("boot_wrapper_version");

	if (!boot_wrapper_version)
		return 0;

	/* find or create "/chosen" node. */
	nodeoffset = fdt_find_or_add_subnode(fdt, 0, "chosen");
	if (nodeoffset < 0)
		return nodeoffset;

	/* add bootwrapper version */
	ret = fdt_setprop_string(fdt, nodeoffset, "bootwrapper,version", boot_wrapper_version);
	if (ret < 0) {
		printf("WARNING: could not set bootwrapper,version %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

	return 0;
}



#define DEVPARTMAXLEN 16
#define LOADADDRMAXLEN 16
#define PATHMAXLEN 256

int scsi_do_load_helper(char * partition, char * addr, char * path)
{
	int  argc = 5;
	char *argv[5];
	char arg0[] = "do_the_dew"; // ?
	char arg1[] = "scsi";

	argv[0]=arg0;
	argv[1]=arg1;
	argv[2]=partition; //e.g. "1:d"
	argv[3]=addr; //e.g. "0xa7000000"
	argv[4]=path; // e.g. "/boot/maindtb.dtb"

	return do_load(NULL, 0, argc, argv, FS_TYPE_EXT);
}

// per arm64 linux boot documenation, default text_offset is 0x80000
#define ADDR_KERNEL "0x88000000"
// this gives us roughly 290 Megabytes for kernel, dtb, and initrd
#define MAX_ADDR_INITRD 0x99000000

// Deckard boot command.  Executes sequence that would normally be in the boot.scr
int do_deckard_boot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char * bootedslot;
	char * dtb_qcom;
	char * eeprom_board;
	char * KERNEL_TARGET = "/boot/Image";
	char * INITRD_TARGET = "/boot/initrd.uImage";
	char partition[DEVPARTMAXLEN];
	char label[DEVPARTMAXLEN];
	char DTB_TARGET[PATHMAXLEN];
	int ret;
	ulong image_size, addr_kernel, addr_dtb, addr_initrd;
	char DTB_ADDR[DEVPARTMAXLEN];
	char INITRD_ADDR[DEVPARTMAXLEN];
	char bootargs[PATHMAXLEN]; // how long can this be?
	char * default_bootargs;
	char * temp_bootargs;

	printf("Deckard custom boot function\n");

	// TODO is there a way to only do this if needed?
	// If we haven't already scanned the SCSI devices we need to do it here
	//scsi_scan(true);

	// Get partition and label from booted slot
	// booted_slot set after parsing DTS modified by SPL
	bootedslot = env_get("booted_slot");
	switch(bootedslot[0]){
		case 'A':
			strncpy(partition, DECKARD_SLOT_A_PARTITION, DEVPARTMAXLEN);
			strncpy(label, "rootfs-A", DEVPARTMAXLEN);
			break;
		case 'B':
			strncpy(partition, DECKARD_SLOT_B_PARTITION, DEVPARTMAXLEN);
			strncpy(label, "rootfs-B", DEVPARTMAXLEN);
			break;
		case 'R':
		default:
			strncpy(partition, DECKARD_SLOT_R_PARTITION, DEVPARTMAXLEN);
			strncpy(label, "system_r", DEVPARTMAXLEN);
			break;
	}

	// Get DTB
	dtb_qcom = env_get("DTB_QCOM");
	eeprom_board = env_get("EEPROM_BOARD");
	if (dtb_qcom) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/%s", dtb_qcom);
	} else if (eeprom_board) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/sm8650-%s.dtb", eeprom_board);
	} else {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/maindtb.dtb");
	}

	// Load kernel
	if ( scsi_do_load_helper(partition, ADDR_KERNEL, KERNEL_TARGET) ) {
		goto fail;
	}
	// Get kernel size.  TODO for now we are assuming an uncompressed linux image
	addr_kernel = hextoul(ADDR_KERNEL, NULL);
	ret = booti_get_image_size(addr_kernel, &image_size);
	if (ret)
		printf("Could not get linux image size ( %d )\n", ret);
    // get dtb address.  kernel will expand to the nearest 2MB boundary, so we give us a 10MB buffer
	addr_dtb = addr_kernel + image_size + 0xA00000;
	// align to next 2MB address
	addr_dtb += 0x200000 - ( addr_dtb % 0x200000 );
	sprintf(DTB_ADDR, "0x%lX", addr_dtb);
	printf("DTB_ADDR %s\n", DTB_ADDR); // debug
	// load dtb
	if ( scsi_do_load_helper(partition, DTB_ADDR, DTB_TARGET) ) {
		goto fail;
	}
	// get address for initrd
	image_size = hextoul(env_get("filesize"), NULL);
	addr_initrd = addr_dtb + image_size + + 0xA00000;
	addr_initrd += 0x200000 - ( addr_initrd % 0x200000 );
	sprintf(INITRD_ADDR, "0x%lX", addr_initrd);
	printf("INITRD_ADDR %s\n", INITRD_ADDR); // debug
	// load inird
	if ( scsi_do_load_helper(partition, INITRD_ADDR, INITRD_TARGET) ) {
		goto fail;
	}

	// check for max file sizes
	image_size = hextoul(env_get("filesize"), NULL);
	if ( addr_initrd + image_size > MAX_ADDR_INITRD ) {
		printf("DANGER!! exceeded maximum memory allocated for kernel, dtb, and initrd!!!");
	}

	// prep bootargs
	default_bootargs = env_get("default_bootargs");
	temp_bootargs = env_get("temp_bootargs");
	snprintf(bootargs, PATHMAXLEN, "%s %s root=PARTLABEL=%s init=/sbin/init ro rauc.slot=%s noresume rootwait earlycon",
									default_bootargs ? default_bootargs : "", temp_bootargs ? temp_bootargs : "", label, bootedslot );
	env_set("bootargs", bootargs);

	// erase temp bootargs which is used for Testing
	env_set("temp_bootargs", "");

	env_set("kernel_addr_r", ADDR_KERNEL);
	env_set("ramdisk_addr_r", INITRD_ADDR);
	env_set("fdtcontroladdr", DTB_ADDR);
	run_command("booti ${kernel_addr_r} ${ramdisk_addr_r} ${fdtcontroladdr}", 0);

	printf("!!Boot Failed, Reset!!");

fail:
	// should not get here, if we do, we should reset
	run_command("reset", 0);
	return -1;
}

// set deckard boot as uboot command so that it can be called from CLI or by
// normal autoboot process
U_BOOT_LONGHELP(deckard_boot,
    "Boot Deckard system image. TODO required inputs and return values\n"
);

U_BOOT_CMD(
	deckard_boot, CONFIG_SYS_MAXARGS,     1,      do_deckard_boot,
    "Boot Deckard system image.", deckard_boot_help_text
);
