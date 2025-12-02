// SPDX-License-Identifier: GPL-2.0+
/*
 * Board init file for Dragonboard 410C
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 */

#include <bootstage.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/pinctrl.h>
#include <efi_config.h>
#include <efi_loader.h>
#include <efi_variable.h>
#include <env.h>
#include <fastboot.h>
#include <init.h>
#include <malloc.h>
#include <mmc.h>
#include <net.h>
#include <sort.h>
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
#include <remoteproc.h>
#include <command.h>
#include <dm/device-internal.h>
#include <fs.h>
#include <image.h>
#include <sysreset.h>
#include <splash.h>
#include <video.h>

#include "deckard_eeprom.h"
#include "deckard_battery.h"
#include "pm8550vs.h"
#include "smb1510.h"
#include "pmk8550.h"
#include <part.h>
#include "deckard.h"

DECLARE_GLOBAL_DATA_PTR;

#define DECKARD_IFNAME "scsi"

#define DECKARD_UBOOT_ENV_SLOT_A "ubootenv_a"
#define DECKARD_UBOOT_FW_SLOT_A "ubootfw_a"
#define DECKARD_ROOTFS_SLOT_A "rootfs-A"
#define DECKARD_UBOOT_SLOT_A "uboot_a"

#define DECKARD_UBOOT_ENV_SLOT_B "ubootenv_b"
#define DECKARD_UBOOT_FW_SLOT_B "ubootfw_b"
#define DECKARD_ROOTFS_SLOT_B "rootfs-B"
#define DECKARD_UBOOT_SLOT_B "uboot_b"

//add these names as alternatives for supporting old ev2 layouts -- delete once we stop using ev2's
#define ALLOW_OLD_ROOTFS_NAME_FALLBACK
#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
  #define DECKARD_ROOTFS_SLOT_OLD_A "system0"
  #define DECKARD_ROOTFS_SLOT_OLD_B "system1"
#endif

//TODO: search for the partlabel with "esp" on LUN0 rather than hardcode here!
#define DECKARD_ESP_PARTITION_NAME "esp"
//hardcoded for now
#define DECKARD_ESP_PARTITION "0:1"

//TODO: search for "efi-A" and "efi-B"
#define DECKARD_EFI_NAME_SLOT_A "efi-A"
#define DECKARD_EFI_NAME_SLOT_B "efi-B"
//hardcoded for now
#define DECKARD_EFI_SLOT_A "0:2"
#define DECKARD_EFI_SLOT_B "0:3"

#define ENV_EFI_ENABLED "enable-efi"
#define ENV_EFI_REMOVABLE_BOOT "efi-removable-boot"
#define ENV_EFI_RESET_BOOT_ENTRIES "efi-reset-boot-entries"
/* Our boot entry is always 0xFAFF for convenience and to avoid conflicts */
#define EFI_INTERNAL_BOOT_INDEX (0xFAFF)

enum boot_priority {
	BOOT_INTERNAL_ONLY,
	BOOT_INTERNAL_FIRST,
	BOOT_REMOVABLE_FIRST,
};

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

#define DECKARD_DUMMY_SERIAL_NUMB "123456789"
#define TCSR_BOOT_MISC_DETECT 0x01fd9000

enum full_screen_images_t 
{
	BOOT_LOGO = 0,
	BATTERY_CHARGING = 1,
	BATTERY_DEAD = 2,
	BATTERY_PROBLEM = 3,
	FULLSCREEN_IMAGES_COUNT
};

bool g_display_enabled = false;
int display_turn_off(void);
int display_turn_off_force(void);
void deckard_load_logo(enum full_screen_images_t image);
int deckard_check_for_key_press(void);
int deckard_prepare_for_boot(void);

struct deckard_esp_info {
	bool valid;
	int devnum;
	int partnum;
};

static enum deckard_efi_mode {
	/* NEVER initialise EFI subsystem */
	EFI_MODE_DISABLED,
	/* When attempting to boot from removable storage */
	EFI_MODE_ONESHOT,
	/* When the user has set the "enable-efi" variable. We will still boot
	 * SteamOS WITHOUT EFI if efibootmgr fails to boot anything.
	 */
	EFI_MODE_PERMANENT,
} g_deckard_efi_mode = EFI_MODE_DISABLED;

static struct deckard_esp_info efivar_store = { 0 };

