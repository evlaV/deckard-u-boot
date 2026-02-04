#include "deckard_devices.h"

#include <button.h>
#include <env.h>
#include <led.h>
#include <power/pmic.h>
#include <linux/delay.h>
#include <linux/arm-smccc.h>
#include <usb.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <command.h>
#include <asm/io.h>
#include <video.h>
#include <stdio_dev.h>

#include "pm8550vs.h"
#include "smb1510.h"
#include "pmk8550.h"
#include "deckard_battery.h"
#include "deckard_log.h"
#include <fastboot.h>
#include <sysreset.h>
#include <fs_loader.h>
#include <fs.h>

#include "deckard_disk.h"

#define DECKARD_FASTBOOT_DUMMY_SERIAL_NUMB "123456789"

#define TCSR_BOOT_MISC_DETECT 0x01fd9000

#define QCOM_SCM_SMCCC_64 0x42000000
#define QCOM_SCM_SVC_BOOT 0x01
#define QCOM_SCM_BOOT_SDI_CONFIG 0x09
#define QCOM_SCM_BOOT_SET_DLMODE 0x10

#define QCOM_SCM_BOOT_DISABLE_SDI (QCOM_SCM_SMCCC_64 | QCOM_SCM_SVC_BOOT << 8 | \
					QCOM_SCM_BOOT_SDI_CONFIG)
#define QCOM_SCM_BOOT_ENABLE_DLMODE (QCOM_SCM_SMCCC_64 | QCOM_SCM_SVC_BOOT << 8 | \
					QCOM_SCM_BOOT_SET_DLMODE)

//these are set as a bit-mask typically when we detect a button is enabled
#define VOLUP_BUTTON_MASK 0b100
#define VOLDN_BUTTON_MASK 0b010
#define SELECT_BUTTON_MASK 0b001

typedef enum
{
	VOLUP_OFF__VOLDN_OFF__SELECT_OFF = 0,
	VOLUP_OFF__VOLDN_OFF__SELECT_ON  = 1,
	VOLUP_OFF__VOLDN_ON__SELECT_OFF  = 2,
	VOLUP_OFF__VOLDN_ON__SELECT_ON   = 3,
	VOLUP_ON__VOLDN_OFF__SELECT_OFF  = 4,
	VOLUP_ON__VOLDN_OFF__SELECT_ON   = 5,
	VOLUP_ON__VOLDN_ON__SELECT_OFF   = 6,
	VOLUP_ON__VOLDN_ON__SELECT_ON    = 7,
} combined_button_state_t;

// From console_vr
extern void consolevr_save_background_image( void );
extern void consolevr_set_background_image_enabled( bool enable );

bool g_display_enabled = false;

enum combined_serial_console_state_t g_total_console_state = 0;

#if IS_ENABLED(CONFIG_SYSRESET_CMD_RESET)
void sysreset_board_reset_prepare(int argc, char *const argv[], enum sysreset_t *reset_type);
#endif
#if IS_ENABLED(CONFIG_FASTBOOT) && !CONFIG_IS_ENABLED(ENV_IS_NOWHERE)
int fastboot_set_reboot_flag(enum fastboot_reboot_reason reason);
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
void deckard_disable_watchdog(void)
{
	// disable watchdog 1
	write32(HWIO_APSS_WDT_TMR1_WDOG_CTL_ADDR, 0);

	// disable watchdog 2
	write32(HWIO_APSS_WDT_TMR2_WDOG_CTL_ADDR, 0);
}

static combined_button_state_t deckard_get_button_state( deckard_devices_t * pdevices )
{
	int mask = 0;
	mask |= ( pdevices->btn_select   && ( button_get_state( pdevices->btn_select) == BUTTON_ON ) )   ? SELECT_BUTTON_MASK: 0;
	mask |= ( pdevices->btn_vol_down && ( button_get_state( pdevices->btn_vol_down) == BUTTON_ON ) ) ? VOLDN_BUTTON_MASK : 0;
	mask |= ( pdevices->btn_vol_up   && ( button_get_state( pdevices->btn_vol_up) == BUTTON_ON ) )   ? VOLUP_BUTTON_MASK : 0;
	return ( combined_button_state_t ) mask;
}

