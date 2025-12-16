// SPDX-License-Identifier: GPL-2.0+
/*
 * Board init file for Dragonboard 410C
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 */

#include <bootstage.h>
#include <dm.h>
#include <efi_config.h>
#include <efi_loader.h>
#include <efi_variable.h>
#include <env.h>
#include <fastboot.h>
#include <malloc.h>
#include <usb.h>
#include <sysreset.h>
#include <asm/io.h>
#include <asm/global_data.h>
#include <fdt_support.h>
#include <linux/delay.h>
#include <button.h>
#include <power/pmic.h>
#include <command.h>
#include <dm/device-internal.h>
#include <fs.h>
#include <image.h>
#include <sysreset.h>
#include <splash.h>
#include <time.h>
#include <video.h>
#include <ansi.h>
#include "pm8550vs.h"
#include "smb1510.h"
#include "pmk8550.h"
#include <string.h>

#include "deckard.h"
#include "deckard_adsp.h"
#include "deckard_battery.h"
#include "deckard_bootmenu.h"
#include "deckard_devices.h"
#include "deckard_disk.h"
#include "deckard_eeprom.h"
#include "deckard_log.h"

enum boot_priority {
	BOOT_INTERNAL_ONLY,
	BOOT_INTERNAL_FIRST,
	BOOT_REMOVABLE_FIRST,
};

#define DECKARD_DUMMY_SERIAL_NUMB "123456789"
#define TCSR_BOOT_MISC_DETECT 0x01fd9000

enum full_screen_images_t 
{
	IMAGE_BOOT_LOGO,
	IMAGE_BATTERY_DEAD,
	IMAGE_ERROR
};

bool g_display_enabled = false;
bool g_factory_mode = false;
boot_intention_t g_boot_intention = BOOT_INTENTION_START_USERSPACE;
static deckard_devices_t g_deckard_devices = { 0 };

int display_turn_off(void);
int display_turn_off_force(void);
static void deckard_load_and_display_image_for_boot_intention( boot_intention_t boot_intention );
int deckard_check_for_key_press(void);
static void deckard_wait_to_boot( boot_intention_t * p_boot_intention, deckard_devices_t * pdevices );

// Normal physical uart serial port
#define SERIAL_CONSOLE_MASK 0b100
// USB_CONSOLE_MASK emulates a serial port device over type-c
#define USB_CONSOLE_MASK 0b010
// VIDEO_CONSOLE_MASK is the VR specific console (when enabled)
// requires the display to be enabled
#define VIDEO_CONSOLE_MASK 0b001
enum combined_serial_console_state_t {
	SERIAL_OFF__USB_OFF__VID_OFF = 0,
	SERIAL_OFF__USB_OFF__VID_ON  = 1,
	SERIAL_OFF__USB_ON__VID_OFF  = 2,
	SERIAL_OFF__USB_ON__VID_ON   = 3,
	SERIAL_ON__USB_OFF__VID_OFF  = 4,
	SERIAL_ON__USB_OFF__VID_ON   = 5,
	SERIAL_ON__USB_ON__VID_OFF   = 6,
	SERIAL_ON__USB_ON__VID_ON    = 7,
};
enum combined_serial_console_state_t g_total_console_state = 0;


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

bool deckard_efi_enabled(void) {
	return g_deckard_efi_mode != EFI_MODE_DISABLED;
}

int enable_factory_reset_if_needed(void);