bool deckard_efi_enabled(void) {
	return g_deckard_efi_mode != EFI_MODE_DISABLED;
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

extern int dwc3_glue_bind(struct udevice *parent);

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

int pon_reason = 0;

static int deckard_do_load_logo(char * logo_name)
{
	#define DEVPARTMAXLEN 16
	#define LOADADDRMAXLEN 16
	#define LOGO_PATHMAX 256

	int  argc = 5;
	char *argv[5];
	char arg0[] = "do_the_dew";
	char part_arg[DEVPARTMAXLEN] = { 0 };  //e.g. "1:d"
	char loadaddr_arg[LOADADDRMAXLEN] = { 0 }; //e.g. "0xa7000000"
	char logo_arg[LOGO_PATHMAX + 1] = { 0 };

	argv[0] = arg0;
	argv[1] = DECKARD_IFNAME;
	argv[2] = part_arg;
	argv[3] = loadaddr_arg;
	argv[4] = logo_arg;

	/* Setup load address */
	snprintf(loadaddr_arg, LOADADDRMAXLEN, "0x%x", CONFIG_SYS_LOAD_ADDR);

	/* First try loading from the ESP */
	snprintf(part_arg, DEVPARTMAXLEN, DECKARD_ESP_PARTITION);
	snprintf(logo_arg, LOGO_PATHMAX, "/steamos/%s", logo_name); //not case sensitive
	if (!do_load(NULL, 0, argc, argv, FS_TYPE_ANY)) {
		log_debug("Deckard: loaded splash from ESP!\n");
		return 0;
	}
	log_warning("Deckard: using splash image from firmware partition\n");

	/* Fall back to firmware partition */
	snprintf(part_arg, DEVPARTMAXLEN, "%s", deckard_get_fw_part()); //this returns 1:d for example
	snprintf(logo_arg, LOGO_PATHMAX, "/%s", logo_name);

	return do_load(NULL, 0, argc, argv, FS_TYPE_ANY);
}

void deckard_load_logo(enum full_screen_images_t image)
{
	int ret = 0;
	
	if(!g_display_enabled) return;

	switch(image)
	{
		case BOOT_LOGO:
			ret = deckard_do_load_logo("logo.bmp.gz");
			break;

		case BATTERY_CHARGING:
			ret = deckard_do_load_logo("logo_battery_charging.bmp.gz");
			break;

		default:
			ret = -1;
			break;
	}

	if ( ret != 0 )
		return;

	bmp_info(CONFIG_SYS_LOAD_ADDR);
	bmp_display(CONFIG_SYS_LOAD_ADDR, 0,0);
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

	/* Read PON reason from pmk8550 (which is from AOP SDAM3) */
	pon_reason = pmk8550_get_pon_reason(pmk8550);

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

int enable_ramoops_serial_console(void)
{
	env_set("stdin","serial,usbacm");
	env_set("stderr","serial,ramoops,usbacm");
	env_set("stdout","serial,ramoops,usbacm");
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
			env_set("reboot_mode", "f");
			break;
		case VOLUP_OFF__VOLDN_ON__SELECT_ON:
			g_deckard_efi_mode = EFI_MODE_ONESHOT;
			/* Initialise USB to detect external storage before EFI comes up */
			usb_init();
			break;
		case VOLUP_ON__VOLDN_OFF__SELECT_OFF:
			break;
		case VOLUP_ON__VOLDN_OFF__SELECT_ON:
			enable_usb_serial_console();
			env_set("bootdelay","20");
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

static char *deckard_get_dev_part_str(const char *part_name)
{
	char dev_part_str[MAX_DEV_PART_STR];
	int ret;

	ret = part_get_dev_part_str_by_name(DECKARD_IFNAME, part_name,
					    dev_part_str);
	if (ret < 0)
		return NULL;

	return strdup(dev_part_str);
}

static char *deckard_get_dev_str(const char *part_name)
{
	return strtok(deckard_get_dev_part_str(part_name), ":");
}

const char *env_scsi_get_dev_part(void)
{
	ofnode node;
	const char *bootdev;

	node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			printf("No u-boot,spl-boot-device, considering slot\n");
			return deckard_get_dev_part_str(DECKARD_UBOOT_ENV_SLOT_A);
		} else {
			printf("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
			case 'A':
				return deckard_get_dev_part_str(DECKARD_UBOOT_ENV_SLOT_A);
				break;
			case 'B':
				return deckard_get_dev_part_str(DECKARD_UBOOT_ENV_SLOT_B);
				break;
			default:
				printf("%s: Invalid booted_slot, defaulting to A env\n", __func__);
				return deckard_get_dev_part_str(DECKARD_UBOOT_ENV_SLOT_A);
				break;
			}
		}
	} else {
		printf("No chosen node default to A!\n");
		return deckard_get_dev_part_str(DECKARD_UBOOT_ENV_SLOT_A);
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
		default:
			return deckard_get_dev_part_str(DECKARD_UBOOT_FW_SLOT_A);
		case 'B':
			return deckard_get_dev_part_str(DECKARD_UBOOT_FW_SLOT_B);
	}
}

const char *fs_firmware_get_partition(void)
{
	return deckard_get_fw_part();
}

static void deckard_reboot_mode_clean(void)
{
	if (!env_get("reboot_mode"))
		return;

	env_set("reboot_mode", "");
	env_save();
	mdelay(1000);
}


static int deckard_unbind_usb(void)
{
	struct device_node *snps_np = NULL;
	struct udevice *dev = NULL;
	ofnode node;
	int ret;

	snps_np = of_find_compatible_node(snps_np, NULL, "snps,dwc3");
	if (!snps_np) {
		log_err("Failed to find snps node\n");
		return -ENOENT;
	}

	/* overwrite dr_mode property in child */
	ret = of_write_prop(snps_np, "dr_mode", strlen("host") + 1, "host");
	if (ret) {
		log_err("Failed to overwrite 'dr_mode' property: %d\n", ret);
		return ret;
	}

	/* child node */
	node = ofnode_by_compatible(ofnode_null(), "snps,dwc3");
	if (!ofnode_valid(node)) {
		log_err("Failed to find glue node\n");
		return -ENOENT;
	}

	ret = device_find_global_by_ofnode(node, &dev);
	if (ret < 0) {
		log_err("failed to get device from node\n");
		return -ENOENT;
	}

	ret = device_remove(dev, DM_REMOVE_NORMAL);
	if (ret) {
		printf("Unable to remove. err:%d\n", ret);
		return ret;
	}

	ret = device_unbind(dev);
	if (ret) {
		printf("Unable to unbind. err:%d\n", ret);
		return ret;
	}

	return 0;
}

static int deckard_reboot_mode_usb(void)
{
	struct device_node *dwc3_np = NULL;
	struct udevice *parent = NULL;
	ofnode node;
	int ret;

	/* Find the DWC3 node itself */
	dwc3_np = of_find_compatible_node(dwc3_np, NULL, "qcom,dwc3");
	if (!dwc3_np) {
		log_err("Failed to find dwc3 node\n");
		return -ENOENT;
	}

	/* overwrite dr_mode property in parent */
	ret = of_write_prop(dwc3_np, "dr_mode", strlen("host") + 1, "host");
	if (ret) {
		log_err("Failed to overwrite 'dr_mode' property: %d\n", ret);
		return ret;
	}

	node = ofnode_by_compatible(ofnode_null(), "qcom,dwc3");
	if (!ofnode_valid(node)) {
		log_err("Failed to find glue node\n");
		return -ENOENT;
	}

	/*
	 * Unbind the already bind usb dwc3-generic-host or dwc3_generic_peripheral
	 * driver to device, and bind our own to make sure everything is setup
	 * correctly.
	 */
	ret = deckard_unbind_usb();
	if (ret < 0) {
		log_err("failed to unbind child node\n");
		return -ENOENT;
	}

	ret = device_find_global_by_ofnode(node, &parent);
	if (ret < 0) {
		log_err("failed to get parent device from node\n");
		return -ENOENT;
	}

	ret = dwc3_glue_bind(parent);
	if (ret < 0) {
		log_warning("failed to bind\n");
		return -ENOENT;
	}

	return 0;
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
			(reason != FASTBOOT_REBOOT_REASON_OEM_USB) &&
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
		env_set("reboot_mode", "e");
		env_save();
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_USB) {
		printf("Setting reboot to USB mode ...\n");
		env_set("reboot_mode", "u");
		env_save();
	}

	return 0;
}
#endif