const char *led_color_to_string(LedColor v)
{
    switch (v)
    {
        case LED_COLOR_OFF:     return "Off";
        case LED_COLOR_WHITE:   return "White";
        case LED_COLOR_YELLOW:  return "Yellow";
        case LED_COLOR_RED:     return "Red";
        case LED_COLOR_MAGENTA: return "Magenta";
        default:                return "Unknown LED color";
    }
}

const char *boot_intention_to_string(boot_intention_t v)
{
    switch (v)
    {
        case BOOT_INTENTION_START_USERSPACE:              return "Start userspace";
        case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:return "Cannot boot: battery not ready";
        case BOOT_INTENTION_CHARGE_AND_WAIT:              return "Charge and wait";
        case BOOT_INTENTION_BOOT_MENU:                    return "Boot menu";
        case BOOT_INTENTION_BOOT_FROM_USB:                return "Boot from USB";
        case BOOT_INTENTION_FASTBOOT:                     return "Fastboot";
        case BOOT_INTENTION_CONSOLE_WAIT:                 return "Console wait";
        case BOOT_INTENTION_BOOTLOADER:                   return "Bootloader (hold)";
        case BOOT_INTENTION_EDL:                          return "EDL (emergency download)";
        default:                                          return "Unknown boot intention";
    }
}


bool deckard_boot_desired( struct pon_on_off_reason pon_poff_reason )
{
	if( pon_poff_reason.pon_reason & 0x80 )
	{
		deckard_log("pon_reason includes button press, continuing to boot\n");
		return true;
	}
	else if( pon_poff_reason.pon_reason & 0x01 )
	{
		deckard_log("pon_reason includes reset, continuing to boot\n");
		return true;
	}
	else if( pon_poff_reason.type == PON_RESET_TYPE_WARM )
	{
		deckard_log("pon_reason includes warm reset, continuing to boot\n");
		return true;
	}

	return false;
}

boot_intention_t calc_boot_intention( deckard_devices_t * pdevices )
{
	// if we're in factory mode, dont worry about any other cases
	if ( deckard_factory_mode_enabled() )
	{
		return BOOT_INTENTION_START_USERSPACE;
	}

	// Figure out if boot-up (to kernel, etc) is desired.
	int boot_up_desired = false;
	int bootmenu_desired = false;
	{
		//Check for button combo's -- only return directly for special debug situations
		//such that even in dire situations we can still get fastboot or usb console
		//otherwise, proceed further
		combined_button_state_t button_state = deckard_get_button_state( pdevices );
		switch(button_state)
		{
			case VOLUP_OFF__VOLDN_OFF__SELECT_OFF:
				break;
			case VOLUP_OFF__VOLDN_OFF__SELECT_ON:
				bootmenu_desired = true;
				break;
			case VOLUP_OFF__VOLDN_ON__SELECT_OFF:
				deckard_log("fastboot mode\n");
				return BOOT_INTENTION_FASTBOOT;
			case VOLUP_OFF__VOLDN_ON__SELECT_ON:
				deckard_log("console wait mode\n");
				return BOOT_INTENTION_CONSOLE_WAIT;
			case VOLUP_ON__VOLDN_OFF__SELECT_OFF:
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

		/*
		* check on/off reasons to see if we were booted by a button
		* push or warm reset (reboot command), in case of unknown reason
		* (insert of battery and then plug charger) boot also.
		*/
		struct pon_on_off_reason pon_poff_reason = { 0 };
		int ret = pmk8550_get_on_off_reason(&pon_poff_reason);
		if (ret < 0) {
			deckard_log("failed to get on off reason\n");
			return BOOT_INTENTION_START_USERSPACE;
		}

		deckard_log("pon_reset_type: %s [0x%x] trigger: %s  [0x%x] reason: 0x%x\n",
			pmk8550_get_pon_type_str(pon_poff_reason.type),
			pon_poff_reason.type,
			pmk8550_get_pon_trigger_str(pon_poff_reason.trigger),
			pon_poff_reason.trigger,
			pon_poff_reason.pon_reason);
		
		//TODO: integrate with above logic potentially?
		if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_BOOTLOADER )
		{
			deckard_log("user issued 'reboot bootloader' detected\n");
			return BOOT_INTENTION_BOOTLOADER;
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_RECOVERY )
		{
			deckard_log("user issued 'reboot recovery' detected\n");
			return BOOT_INTENTION_CONSOLE_WAIT;
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_FASTBOOT )
		{
			deckard_log("user issued 'reboot fastboot' detected\n");
			return BOOT_INTENTION_FASTBOOT;
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_EDL )
		{
			deckard_log("user issued 'reboot edl' detected\n");
			return BOOT_INTENTION_EDL;
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_USB)
		{
			deckard_log("user issued 'reboot usb' detected\n");
			return BOOT_INTENTION_BOOT_FROM_USB;
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_NONE )
		{
			//default case
		}

		boot_up_desired = deckard_boot_desired( pon_poff_reason );
	}

	if( boot_up_desired && !deckard_battery_ok_for_userspace( pdevices->max17320 ) )
	{
		deckard_log("boot intention: cannot boot, battery not ready\n");
		return BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY;
	}
	else
	{
		if ( bootmenu_desired )
		{
			deckard_log("Select button pressed. Boot intention: boot menu\n");
			return BOOT_INTENTION_BOOT_MENU;
		}
		else if ( boot_up_desired )
		{
			return BOOT_INTENTION_START_USERSPACE;
		}
		else if ( !pdevices->btn_power )
		{
			deckard_log("ERROR: power button not found! Forcing bootloader active.\n");
			return BOOT_INTENTION_BOOTLOADER;
		}
		else
		{
			return BOOT_INTENTION_CHARGE_AND_WAIT;
		}
	}
}