// #define CHECK_PMICS
#ifdef CHECK_PMICS
void check_pmic(const char *const pmic_dts, const char *const pmic_name)
{
	int             ret;
	struct udevice *pmic = NULL;

	ret = pmic_get(pmic_dts, &pmic);
	if (ret != 0) {
		deckard_log("Failed to Find %s - %s, ret: %d\n", pmic_dts, pmic_name, ret);
		return;
	}

	ret = pmic_reg_read(pmic, 0x0105);
	if (ret < 0) {
		deckard_log("%s failed: %d\n", pmic_name, ret);
	}
	// else
	// {
	// 	deckard_log("%s subtype: 0x%02X\n", pmic_name, ret);
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

int usb_setup(void)
{
	return run_command("usb start", 0);
}

static int deckard_load_image(char * logo_name)
{
	/* Setup load address */
	char loadaddr_arg[LOADADDRMAXLEN] = { 0 }; //e.g. "0xa7000000"
	snprintf(loadaddr_arg, LOADADDRMAXLEN, "0x%x", CONFIG_SYS_LOAD_ADDR);

	char logo_arg[LOGO_PATHMAX + 1] = { 0 };
	snprintf(logo_arg, LOGO_PATHMAX, "/steamos/%s", logo_name); //not case sensitive

	char deckard_firmware_part[DEVPARTMAXLEN] = { 0 }; //e.g. "1:d"
	if ( deckard_get_dev_part_str_buf( deckard_get_booted_firmware_slotname(),
		&deckard_firmware_part[0], sizeof(deckard_firmware_part ) ) != 0 )
	{
		return 1;
	}

	char *argv[] = { "",
			disk_interface_to_str( booted_from_usb() ? DISK_INTERFACE_USB : DISK_INTERFACE_SCSI ),
			deckard_firmware_part, loadaddr_arg, logo_arg };
	int argc = 5;
	if (!do_load(NULL, 0, argc, argv, FS_TYPE_ANY)) {
		log_debug("Deckard: loaded splash from ESP!\n");
		return 0;
	}
	log_warning("Deckard: using splash image from firmware partition\n");


	snprintf(logo_arg, LOGO_PATHMAX, "/%s", logo_name);

	return do_load(NULL, 0, argc, argv, FS_TYPE_ANY);
}

// TODO: confirm that this isn't leaking memory 
// Ideally, also add a comment as to what this does for memory ownership

static void deckard_load_and_display_image_for_boot_intention( boot_intention_t boot_intention )
{
	if(!g_display_enabled) return;

	int ret = 0;
	switch(boot_intention)
	{
		case BOOT_INTENTION_CHARGE_AND_WAIT:
			// Dont show an image on BOOT_INTENTION_CHARGE_AND_WAIT!
			return;
		case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:
			ret = deckard_load_image("logo_battery_low.bmp.gz");
			break;
		case BOOT_INTENTION_BOOT_MENU:
			ret = deckard_load_image("logo_error.bmp.gz");
			break;
		default:
			ret = deckard_load_image("logo.bmp.gz");
			break;
	}

	if ( ret != 0 )
		return;

	bmp_info(CONFIG_SYS_LOAD_ADDR);
	bmp_display(CONFIG_SYS_LOAD_ADDR, 0,0);
}

int console_state_handler(void) 
{
	fflush(stdout); //needed to make things not explode
	fflush(stderr); //needed to make things not explode

	//note: we need to have some console, so no console on at all is not valid, so we handle that below
	switch(g_total_console_state)
	{
		case SERIAL_OFF__USB_OFF__VID_OFF:
			env_set("stdin", "serial");
			env_set("stderr","serial");
			env_set("stdout","serial");
			break;
		case SERIAL_OFF__USB_OFF__VID_ON:
			env_set("stdin", "serial");
			env_set("stderr","serial");
			env_set("stdout","serial,vidconsole");
			break;
		case SERIAL_OFF__USB_ON__VID_OFF:
			env_set("stdin", "usbacm");
			env_set("stderr","usbacm");
			env_set("stdout","usbacm");
			break;
		case SERIAL_OFF__USB_ON__VID_ON:
			env_set("stdin", "usbacm");
			env_set("stderr","usbacm");
			env_set("stdout","usbacm,vidconsole");
			break;
		case SERIAL_ON__USB_OFF__VID_OFF:
			env_set("stdin", "serial");
			env_set("stderr","serial");
			env_set("stdout","serial");
			break;
		case SERIAL_ON__USB_OFF__VID_ON:
			env_set("stdin", "serial");
			env_set("stderr","serial");
			env_set("stdout","serial,vidconsole");
			break;
		case SERIAL_ON__USB_ON__VID_OFF:
			env_set("stdin", "serial,usbacm");
			env_set("stderr","serial,usbacm");
			env_set("stdout","serial,usbacm");
			break;
		case SERIAL_ON__USB_ON__VID_ON:
			env_set("stdin", "serial,usbacm");
			env_set("stderr","serial,usbacm");
			env_set("stdout","serial,usbacm,vidconsole");
			break;
		default:
			break;
	}

	return 0;
}

int enable_ramoops_serial_console(void)
{
	env_set("stdin","serial,usbacm");
	env_set("stderr","serial,ramoops,usbacm");
	env_set("stdout","serial,ramoops,usbacm");
	return 0;
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
		deckard_log("Unable to remove. err:%d\n", ret);
		return ret;
	}

	ret = device_unbind(dev);
	if (ret) {
		deckard_log("Unable to unbind. err:%d\n", ret);
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
		deckard_log("Setting reboot to fastboot flag ...\n");
		env_set("reboot_mode", "f");
		env_save();
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_EDL) {
		deckard_log("Rebooting to EDL mode ...\n");
		env_set("reboot_mode", "e");
		env_save();
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_USB) {
		deckard_log("Setting reboot to USB mode ...\n");
		env_set("reboot_mode", "u");
		env_save();
	}

	return 0;
}
#endif