int led_set_color_and_state(char * led_name, enum led_state_t state)
{
	struct udevice  *led;
	int ret = led_get_by_label(led_name, &led);
	if ( ret < 0) {
		printf("Couldn't find led!\n");
		return ret;
	}

	led_set_state(led, state);

	return ret;
}

/* Get the devnum/partnum for a partition from its GPT partlabel */
static int deckard_find_part_by_label(const char *partname, int *devnum, int *partnum)
{
	int r;
	struct udevice *blkdev;
	struct blk_desc *desc;
	struct uclass *blk_class;
	struct disk_partition part;

	uclass_get(UCLASS_BLK, &blk_class);

	/* Devices are already probed! */
	uclass_foreach_dev(blkdev, blk_class) {
		if (device_get_uclass_id(blkdev) != UCLASS_BLK)
			continue;

		desc = dev_get_uclass_plat(blkdev);
		if (!desc || desc->part_type == PART_TYPE_UNKNOWN)
			continue;
		/* Only look for non-removable SCSI devices (internal storage) */
		// FIXME: we might need a more reliable way to identify internal storage!
		if (desc->uclass_id != UCLASS_SCSI || desc->removable)
			continue;

		for (*partnum = 1;; (*partnum)++) {
			r = part_get_info(desc, *partnum, &part);
			/* Last partition, go next */
			if (r < 0)
				break;
			/* Skip if the name doesn't match */
			if (strncmp(partname, part.name, strlen(partname)))
				continue;
			// printf("Found EFI partition '%s' (%d:%d) blksz %#lx\n", part.name, desc->devnum, *partnum, desc->blksz);
			*devnum = desc->devnum;

			return 0;
		}
	}

	return -ENODEV;
}