bool deckard_should_wait_for_button_to_boot( boot_intention_t boot_intention )
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
		case BOOT_INTENTION_EDL: // EDL requires a restart so hang in charge mode
		// it would be cool to omit default and let the compile flag if we missed a case, but it doesnt work
		default:
			return true;
	}
}


LedColor get_led_color_for_boot_intention( deckard_devices_t * pdevices, boot_intention_t boot_intention )
{
	if ( boot_intention == BOOT_INTENTION_CHARGE_AND_WAIT )
	{
		// TODO: make a determination based on if we're connected
		// if ( deckard_battery_is_charging_enabled( pdevices->pm8550b ) )
		return LED_COLOR_OFF;
	}

	switch ( boot_intention )
	{
		case BOOT_INTENTION_START_USERSPACE:
			return LED_COLOR_WHITE;
		case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:
			return LED_COLOR_RED;
		case BOOT_INTENTION_BOOT_MENU:
		case BOOT_INTENTION_BOOT_FROM_USB:
		case BOOT_INTENTION_FASTBOOT:
		case BOOT_INTENTION_CONSOLE_WAIT:
		case BOOT_INTENTION_BOOTLOADER:
		case BOOT_INTENTION_EDL:
			return LED_COLOR_MAGENTA;
		default:
			return LED_COLOR_OFF;
	}
}

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