static int deckard_efi_setup(void *ctx, struct event *event)
{
	efi_status_t ret;

	/* Get the EFI subsystem up and running! */
	ret = efi_init_obj_list();
	if (ret != EFI_SUCCESS) {
		deckard_log("EFI: Init failed!\n");
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

void setup_boot_intention_global_state( boot_intention_t boot_intention )
{
	switch ( boot_intention )
	{
		case BOOT_INTENTION_BOOT_FROM_USB:
			g_deckard_efi_mode = EFI_MODE_ONESHOT;
			usb_init(); // Initialise USB to detect external storage before EFI comes up
			break;
		case BOOT_INTENTION_FASTBOOT:
			env_set("reboot_mode", "f");
			break;
		case BOOT_INTENTION_CONSOLE_WAIT:
			g_total_console_state |= USB_CONSOLE_MASK;
			g_total_console_state |= VIDEO_CONSOLE_MASK; // warning: display must be on for this to not fail
			env_set("bootdelay","20");
			break;
		case BOOT_INTENTION_BOOTLOADER:
			g_total_console_state |= USB_CONSOLE_MASK;
			env_set("bootdelay","-1");
			break;
		default:
			break;
	}
}

/* EARLY ENTRY POINT */
int misc_init_r(void)
{
	printf("\n");
	deckard_log( "********** misc_init_r **********\n" );

	ofnode node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		const char *bootdev;
		u32 boot_wrapper_version;

		bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			deckard_log("No u-boot,spl-boot-device, considering A slot\n");
			env_set("booted_slot","A");
		} else {
			deckard_log("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
			case 'A':
				env_set("booted_slot","A");
				break;
			case 'B':
				env_set("booted_slot","B");
				break;
			case 'U':
				env_set("booted_slot","U");
				break;
			default:
				env_set("booted_slot","A");
				break;
			}
		}

		int ret = ofnode_read_u32(node, "boot-wrapper-version", &boot_wrapper_version);
		if (ret)
			deckard_log("No boot wrapper version\n");
		else {
			deckard_log("Boot Wrapper version: %d\n", boot_wrapper_version);
			env_set_ulong("boot_wrapper_version", boot_wrapper_version);
		}

	} else {
		panic("No chosen node !\n");
	}

	deckard_log( "init_esp\n" );
	// TODO: This does set esp_part, but nobody uses it!
	init_esp();

	deckard_log( "deckard_disable_watchdog\n" );
	deckard_disable_watchdog();

	deckard_log( "deckard_setup_devices\n" );
	// TODO: pull out the prep battery for boot into a separate function
	int nsetuperror = deckard_setup_devices( &g_deckard_devices );

	deckard_log( "set led white\n" );
	deckard_setledcolor( LED_COLOR_WHITE, &g_deckard_devices );

	if( booted_from_usb() )
	{
		deckard_log( "usb_setup\n" );
		usb_setup(); // TODO: look at result?
	}

	if ( nsetuperror )
	{
		deckard_log("ERROR in deckard_setup_devices %d\n", nsetuperror );
		// TODO: if fatal error, show an error display screen if possible
	}

	g_factory_mode = deckard_factory_mode_enabled();
	deckard_log( "Factory mode: %d\n", g_factory_mode );

	g_display_enabled = g_factory_mode ? 0 : env_get_ulong( "display", 10, 1 ); // defaults to on in non-factory mode
	deckard_log("Display Enabled: %d\n", g_display_enabled );

	if ( !g_display_enabled )
	{
		// TODO: is this needed?
		// force just means dont check if g_display_enabled is true
		display_turn_off_force();
	}
	
	// Give devs the opportunity to use a big hammer to force stuff on
	if (env_get_ulong("vidconsole_enable", 10, 0) == 1)
	{
		g_total_console_state |= VIDEO_CONSOLE_MASK;
	}

	if (env_get_ulong("usbconsole_enable", 10, 0) == 1)
	{
		g_total_console_state |= USB_CONSOLE_MASK;
		env_set("bootdelay","20");
	}

	// CALC POWER-ON BOOT INTENTION
	g_boot_intention = calc_boot_intention( &g_deckard_devices, 0 );
	deckard_log( "g_boot_intention %d\n", g_boot_intention );

	// Set LED Color
	deckard_setledcolor(
		( g_boot_intention == BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY ) ? LED_COLOR_RED : LED_COLOR_WHITE,
		&g_deckard_devices );
	deckard_log( "set led to boot intention color\n" );

	// SHOW BOOT IMAGE
	if ( g_boot_intention == BOOT_INTENTION_CHARGE_AND_WAIT ||
		g_boot_intention == BOOT_INTENTION_CONSOLE_WAIT || 
		g_boot_intention == BOOT_INTENTION_BOOTLOADER )
	{
		// intentionally dont show an image. Also keeps the display off
		// (if it was previously off)
	}
	else if ( g_boot_intention == BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY )
	{
		// Dont immediately load and show the image. Doing so prior to DSP
		// charging may be too high of a risk of battery load if the battery is drained
		// on power up.  We DO attempt to show a low battery image
		// later on in the wait loop, as even if that browns out the unit a
		// suequent reset (then charge) is alright.
		// But doing so here could be a permenant boot loop under minimal power
		// conditions
	}
	else
	{
		// We've now decided we want to show the user a splash image.
		// Do it!
		deckard_log( "load and display image\n" );
		deckard_load_and_display_image_for_boot_intention( g_boot_intention );
	}

	// SETUP CUSTOM STATE TO EXECUTE ON BOOT INTENTION
	deckard_log( "set boot intention global state\n" );
	setup_boot_intention_global_state( g_boot_intention );

	g_total_console_state |= SERIAL_CONSOLE_MASK; //seems likely we always need this on otherwise will get crashes / sync abort handler loops?
	console_state_handler();

	/*
	 * Check if we have a special boot command
	 * f - for fastboot and b - to stay in bootloader
	 * mode, i.e, interrupt autoboot
	 * and e - for edl mode
	 */
	// TODO: Add enum for reboot_mode?
	char * reboot_mode = env_get("reboot_mode");
	int fastbootmode = 0;
	int edlmode = 0;
	if (reboot_mode && (reboot_mode[0] == 'f' ))
		fastbootmode = 1;
	else if (reboot_mode && (reboot_mode[0] == 'e' ))
		edlmode = 1;

	/* cleanup all reboot_mode setups if any */
	deckard_log( "deckard_reboot_mode_clean\n" );
	deckard_reboot_mode_clean();

	// get board data from eeprom
	deckard_log( "deckard_eeprom_init\n" );
	deckard_eeprom_init();

	/*
	 * Set the magic $fdtfile variable so we load the right DTB during EFI boot. This will be used
	 * to populate the EFI configuration table, it can be overwritten by an OS Loader.
	 */
	char * eeprom_board = env_get("EEPROM_BOARD");
	if (eeprom_board) {
		char fdtfile[32];
		snprintf(fdtfile, 32, "qcom/sm8650-%s.dtb", eeprom_board);
		env_set("fdtfile", fdtfile);
	}

	// do we still need this?
	char * bootedslot = env_get("booted_slot");
	env_set("BOOT_ORDER", bootedslot);
	deckard_log("booted_slot: %s\n", env_get("booted_slot"));

	/* Check if special boot mode (fastboot, bootloader, edl) was setup */
	if(fastbootmode) {
		deckard_log("Starting fastboot mode...\n");
		// TODO: make this consistent with the other user of kernel_addr_r, which has a leading 0x prefix
		env_set("kernel_addr_r","90000000");
		/*
		 * serial# is a special var, like ethaddr, they are defined as
		 * write-once as a protection, we want to maintain that protection
		 * writing it only once here, if not before, and avoid error after.
		 */
		if (!env_get("serial#")) {
			deckard_log("setting dummy serial number: %s\n", DECKARD_DUMMY_SERIAL_NUMB);
			env_set("serial#", DECKARD_DUMMY_SERIAL_NUMB);
			env_save();
		}

		deckard_setledcolor( LED_COLOR_WHITE, &g_deckard_devices );

		/* disable display (logo off) when idle in fastboot mode */
		display_turn_off();

		run_command("fastboot -l 0x880000000 -s 0x10000000 usb 0", 0);
	}

	if (edlmode) {
		deckard_log("Rebooting to EDL mode...\n");
		deckard_reboot_mode_edl();
	}

	if ( g_boot_intention == BOOT_INTENTION_BOOT_FROM_USB )
	{
		//TODO: gaber this is likely interfering with USB boot
		deckard_log("Starting usb mode...\n");
		int ret = deckard_reboot_mode_usb();
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
			deckard_log("EFI: Couldn't find EFI variable store partition!\n");
	}

	deckard_log("deckard init: done\n");

	return 0;
}