static int deckard_find_efi_var_store(void)
{
	char partname[16] = { 0 };
	char *slot;

	if (efivar_store.valid)
		return 0;

	slot = env_get("booted_slot");
	/* Build the partition name (efi-A/efi-B) */
	strlcat(partname, "efi-", 16);
	strlcat(partname, slot, 16);

	if (!deckard_find_part_by_label(partname, &efivar_store.devnum, &efivar_store.partnum)) {
		efivar_store.valid = true;
		return 0;
	}

	return -ENOENT;
}

static int deckard_efi_setup(void *ctx, struct event *event)
{
	efi_status_t ret;

	/* Get the EFI subsystem up and running! */
	ret = efi_init_obj_list();
	if (ret != EFI_SUCCESS) {
		printf("EFI: Initialisation failed!\n");
		return 0;
	}

	/*
	* FIXME: for the golden master we will not support booting SteamOS with EFI, so skip creating the
	* boot entry for it and setting BootOrder. We will instead allow bootmgr to set BootOrder based on
	* detected removable storage devices (USB drive, sdcard).
	*
	* After GM, we can re-add the logic here to set up the SteamOS boot entry and use BootNext for
	* the "oneshot" removable storage boot mode.
	*/

	return 0;
}

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice     *btn = NULL;
	struct udevice *led_red = NULL;
	struct udevice *led_green = NULL;
	struct udevice *led_blue = NULL;

	int                 ret;
	enum button_state_t state;
	char * bootedslot;
	char *reboot_mode;
	char *eeprom_board;
	int fastbootmode = 0;
	int bootloadermode = 0;
	int edlmode = 0;
	int usbmode = 0;

	ofnode node;
	enum combined_button_state_t total_button_state = 0;
	int power_button_state = 0;
	
	node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		const char *bootdev;
		u32 boot_wrapper_version;

		bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			printf("No u-boot,spl-boot-device, considering A slot\n");
			env_set("booted_slot","A");
		} else {
			printf("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
			case 'A':
				env_set("booted_slot","A");
				break;
			case 'B':
				env_set("booted_slot","B");
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

	deckard_disable_watchdog();

	deckard_setup();

	//default to display being ON if the variable is undefined
	if ( (env_get_ulong("display", 10, 1) == 1)) {
		g_display_enabled = true;
		printf("DISPLAY ON\n");
	}
	else
	{
		g_display_enabled = false;
		printf("DISPLAY OFF\n");
		display_turn_off_force(); //force just means dont check if g_display_enabled is true
	}

	if ( !env_get("factory-mode") ) {
		deckard_load_logo(BOOT_LOGO);
	}

	ret = button_get_by_label("Volume Down", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol- button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol- button pressed\n");
			total_button_state |= VOLDN_BUTTON_MASK;
		}
	}

	ret = button_get_by_label("Volume Up", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol+ button!\n");
	} else {
		state = button_get_state(btn);
		if (state == BUTTON_ON) {
			printf("Vol+ button pressed\n");
			total_button_state |= VOLUP_BUTTON_MASK;
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

	ret = button_get_by_label("Select", &btn);
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

	/*
	 * Check if we have a special boot command
	 * f - for fastboot and b - to stay in bootloader
	 * mode, i.e, interrupt autoboot
	 * and e - for edl mode
	 */
	reboot_mode = env_get("reboot_mode");
	if (reboot_mode && (reboot_mode[0] == 'f' ))
		fastbootmode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'b' ))
		bootloadermode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'e' ))
		edlmode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'u' ))
		usbmode = 1;

	/* cleanup all reboot_mode setups if any */
	deckard_reboot_mode_clean();

	ret = led_get_by_label("red", &led_red);
	if (ret < 0) {
		printf("Couldn't find red led!\n");
	} else {
		led_set_state(led_red, LEDST_ON);
	};

	ret = led_get_by_label("green", &led_green);
	if (ret < 0) {
		printf("Couldn't find green led!\n");
	} else {
		led_set_state(led_green, LEDST_ON);
	};

	ret = led_get_by_label("blue", &led_blue);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
		led_set_state(led_blue, LEDST_ON);
	};

	// get board data from eeprom
	deckard_eeprom_init();

	env_set("UBOOTLUNIDX", deckard_get_dev_str(DECKARD_UBOOT_SLOT_A));
	env_set("SYSTEMLUNIDX", deckard_get_dev_str(DECKARD_ROOTFS_SLOT_A));

	/*
	 * Set the magic $fdtfile variable so we load the right DTB during EFI boot. This will be used
	 * to populate the EFI configuration table, it can be overwritten by an OS Loader.
	 */
	eeprom_board = env_get("EEPROM_BOARD");
	if (eeprom_board) {
		char fdtfile[32];
		snprintf(fdtfile, 32, "qcom/sm8650-%s.dtb", eeprom_board);
		env_set("fdtfile", fdtfile);
	}

	// do we still need this?
	bootedslot = env_get("booted_slot");
	env_set("BOOT_ORDER", bootedslot);
	printf("booted_slot: %s\n", env_get("booted_slot"));

	/* Check if special boot mode (fastboot, bootloader, edl) was setup */
	if(fastbootmode) {
		printf("Starting fastboot mode...\n");
		env_set("kernel_addr_r","90000000"); //is this right?
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

		led_set_color_and_state("red",LEDST_BLINK);
		led_set_color_and_state("green",LEDST_BLINK);
		led_set_color_and_state("blue",LEDST_BLINK);

		/* disable display (logo off) when idle in fastboot mode */
		display_turn_off();

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

	if (usbmode) {
		printf("Starting usb mode...\n");
		ret = deckard_reboot_mode_usb();
		if (!ret) {
			run_command("usb start", 0);
		}
	}

	// if requested, use the default env boot variables
	// otherwise override with deckard custom boot command
	if ( !env_get("use_custom_boot") ){
		env_set("bootcmd", "deckard_boot");
	}

	/* Check if EFI is enabled (enable-efi=1) */
	if (env_get(ENV_EFI_ENABLED) && !strcmp(env_get(ENV_EFI_ENABLED), "1"))
		g_deckard_efi_mode = EFI_MODE_PERMANENT;

	// If EFI is enabled (either oneshot or permanently) then bring up the EFI subsystem
	if (g_deckard_efi_mode) {
		if (!deckard_find_efi_var_store())
			/* Trigger EFI setup just before the main loop */
			event_register("deckard_bootefi", EVT_MAIN_LOOP, deckard_efi_setup, NULL);
		else
			printf("EFI: Couldn't find EFI variable store partition!\n");
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

	/* find or create "/chosen" node. */
	nodeoffset = fdt_find_or_add_subnode(fdt, 0, "chosen");
	if (nodeoffset < 0)
		return nodeoffset;

	/* add bootwrapper version */
	boot_wrapper_version = env_get("boot_wrapper_version");
	ret = fdt_setprop_string(fdt, nodeoffset, "bootwrapper,version", boot_wrapper_version ? boot_wrapper_version : "-1");
	if (ret < 0) {
		printf("WARNING: could not set bootwrapper,version %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

	/* add charger capability */
	//this will indicate that u-boot has the ability to do charging within u-boot
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,charger", "1");
	if (ret < 0) {
		printf("WARNING: could not set u-boot,charger %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

	/* add display capability */
	//this will indicate that u-boot has the ability to driver display within u-boot
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,display", g_display_enabled ? "1": "0");
	if (ret < 0) {
		printf("WARNING: could not set u-boot,display %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
	/* add old rootfs fallback capability */
	//this will indicate that u-boot has the ability to fall back to old rootfs
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,oldrootfssupport", "1"); //set this to 1 when display works
	if (ret < 0) {
		printf("WARNING: could not set u-boot,oldrootfs %s.\n",
			   fdt_strerror(ret));
		return ret;
	}
#endif

	/* Enable the Kernel simple-framebuffer if display is still enabled */
	if (g_display_enabled) {
		int offset;

		fdt_for_each_node_by_compatible(offset, fdt, -1, "simple-framebuffer") {
			ret = fdt_setprop_string(fdt, offset, "status", "okay");
			if (ret < 0) {
				printf("WARNING: could not enable simple-frambuffer %s.\n",
			   		fdt_strerror(ret));
				return ret;
			}
		}
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
	char arg1[] = DECKARD_IFNAME;
	int ret;

	argv[0]=arg0;
	argv[1]=arg1;
	argv[2]=partition; //e.g. "1:d"
	argv[3]=addr; //e.g. "0xa7000000"
	argv[4]=path; // e.g. "/boot/maindtb.dtb"
	
	//try with BTRFS first while ignoring the blkdev error, then if that fails, then try with EXT4
	ret = do_load(NULL, FS_DO_LOAD_IGNORE_BLKDEV_ERROR, argc, argv, FS_TYPE_BTRFS);
	if (ret) 
	{
		ret = do_load(NULL, 0, argc, argv, FS_TYPE_EXT);
	}

	return ret;
}

int power_button_pushed(struct udevice *btn)
{
	enum button_state_t state;
	state = button_get_state(btn);
	if (state == BUTTON_ON) {
		printf("PWR button pressed\n");
		return 1;
	}

	return 0;
}

int deckard_adsp_start(int id)
{
	int ret;

	if (!rproc_is_initialized()) {
		ret = rproc_init();
		if (ret < 0) {
			printf("deckard: failed to initialized rproc\n");
			return ret;
		}
	}

	ret = rproc_start(id);
	if (ret < 0) {
		printf("deckard: failed to start rproc\n");
		return ret;
	}

	return 0;
}

int deckard_check_for_key_press()
{
	if (tstc()) {	/* we got a key press	*/
		if(getchar() == 'j') return 1;
	}

	return 0;
}

bool deckard_shutdown_condition(struct udevice *pmic)
{
	static unsigned long timeout;

	if (!pmic)
		return false;

	if (deckard_battery_is_charging_enabled(pmic)) {
		if(timeout > 0)
			printf("charging enabled\n");

		timeout = 0;
		return false;
	}

	if (!timeout)
	{
		printf("charging disabled\n");
		timeout = timer_get_us() + DECKARD_SHUTDOWN_TIMEOUT_US;
	}

	if (time_after(timer_get_us(), timeout))
		return true;

	return false;
}

#if defined(CONFIG_SYS_CONSOLE_IS_IN_ENV) && \
defined(CONFIG_SYS_CONSOLE_OVERWRITE_ROUTINE)
int overwrite_console(void)
{
	return 0;
}
#endif

int display_turn_off()
{
	if(!g_display_enabled) return 0;

	if (env_get_ulong("keep_display", 10, 0) == 1)
		return 0;

	return display_turn_off_force();
}

int display_turn_off_force()
{
	return run_command("mdss stop", 0);
}


#if CONFIG_IS_ENABLED(SHOW_BOOT_PROGRESS)
void show_boot_progress(int progress)
{
	if (progress != BOOTSTAGE_ID_ENTER_CLI_LOOP)
		return;

	display_turn_off();
}
#endif

int deckard_prepare_for_boot()
{
	struct udevice  *led_red, *led_green, *led_blue;
	struct udevice *pm8550b = NULL;
	struct udevice *max17320 = NULL;
	int power_button_avail = 0;
	struct udevice *btn;
	int adsp_loaded = 0;
	int leds_avail = 0;
	int id = 0;
	int soc;

	//check pon_reason to see if we were booted by a button push or warm reset (reboot command)

	printf("pon_reason: %d\n",pon_reason);
	if(pon_reason & 0x80)
	{
		printf("pon_reason includes button press, continuing to boot\n");
		return 0;
	}
	else if(pon_reason & 0x01)
	{
		printf("pon_reason includes warm reset, continuing to boot\n");
		return 0;
	}

	printf("Waiting for power button press to continue booting...\n");

	int ret_r = led_get_by_label("red", &led_red);
	int ret_g = led_get_by_label("green", &led_green);
	int ret_b = led_get_by_label("blue", &led_blue);
	if ( ret_r < 0 || ret_g < 0 || ret_b < 0 ) {
		printf("Couldn't find led!\n");
	}
	 else 
	{
		leds_avail = 1;
	}
	
	int ret = button_get_by_label("Power Button", &btn);
	if (ret < 0) 
	{
		printf("Couldn't find power button!\n");
	} 
	else
	{
		power_button_avail = 1;
	}

	if(power_button_avail && !power_button_pushed(btn))
	{
		//start up adsp
		printf("adsp: starting...\n");
		ret = deckard_adsp_start(id);
		if (!ret)
			adsp_loaded = 1;
	}
	
	// Find the PM8550B
	ret = pmic_get("pmic@7", &pm8550b);
	if (ret != 0)
		printf("Failed to Find PM8550B\n");
	
	// Find the max17320
	ret = pmic_get("pmic@36", &max17320);
	if (ret != 0) {
		printf("Failed to Find max17320\n");
	} else {
		// get battery capacity
		soc = deckard_battery_get_capacity(max17320);
		printf("Battery: %d %%\n", soc);
	}

	int deckard_shutdown_loop_counter = 1;

	deckard_load_logo(BATTERY_CHARGING);	

	int deckard_display_off_counter = 20; // 100ms * 20 = 2s
	//trap in uboot while we wait for power button to be pushed
	//we should charge and do various things here like light up the LED's etc
	while(power_button_avail)
	{
		if(power_button_pushed(btn))
			break;

#if 0
		//may want to check for keyboard press for some reason or another here
		if(deckard_check_for_key_press())
		{
			key_pressed = 1;
			break;
		}
#endif

		//check every 10 loops to avoid spamming the SPMI bus
		if ( deckard_shutdown_loop_counter-- == 0 )
		{
			deckard_shutdown_loop_counter = 10;
			if(deckard_shutdown_condition(pm8550b)) 
			{
				printf("Device not charging for %d seconds, Shutdown\n", DECKARD_SHUTDOWN_TIMEOUT_S);
				run_command("splctl set-uboot-poweroff-reason battery", 0); //inform the spl that we are shutting down due to low battery
				ret = sysreset_walk(SYSRESET_POWER_OFF);
				if (ret == -EINPROGRESS)
					mdelay(1000);
				break;
			}
		}
		
		if ( deckard_display_off_counter-- == 0 )
		{
			display_turn_off();
		}

		//polling loop -- maybe be interrupt driven somehow?
		mdelay(100);
	}

	if (adsp_loaded)
	{
		printf("adsp: stopping...\n");
		rproc_stop(id);
	}
		
	if(leds_avail)
	{
		led_set_state(led_red, LEDST_ON);
		led_set_state(led_green, LEDST_ON);
		led_set_state(led_blue, LEDST_ON);
		mdelay(100);
		led_set_state(led_red, LEDST_OFF);
		led_set_state(led_green, LEDST_OFF);
		led_set_state(led_blue, LEDST_OFF);
		mdelay(100);
		led_set_state(led_red, LEDST_ON);
		led_set_state(led_green, LEDST_ON);
		led_set_state(led_blue, LEDST_ON);
	}

	return 0;
}

/* EFI booting with EFI vars on a custom partition */
efi_status_t efi_set_blk_dev_to_system_partition(void)
{
	int r;
	char part_str[10];

	r = deckard_find_efi_var_store();
	if (r)
		return EFI_DEVICE_ERROR;
	// It would be nice to use fs_set_blk_dev_with_part() but it doesn't seem to behave quite right
	snprintf(part_str, sizeof(part_str), "%x:%x",
		 efivar_store.devnum, efivar_store.partnum);
	// printf("%s: %s\n", __func__, part_str);
	r = fs_set_blk_dev(blk_get_uclass_name(efi_system_partition.uclass_id),
			   part_str, FS_TYPE_ANY);
	if (r)
		return EFI_DEVICE_ERROR;

	return EFI_SUCCESS;
}

// per arm64 linux boot documenation, default text_offset is 0x80000
#define ADDR_KERNEL "0x88000000"
// this gives us roughly 290 Megabytes for kernel, dtb, and initrd
#define MAX_ADDR_INITRD 0x99000000

// find dtb path based on env and eeprom settings, load dtb from flash to memory
// returns 0 on success
// returns -1 on failure
int deckard_load_dtb(char * partition, char * addr)
{
	char * dtb_qcom;
	char * eeprom_board;
	char DTB_TARGET[PATHMAXLEN];

	// Get DTB
	dtb_qcom = env_get("DTB_QCOM");
	eeprom_board = env_get("EEPROM_BOARD");

	// try custom dtb from fw env
	if (dtb_qcom) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/%s", dtb_qcom);
		// load dtb
		if ( scsi_do_load_helper(partition, addr, DTB_TARGET) == 0 ) {
			return 0;
		}
	}

	// try dtb set in eeprom
	if (eeprom_board) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/sm8650-%s.dtb", eeprom_board);
		// load dtb
		if ( scsi_do_load_helper(partition, addr, DTB_TARGET) == 0 ) {
			return 0;
		}
	}

	// try defualt dtb
	snprintf(DTB_TARGET, PATHMAXLEN, "/boot/maindtb.dtb");
	return scsi_do_load_helper(partition, addr, DTB_TARGET);
}

// Deckard boot command.  Executes sequence that would normally be in the boot.scr
int do_deckard_boot(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char * bootedslot;
	char * KERNEL_TARGET = "/boot/Image";
	char * INITRD_TARGET = "/boot/initrd.uImage";
	char *partition;
	char label[DEVPARTMAXLEN];
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
		default:
			partition = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_A);
			strncpy(label, DECKARD_ROOTFS_SLOT_A, DEVPARTMAXLEN);
			break;
		case 'B':
			partition = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_B);
			strncpy(label, DECKARD_ROOTFS_SLOT_B, DEVPARTMAXLEN);
			break;
	}

#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
	if(!partition)
	{
		printf("Trying old rootfs partition names...\n");
		switch(bootedslot[0]){
			case 'A':
			default:
				partition = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_OLD_A);
				strncpy(label, DECKARD_ROOTFS_SLOT_OLD_A, DEVPARTMAXLEN);
				break;
			case 'B':
				partition = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_OLD_B);
				strncpy(label, DECKARD_ROOTFS_SLOT_OLD_B, DEVPARTMAXLEN);
				break;
		}
	}
#endif

	// Do not run adsp charging if we are in factory mode or coming out of ship mode
	if ( !env_get("factory-mode") && !env_get("ship-mode") ) {
		deckard_prepare_for_boot();
	}

	if ( env_get("ship-mode") ) {
		// clear ship-mode ( used to boot straight to OS if coming out of ship mode )
		env_set("ship-mode", "");
		env_save();
	}

	// FIXME: eventually invert this so we try EFI first, maybe find a less hacky way to do this?
	if (g_deckard_efi_mode) {
		printf("Attempting to boot with EFI!\n");
		run_command("bootefi bootmgr", 0);
	}

	printf("Attempting to load kernel from %s [ %s ]\n", label, partition);

	// Load kernel
	if ( scsi_do_load_helper(partition, ADDR_KERNEL, KERNEL_TARGET) ) {
		printf("Failed to load kernel from %s [ %s ]\n", label, partition);
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
	if ( deckard_load_dtb(partition, DTB_ADDR) ) {
		printf("Failed to load dtb from %s [ %s ]\n", label, partition);
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
		printf("Failed to load initrd from %s [ %s ]\n", label, partition);
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
	snprintf(bootargs, PATHMAXLEN, "%s %s root=PARTLABEL=%s init=/sbin/init rauc.slot=%s noresume rootwait earlycon",
									default_bootargs ? default_bootargs : "", temp_bootargs ? temp_bootargs : "", label, bootedslot );
	env_set("bootargs", bootargs);

	// erase temp bootargs which is used for Testing
	env_set("temp_bootargs", "");

	env_set("kernel_addr_r", ADDR_KERNEL);
	env_set("ramdisk_addr_r", INITRD_ADDR);
	env_set("fdtcontroladdr", DTB_ADDR);

	display_turn_off();
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

int do_deckard_logo(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	deckard_load_logo(BOOT_LOGO);
	return 0;
}

U_BOOT_LONGHELP(deckard_logo,
    "Show Deckard logo\n"
);

U_BOOT_CMD(
	deckard_logo, CONFIG_SYS_MAXARGS,     1,      do_deckard_logo,
    "Deckard display logo", deckard_logo_help_text
);