// TODO: return richer error codes / bitmask for display, including fatal/non-fatal
int deckard_setup_devices( deckard_devices_t * pdevices )
{
	deckard_log("deckard_setup_devices\n");

	int nerror = 0;
	memset( pdevices, 0, sizeof( *pdevices ) );

#if CHECK_PMICS
	check_pmics_comms();
#endif

	// Find the PMK8650
	int ret = pmic_get("pmic@0", &pdevices->pmk8550);
	if (ret != 0) {
		deckard_log("ERROR: Failed to find pmk8550\n");
		nerror = 1;
	}

	// Find the PM8550VS_e
	ret = pmic_get("pmic@4", &pdevices->pm8550vse);
	if (ret != 0) {
		deckard_log("ERROR: Failed to find pm8550vse\n");
		nerror = 1;
	}

	// Find the PM8550B
	ret = pmic_get("pmic@7", &pdevices->pm8550b);
	if (ret != 0) {
		deckard_log("ERROR: Failed to find pm8550b\n");
		nerror = 1;
	}

	// Find the max17320
	ret = pmic_get("pmic@36", &pdevices->max17320);
	if (ret != 0) {
		deckard_log("ERROR: Failed to find max17320\n");
		nerror = 1;
	}

	// Find the max17320 - external battery
	ret = pmic_get("pmic@76", &pdevices->max17320_external);
	if (ret != 0) {
		deckard_log("ERROR: Failed to find max17320 External\n");
		nerror = 1;
	}

	// Power button
	ret = button_get_by_label( "Power Button", &pdevices->btn_power );
	if (ret != 0) {
		deckard_log("ERROR: Failed to find button: Power Button\n");
		nerror = 1;
	} 

	// Vol down button
	ret = button_get_by_label( "Volume Down", &pdevices->btn_vol_down );
	if (ret != 0) {
		deckard_log("ERROR: Failed to find button: Volume Down\n");
		nerror = 1;
	} 

	// Vol down button
	ret = button_get_by_label( "Volume Up", &pdevices->btn_vol_up );
	if (ret != 0) {
		deckard_log("ERROR: Failed to find button: Volume Up\n");
		nerror = 1;
	} 

	// Vol down button
	ret = button_get_by_label( "Select", &pdevices->btn_select );
	if (ret != 0) {
		deckard_log("ERROR: Failed to find button: Select\n");
		nerror = 1;
	} 

	led_get_by_label( "red", &pdevices->led_red );
	led_get_by_label( "green", &pdevices->led_green );
	led_get_by_label( "blue", &pdevices->led_blue );

	if ( pdevices->pmk8550 )
	{
		// TODO: does this have side-effects? Should we check the return values?
		smb1510_check_subtype(pdevices->pmk8550);
	}

	/* Fix GPIO Levels that XBL continues to not let us */
	// TODO: verify result?
	if ( pdevices->pm8550vse )
	{
		pm8550vs_set_gpio_vin_level(pdevices->pm8550vse, PM8550VS_GPIO_3, PM8550VS_GPIO_VIN_1);
		pm8550vs_set_gpio_vin_level(pdevices->pm8550vse, PM8550VS_GPIO_4, PM8550VS_GPIO_VIN_1);
	}

	/* Ensure Battery is OK to proceed */
	// TODO: verify result
	if ( pdevices->pmk8550 && pdevices->pm8550b && pdevices->max17320 )
	{
		deckard_battery_prep_for_boot(pdevices->pmk8550, pdevices->pm8550b, pdevices->max17320);
	}

	if ( pdevices->pmk8550 )
	{
		deckard_log("smb1510: enable fixed frequency at 500khz mode\n");
		// TODO: verify result
		smb1510_set_switching_frequency(pdevices->pmk8550, 0x0F);
	}

	return nerror;
}

void deckard_setledcolor( LedColor color, deckard_devices_t * pdevices )
{
	enum led_state_t vals[3] = { LEDST_OFF, LEDST_OFF, LEDST_OFF };
	if ( color == LED_COLOR_WHITE )
	{
		vals[0] = LEDST_ON; vals[1] = LEDST_ON; vals[2] = LEDST_ON;
	}
	else if ( color == LED_COLOR_YELLOW )
	{
		vals[0] = LEDST_ON; vals[1] = LEDST_ON; 
	}
	else if ( color == LED_COLOR_RED )
	{
		vals[0] = LEDST_ON;
	}
	else if ( color == LED_COLOR_MAGENTA )
	{
		vals[0] = LEDST_ON; vals[2] = LEDST_ON;
	}
	else // OFF
	{
	}

	if ( pdevices->led_red )
		led_set_state(pdevices->led_red, vals[0] );
	if ( pdevices->led_green )
		led_set_state(pdevices->led_green, vals[1] );
	if ( pdevices->led_blue )
		led_set_state(pdevices->led_blue, vals[2]);
}

bool deckard_factory_mode_enabled( void )
{
	return ( env_get_ulong( "factory-mode", 10, 0 ) != 0 ); // defaults to off
}

bool deckard_adb_mode_enabled( void )
{
	return ( env_get_ulong( "adb-mode", 10, 0 ) != 0 ); // defaults to off
}

int deckard_write_string_to_vidconsole(const char * str)
{
	//this stdio_get_by_name will probe the vidconsole if it is not already probed
	//so this can potentially cause problems with env_set("stdout","vidconsole")
	//presently no problems are observed, but keep at eye out for that
	struct stdio_dev *vc = stdio_get_by_name("vidconsole");
	if ( vc && vc->puts )
	{
		vc->puts( vc, str );
	}
	else 
		return 1;

	return 0;
}