int ft_board_setup(void *fdt, struct bd_info *bd)
{
	/* find or create "/chosen" node. */
	int nodeoffset = fdt_find_or_add_subnode(fdt, 0, "chosen");
	if (nodeoffset < 0)
		return nodeoffset;

	/* add bootwrapper version */
	char * boot_wrapper_version = env_get("boot_wrapper_version");
	int ret = fdt_setprop_string(fdt, nodeoffset, "bootwrapper,version", boot_wrapper_version ? boot_wrapper_version : "-1");
	if (ret < 0) {
		deckard_log("WARNING: could not set bootwrapper,version %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

	/* add charger capability */
	//this will indicate that u-boot has the ability to do charging within u-boot
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,charger", "1");
	if (ret < 0) {
		deckard_log("WARNING: could not set u-boot,charger %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

	/* add display capability */
	//this will indicate that u-boot has the ability to driver display within u-boot
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,display", g_display_enabled ? "1": "0");
	if (ret < 0) {
		deckard_log("WARNING: could not set u-boot,display %s.\n",
			   fdt_strerror(ret));
		return ret;
	}

#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
	/* add old rootfs fallback capability */
	//this will indicate that u-boot has the ability to fall back to old rootfs
	ret = fdt_setprop_string(fdt, nodeoffset, "u-boot,oldrootfssupport", "1"); //set this to 1 when display works
	if (ret < 0) {
		deckard_log("WARNING: could not set u-boot,oldrootfs %s.\n",
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
				deckard_log("WARNING: could not enable simple-frambuffer %s.\n",
			   		fdt_strerror(ret));
				return ret;
			}
		}
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

static bool deckard_shutdown_condition(struct udevice *pmic)
{
	static unsigned long timeout;

	if (!pmic)
		return false;

	if (deckard_battery_is_charging_enabled(pmic)) {
		if(timeout > 0)
			deckard_log("[charging enabled]\n");

		timeout = 0;
		return false;
	}

	if (!timeout)
	{
		deckard_log("[charging disabled]\n");
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

int display_turn_on(void)
{
	struct udevice *dev;

	if(!g_display_enabled) return 0;

	return uclass_first_device_err(UCLASS_VIDEO, &dev);
}

int display_turn_off()
{
	if(!g_display_enabled) return 0;

	if (env_get_ulong("keep_display", 10, 0) == 1)
		return 0;

	return display_turn_off_force();
}

int display_turn_off_force()
{
	g_total_console_state &= ~VIDEO_CONSOLE_MASK;
	console_state_handler();
	return run_command("mdss stop", 0);
}


void show_boot_progress(int progress)
{
	if (progress != BOOTSTAGE_ID_ENTER_CLI_LOOP)
		return;

	display_turn_off();
}

// TODO: move all functions which are just big switch statements which
// are required for all cases... pull into common stop in deckard_devices

static bool deckard_should_wait( boot_intention_t boot_intention )
{
	switch ( boot_intention )
	{
		case BOOT_INTENTION_START_USERSPACE:
		case BOOT_INTENTION_BOOT_MENU:
		case BOOT_INTENTION_BOOT_FROM_USB:
		case BOOT_INTENTION_FASTBOOT:
		case BOOT_INTENTION_CONSOLE_WAIT:
		case BOOT_INTENTION_BOOTLOADER:
			return false;

		case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:
		case BOOT_INTENTION_CHARGE_AND_WAIT:
		// it would be cool to omit default and let the compile flag if we missed a case, but it doesnt work
		default:
			return true;
	}
}



/*
	turn device on. lowest level battery allow us to get into SPL at all (prior to uboot).
	in uboot:
		- can still be in hairy edge.
		- need to have enough to boot DSP for higher power charging
		- need to have enough to boot displays
*/

// trap in uboot while we wait for power button to be pushed
// we should charge and do various things here like light up the LED's etc

static void deckard_wait_to_boot( boot_intention_t * p_boot_intention, deckard_devices_t * pdevices )
{
	printf("\n");
	deckard_log("********** deckard_wait_to_boot **********\n");
	deckard_log("boot_intention: %d\n", *p_boot_intention );

	boot_intention_t boot_intention = *p_boot_intention;

	// Check for the existance of devices used below
	if ( !pdevices->max17320 || !pdevices->pm8550b )
	{
		deckard_log( "ERROR: no pdevices->max17320/pdevices->pm8550b. Cannot wait to boot.\n" );
		*p_boot_intention = BOOT_INTENTION_START_USERSPACE;
		return;
	}

	if ( !pdevices->btn_power )
	{
		deckard_log( "ERROR: no pdevices->btn_power. Cannot wait to boot.\n" );
		*p_boot_intention = BOOT_INTENTION_START_USERSPACE;
		return;
	}

	int deckard_shutdown_loop_counter = 1;
	static const int k_nBatteryImageCount = 10 * 3; // 100ms * 10 * 5 = 3s
	int deckard_display_off_counter = k_nBatteryImageCount;
	bool adsp_loaded = 0;
	bool in_shutdown = 0;

	// This default implies a new discrete press will be required to retrigger,
	// which is intentional
	int power_button_down = ( button_get_state( pdevices->btn_power ) == BUTTON_ON );

	// WARNING: Do not set LED color in the following loop when the ADSP is loaded
	// The ADSP sets LED colors when active based on state only it is aware of
	// (such as USB cable insertion). In order to not stomp on each other,
	// we CANNOT set the LED color
	//
	// tldr; if ADSP is running (in either lite or non-lite mode) it is responsible
	// for LED handling.  uboot only does LED control, for the duration the ADSP
	// is not loaded
	//
	// TODO: also do not use the i2c bus that the ADSP is on.  It needs it for
	// power negotiation

	while ( true )
	{
		// Start the ADSP to allow charging
		// TODO: throttle this, if for some reason we cant start?
		if ( !adsp_loaded && deckard_battery_ok_for_dsp( pdevices->max17320 ) )
		{
			deckard_setledcolor( LED_COLOR_OFF, &g_deckard_devices );
			adsp_loaded = ( deckard_adsp_start() == 0 );
		}

		// Watch for power button click
		{
			int latest_power_button_down = ( button_get_state( pdevices->btn_power ) == BUTTON_ON );
			if ( latest_power_button_down && power_button_down != latest_power_button_down )
			{
				// Update our boot intention on a click
				boot_intention = calc_boot_intention( pdevices, 1 );
				deckard_log( "User power button pressed. boot_intention:%d\n", boot_intention );
				
				if ( boot_intention == BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY )
				{
					if ( !adsp_loaded )
					{
						deckard_setledcolor( LED_COLOR_RED, &g_deckard_devices );
					}

					// TODO: only show if battery is ok to show
					deckard_load_and_display_image_for_boot_intention( boot_intention );
					deckard_display_off_counter = k_nBatteryImageCount;
				}

				// Is it time to exit our loop?
				if ( !deckard_should_wait( boot_intention ) )
				{
					break;
				}
			}

			power_button_down = latest_power_button_down;
		}

#if 0
		//may want to check for keyboard press for some reason or another here
		if(deckard_check_for_key_press())
		{
			key_pressed = 1;
			break;
		}
#endif

		//check every 10 loops to avoid spamming the SPMI bus
		if ( !in_shutdown && deckard_shutdown_loop_counter-- == 0 )
		{
			int soc = deckard_battery_get_capacity(pdevices->max17320);
			int avg_vcell = deckard_battery_get_avg_vcell(pdevices->max17320);
			int avg_current = deckard_battery_get_avg_current(pdevices->max17320);
			deckard_log("\r" ANSI_CLEAR_LINE "Battery: %d%%, %dmV, %dmA", soc,avg_vcell,avg_current); fflush(stdout);

			deckard_shutdown_loop_counter = 10;
			if( deckard_shutdown_condition( pdevices->pm8550b ) )
			{
				deckard_log("Device not charging for %d seconds, Shutdown\n", DECKARD_SHUTDOWN_TIMEOUT_S);
				run_command("splctl set-uboot-poweroff-reason battery", 0); //inform the spl that we are shutting down due to low battery

				deckard_log("SYSRESET_POWER_OFF\n" );
				int ret = sysreset_walk(SYSRESET_POWER_OFF);
				if ( ret )
				{
					deckard_log("WARNING: SYSRESET_POWER_OFF returned error code %d\n", ret );

					// Note: SYSRESET_POWER_OFF can take time, so if the first call to SYSRESET_POWER_OFF succeeds
					// we want to give it time to take effect and not attempt a subsequent immediate call
					// But at the same time, we don't want to just return from this wait loop (which would
					// have the effect of beginning to start the kernel).
				}
				else
				{
					in_shutdown = true;
				}
			}
		}

		if ( deckard_display_off_counter >= 0 && ( deckard_display_off_counter-- == 0 ) )
		{
			display_turn_off();
			if ( !adsp_loaded )
			{
				deckard_setledcolor( LED_COLOR_OFF, &g_deckard_devices );
			}
		}

		// polling loop
		// TODO : maybe be interrupt driven somehow?
		mdelay(100);
	}

	if ( adsp_loaded ) deckard_adsp_stop();
	deckard_log( "deckard_wait_to_boot end\n" );
	*p_boot_intention = boot_intention;
}

int deckard_show_boot_menu(void) 
{
	display_turn_on();
	g_total_console_state |= VIDEO_CONSOLE_MASK;
	console_state_handler();
	mdelay(500);

	// This blocks until the user returns from the bootmenu
	// see common/menu.c , bootmenu_loop
	run_command("bootmenu",0);
	
	return 0;
}

// per arm64 linux boot documenation, default text_offset is 0x80000
#define ADDR_KERNEL "0x88000000"
// this gives us roughly 290 Megabytes for kernel, dtb, and initrd
#define MAX_ADDR_INITRD 0x99000000

// Deckard boot command.  Executes sequence that would normally be in the boot.scr
// Note: This runs both when manually invoked via the command-line, and also
// under the nominal startup condition via env_set("bootcmd", "deckard_boot");


static const char * get_booted_rootfs_slotname( void )
{
	switch( get_booted_slot() ){
		case 'B':
			return DECKARD_ROOTFS_SLOT_B;
		case 'U':
			return DECKARD_ROOTFS_SLOT_U;
		case 'A':
		default:
			return DECKARD_ROOTFS_SLOT_A;
	}
}

#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK

static const char * get_booted_rootfs_slotname_OLD( void )
{
	switch( get_booted_slot() )
	{
		case 'B':
			return DECKARD_ROOTFS_SLOT_OLD_B;
		case 'A':
		default:
			return DECKARD_ROOTFS_SLOT_OLD_A;
	}
}
#endif


int deckard_boot_cmd(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	printf("\n");
	deckard_log( "********** deckard_boot_cmd **********\n" );

	char * k_KERNEL_TARGET = "/boot/Image";  // should be const but uboot's do_load makes this inconvenient
	char * k_INITRD_TARGET = "/boot/initrd.uImage";  // should be const but uboot's do_load makes this inconvenient

	//TODO: this function assumes scsi has already been probed -- can we enforce that somehow?

	const char *label = get_booted_rootfs_slotname();
	char partition[DEVPARTMAXLEN] = { 0 };
	if ( deckard_get_dev_part_str_buf( label, &partition[0], sizeof(partition) ) != 0 )
	{
		partition[0] = '\0';
	}

#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
	if( !partition[0] )
	{
		deckard_log("Trying old rootfs partition names...\n");
		label = get_booted_rootfs_slotname_OLD();
		if ( deckard_get_dev_part_str_buf( label, &partition[0], sizeof(partition) ) != 0 )
		{
			partition[0] = '\0';
		}
	}
#endif

	const enum disk_interface_t disk_interface = booted_from_usb() ? DISK_INTERFACE_USB : DISK_INTERFACE_SCSI;

	if ( g_boot_intention == BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY ||
		g_boot_intention == BOOT_INTENTION_CHARGE_AND_WAIT )
	{
		// This may be minutes ... hours ... days...
		// We will stay in this function until the user clicks on the power
		// button with the intention to boot the device

		deckard_wait_to_boot( &g_boot_intention, &g_deckard_devices );

		// The boot intention has changed. Re setup the needed state
		setup_boot_intention_global_state( g_boot_intention );
	}

	// We're booting!
	deckard_setledcolor( LED_COLOR_WHITE, &g_deckard_devices );
	deckard_load_and_display_image_for_boot_intention( g_boot_intention );

	// FIXME: eventually invert this so we try EFI first, maybe find a less hacky way to do this?
	if (g_deckard_efi_mode) {
		deckard_log("Attempting to boot with EFI!\n");
		run_command("bootefi bootmgr", 0);
	}

	deckard_log("Attempting to load kernel from %s [ %s ]\n", label, partition);

	// Load kernel
	if ( do_load_helper(disk_interface, partition, ADDR_KERNEL, k_KERNEL_TARGET) ) {
		deckard_log("Failed to load kernel from %s [ %s ]\n", label, partition);
		goto fail;
	}

	// Get kernel size.  TODO for now we are assuming an uncompressed linux image
	ulong addr_kernel = hextoul(ADDR_KERNEL, NULL);
	ulong image_size = 0;
	int ret = booti_get_image_size(addr_kernel, &image_size);
	if (ret)
		deckard_log("Could not get linux image size ( %d )\n", ret);
    // get dtb address.  kernel will expand to the nearest 2MB boundary, so we give us a 10MB buffer
	ulong addr_dtb = addr_kernel + image_size + 0xA00000;
	// align to next 2MB address
	addr_dtb += 0x200000 - ( addr_dtb % 0x200000 );

	char DTB_ADDR[DEVPARTMAXLEN] = { 0 };
	sprintf(DTB_ADDR, "0x%lX", addr_dtb);
	deckard_log("DTB_ADDR %s\n", DTB_ADDR); // debug

	// load dtb
	if ( deckard_load_dtb(disk_interface, partition, DTB_ADDR) ) {
		deckard_log("Failed to load dtb from %s [ %s ]\n", label, partition);
		goto fail;
	}
	// get address for initrd
	image_size = hextoul(env_get("filesize"), NULL);
	ulong addr_initrd = addr_dtb + image_size + + 0xA00000;
	addr_initrd += 0x200000 - ( addr_initrd % 0x200000 );
	char INITRD_ADDR[DEVPARTMAXLEN] = { 0 };
	sprintf(INITRD_ADDR, "0x%lX", addr_initrd);
	deckard_log("INITRD_ADDR %s\n", INITRD_ADDR); // debug
	// load inird
	if ( do_load_helper(disk_interface, partition, INITRD_ADDR, k_INITRD_TARGET) ) {
		deckard_log("Failed to load initrd from %s [ %s ]\n", label, partition);
		goto fail;
	}

	// check for max file sizes
	image_size = hextoul(env_get("filesize"), NULL); // fixme hextoul(NULL) is bad
	if ( addr_initrd + image_size > MAX_ADDR_INITRD ) {
		deckard_log("DANGER!! exceeded maximum memory allocated for kernel, dtb, and initrd!!!");
		// FIX<E: goto fail?
	}

	// prep bootargs
	char * default_bootargs = env_get("default_bootargs");
	char * temp_bootargs = env_get("temp_bootargs");
	char bootargs[PATHMAXLEN] = { 0 }; // how long can this be?
	snprintf(bootargs, PATHMAXLEN, "%s %s root=PARTLABEL=%s init=/sbin/init rauc.slot=%c noresume rootwait earlycon",
									default_bootargs ? default_bootargs : "", temp_bootargs ? temp_bootargs : "", label, get_booted_slot() );
	env_set("bootargs", bootargs);

	// erase temp bootargs which is used for Testing
	env_set("temp_bootargs", "");

	env_set("kernel_addr_r", ADDR_KERNEL);
	env_set("ramdisk_addr_r", INITRD_ADDR);
	env_set("fdtcontroladdr", DTB_ADDR);

	if ( g_boot_intention == BOOT_INTENTION_BOOT_MENU )
	{
		deckard_log("setup bootmenu\n");
		deckard_setup_bootmenu();
		deckard_log("show bootmenu\n");
		deckard_show_boot_menu();
		// you cannot print here, or it will show in the boot menu!
		deckard_boot_menu_handle_actions();
		deckard_log("handled bootmenu actions\n");
	}

	//even though we are about to turn the display off, we fill the framebuffer with the boot logo
	//then the kernel will copy that image to it's framebuffer so we see the boot logo
	g_total_console_state &= ~VIDEO_CONSOLE_MASK;
	console_state_handler();

	// TODO: would be really nice to be able to load image without turning display on/off
	deckard_load_and_display_image_for_boot_intention(BOOT_INTENTION_START_USERSPACE); //switch to booting logo before we boot up

	// This is to workaround an incomplete hand-off implementation, where we (as of 12/11/25)
	// need to allow the kernel to turn the display on itself (vs a 'hot transfer')
	display_turn_off();

    // Attempt to launch the kernel.  If this succeeds, we now leave uboot and enter kernel code
	deckard_log("**********   ATTEMPTING TO BOOT KERNEL ********\n");
	run_command("booti ${kernel_addr_r} ${ramdisk_addr_r} ${fdtcontroladdr}", 0);
	deckard_log("**********   KERNEL BOOT FAILED. RESETTING. ********\n");

	// intentional fall through
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
	deckard_boot, CONFIG_SYS_MAXARGS,     1,      deckard_boot_cmd,
    "Boot Deckard system image.", deckard_boot_help_text
);

int deckard_logo_cmd(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	deckard_load_and_display_image_for_boot_intention(BOOT_INTENTION_START_USERSPACE);
	return 0;
}

int deckard_bootmenu_cmd(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	deckard_show_boot_menu();
	return 0;
}

U_BOOT_LONGHELP(deckard_logo,
    "Show Deckard logo\n"
);

U_BOOT_CMD(
	deckard_logo, CONFIG_SYS_MAXARGS,     1,      deckard_logo_cmd,
    "Deckard display logo", deckard_logo_help_text
);

U_BOOT_LONGHELP(deckard_bootmenu,
    "Show Deckard bootmenu\n"
);

U_BOOT_CMD(
	deckard_bootmenu, CONFIG_SYS_MAXARGS,     1,      deckard_bootmenu_cmd,
    "Deckard display logo", deckard_bootmenu_help_text
);