static int console_state_handler(enum combined_serial_console_state_t total_console_state) 
{
	fflush(stdout); //needed to make things not explode
	fflush(stderr); //needed to make things not explode

	//note: we need to have some console, so no console on at all is not valid, so we handle that below
	switch(total_console_state)
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

void console_state_activate(void)
{
	console_state_handler(g_total_console_state);
}

void console_state_set_flags(uint8_t console_mask)
{
	g_total_console_state |= console_mask;
}

void console_state_clear_flags(uint8_t console_mask)
{
	g_total_console_state &= ~console_mask;
}

int enable_ramoops_serial_console(void)
{
	env_set("stdin","serial,usbacm");
	env_set("stderr","serial,ramoops,usbacm");
	env_set("stdout","serial,ramoops,usbacm");
	return 0;
}

int display_turn_on(void)
{
	struct udevice *dev;

	if( ! display_enabled() ) return 0;

	return uclass_first_device_err(UCLASS_VIDEO, &dev);
}

int display_turn_off()
{
	if( ! display_enabled() ) return 0;

	if (env_get_ulong("keep_display", 10, 0) == 1)
		return 0;

	return display_turn_off_force();
}

int display_turn_off_force()
{
	console_state_clear_flags(VIDEO_CONSOLE_MASK);
	console_state_activate();
	return run_command("mdss stop", 0);
}

bool display_enabled(void)
{
	return g_display_enabled;
}

void set_display_enabled(bool state)
{
	g_display_enabled = state;
}

int deckard_unbind_usb(void)
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

int deckard_reboot_mode_usb(void)
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

void deckard_sysreset_prepare_edl(void)
{
	struct arm_smccc_res res = {0};

	/*
	 * Disble debug mode,
	 * a1: num of args
	 * a2: 1 => Disable watchdog
	 * a3: 0 => Disable SDI
	 */
	arm_smccc_smc(QCOM_SCM_BOOT_DISABLE_SDI, 0x2, 0x1, 0x0, 0, 0, 0, 0, &res);
	mdelay(100);

	/*
	 * Setup the boot_misc to 0x1 for EDL, 0x10 is minidump,
	 * 0x20 is dump. We may need to disalble PMIC watchdog
	 * also to avoid trigger during EDL transfers, but
	 * our platform has it disable by default.
	 */
	writel(0x1, TCSR_BOOT_MISC_DETECT);
}

#if IS_ENABLED(CONFIG_SYSRESET_CMD_RESET)
void sysreset_board_reset_prepare(int argc, char *const argv[], enum sysreset_t *reset_type)
{
	if ((argc == 2 && !strncmp(argv[1], "edl", 3)) ||
		(pmk8550_get_reboot_command_reason(NULL) == REBOOT_COMMAND_REASON_EDL)) {
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_NONE);
		deckard_sysreset_prepare_edl();
		*reset_type = SYSRESET_WARM;
	} else if (argc == 2 && !strncmp(argv[1], "usb", 3)) {
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_USB);
		run_command("splctl force-usb", 0);
	} else if (argc == 2 && !strncmp(argv[1], "fastboot", 8)) {
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_FASTBOOT);
	} else if (argc == 2 && !strncmp(argv[1], "bootloader", 8)) {
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_BOOTLOADER);
	} else if (argc == 2 && !strncmp(argv[1], "recovery", 8)) {
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_RECOVERY);
	}
}
#endif

#if IS_ENABLED(CONFIG_FASTBOOT) && !CONFIG_IS_ENABLED(ENV_IS_NOWHERE)
int fastboot_set_reboot_flag(enum fastboot_reboot_reason reason)
{
	if ((reason != FASTBOOT_REBOOT_REASON_FASTBOOTD) &&
			(reason != FASTBOOT_REBOOT_REASON_OEM_EDL) &&
			(reason != FASTBOOT_REBOOT_REASON_RECOVERY) &&
			(reason != FASTBOOT_REBOOT_REASON_OEM_USB) &&
			(reason != FASTBOOT_REBOOT_REASON_BOOTLOADER))
		return -ENOTSUPP;

	if (reason == FASTBOOT_REBOOT_REASON_FASTBOOTD) {
		deckard_log("Setting reboot to fastboot flag ...\n");
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_FASTBOOT);
	} else if (reason == FASTBOOT_REBOOT_REASON_BOOTLOADER) {
		deckard_log("Rebooting to bootloader mode ...\n");
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_BOOTLOADER);
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_EDL) {
		deckard_log("Rebooting to EDL mode ...\n");
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_EDL);
	} else if (reason == FASTBOOT_REBOOT_REASON_RECOVERY) {
		deckard_log("Setting reboot to recovery mode ...\n");
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_RECOVERY);
	} else if (reason == FASTBOOT_REBOOT_REASON_OEM_USB) {
		deckard_log("Setting reboot to USB mode ...\n");
		pmk8550_set_reboot_command_reason(REBOOT_COMMAND_REASON_USB);
	}

	return 0;
}
#endif

void deckard_fastboot_enable(deckard_devices_t * pdevices)
{
	// TODO: make this consistent with the other user of kernel_addr_r, which has a leading 0x prefix
	env_set("kernel_addr_r","90000000");
	/*
		* serial# is a special var, like ethaddr, they are defined as
		* write-once as a protection, we want to maintain that protection
		* writing it only once here, if not before, and avoid error after.
		*/
	if (!env_get("serial#")) {
		deckard_log("setting dummy serial number: %s\n", DECKARD_FASTBOOT_DUMMY_SERIAL_NUMB);
		env_update("serial#", DECKARD_FASTBOOT_DUMMY_SERIAL_NUMB);
	}

	deckard_setledcolor( LED_COLOR_WHITE, pdevices );

	/* disable display (logo off) when idle in fastboot mode */
	display_turn_off();

	run_command("fastboot -l 0x880000000 -s 0x10000000 usb 0", 0);
}


void deckard_display_image( boot_intention_t boot_intention )
{
	if ( ! display_enabled() )
		return;

	const char * filename = NULL;
	const char * fallbackFilename = NULL;

	switch( boot_intention )
	{
		case BOOT_INTENTION_START_USERSPACE:
			filename = "logo.bmp.gz";
			break;
		case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:
			filename = "logo_battery_low.bmp.gz";
			break;
		case BOOT_INTENTION_BOOT_MENU:
			filename = "logo_console.bmp.gz";
			fallbackFilename = "logo_error.bmp.gz";
			break;
		case BOOT_INTENTION_BOOT_FROM_USB:
			filename = "logo_usb.bmp.gz";
			fallbackFilename = "logo.bmp.gz";
			break;
		default:
			return; // Don't attempt to show "no image"
	}

	deckard_log( "showing image for: %s\n", boot_intention_to_string( boot_intention ) );

	bool displayed_image = false;

	// Try the primary image name for all load sources (partitions)
	for ( load_source_t load_source = 0; load_source < LOAD_SOURCE_COUNT; ++load_source )
	{
		int ret = deckard_load_image( filename, CONFIG_SYS_LOAD_ADDR, load_source );
		if ( ret == 0 )
		{
			deckard_log("Found primary image %s, in load_source %d\n", filename, load_source );
			bmp_info(CONFIG_SYS_LOAD_ADDR);
			bmp_display(CONFIG_SYS_LOAD_ADDR, 0,0);
			displayed_image = true;
			break;
		}
	}

	// Try the fallback image for all load sources
	if ( !displayed_image )
	{
		for ( load_source_t load_source = 0; load_source < LOAD_SOURCE_COUNT; ++load_source )
		{
			int ret = deckard_load_image( fallbackFilename, CONFIG_SYS_LOAD_ADDR, load_source );
			if ( ret == 0 )
			{
				deckard_log("Found fallback image %s, in load_source %d\n", fallbackFilename, load_source );
				bmp_info(CONFIG_SYS_LOAD_ADDR);
				bmp_display(CONFIG_SYS_LOAD_ADDR, 0,0);
				displayed_image = true;
				break;
			}
		}
	}

	if ( !displayed_image )
	{
		deckard_log("ERROR: Unable to find image\n" );
		return;
	}

	if ( boot_intention == BOOT_INTENTION_BOOT_MENU )
	{
		consolevr_save_background_image();
		consolevr_set_background_image_enabled( true );

		#define ESC "\x1b"
		const char *text = "Boot Menu Starting...";
		int col = ( ( 80 - (int) strlen( text ) ) / 2 ) + 1;
		char buffer[128] = { 0 };
		snprintf( buffer, sizeof(buffer) - 1, ESC "7"		/* Save cursor position */
												ESC "[1;1H"   /* move cursor to 1,1 */
												ESC "[2K"     /* erase line */
												ESC "[1;%dH"  /* center the string */
												"%s"          /* print string */
												ESC "8",      /* restore cursor */
												col, text);

		deckard_write_string_to_vidconsole((const char *) buffer);
	}
}

