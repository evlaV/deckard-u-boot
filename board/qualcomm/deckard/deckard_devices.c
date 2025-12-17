#include "deckard_devices.h"

#include <button.h>
#include <env.h>
#include <led.h>
#include <power/pmic.h>

#include "pm8550vs.h"
#include "smb1510.h"
#include "pmk8550.h"
#include "deckard_battery.h"
#include "deckard_log.h"


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

static combined_button_state_t deckard_get_button_state( deckard_devices_t * pdevices )
{
	int mask = 0;
	mask |= ( pdevices->btn_select   && ( button_get_state( pdevices->btn_select) == BUTTON_ON ) )   ? SELECT_BUTTON_MASK: 0;
	mask |= ( pdevices->btn_vol_down && ( button_get_state( pdevices->btn_vol_down) == BUTTON_ON ) ) ? VOLDN_BUTTON_MASK : 0;
	mask |= ( pdevices->btn_vol_up   && ( button_get_state( pdevices->btn_vol_up) == BUTTON_ON ) )   ? VOLUP_BUTTON_MASK : 0;
	return ( combined_button_state_t ) mask;
}


boot_intention_t calc_boot_intention( deckard_devices_t * pdevices, int assume_boot_up_desired )
{
	// These boot-up modes (those which are dev centric) blindly obey "reboot_mode"
	// Note that in some cases like 'b', they are set from the OS (user-space)
	// so do not expect to see the corresponding setter in uboot!

	char * reboot_mode = env_get("reboot_mode");
	if ( reboot_mode )
	{
		switch ( *reboot_mode )
		{
			case 'b':
				return BOOT_INTENTION_BOOTLOADER;
			case 'u':
				return BOOT_INTENTION_BOOT_FROM_USB;
			case 'f':
				return BOOT_INTENTION_FASTBOOT;
			case 'e':
				return BOOT_INTENTION_EDL;
			default:
				break;
		}
	}

	// Next, if we're in factory mode, dont worry about any other cases
	if ( deckard_factory_mode_enabled() )
	{
		return BOOT_INTENTION_START_USERSPACE;
	}

	// Figure out if boot-up (to kernel, etc) is desired.
	int boot_up_desired = false;
	if ( assume_boot_up_desired )
	{
		boot_up_desired = true;
	}
	else
	{
		boot_up_desired = false;
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
		
		if(pon_poff_reason.pon_reason & 0x80)
		{
			deckard_log("pon_reason includes button press, continuing to boot\n");
			boot_up_desired = true;
		}
		else if(pon_poff_reason.pon_reason & 0x01)
		{
			deckard_log("pon_reason includes reset, continuing to boot\n");
			boot_up_desired = true;
		}
		else if(pon_poff_reason.type == PON_RESET_TYPE_WARM )
		{
			deckard_log("pon_reason includes warm reset, continuing to boot\n");
			boot_up_desired = true;
		}
		
		//TODO: integrate with above logic potentially?
		if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_BOOTLOADER )
		{
			deckard_log("user issued 'reboot bootloader' detected\n");
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_RECOVERY )
		{
			deckard_log("user issued 'reboot recovery' detected\n");
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_EDL )
		{
			deckard_log("user issued 'reboot edl' detected\n");
		}
		else if( pon_poff_reason.reboot_command_reason == REBOOT_COMMAND_REASON_NONE )
		{
			//default case
		}
	}

	if( boot_up_desired && !deckard_battery_ok_for_userspace( pdevices->max17320 ) )
	{
		deckard_log("boot intention: cannot boot, battery not ready\n");
		return BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY;
	}
	else
	{
		combined_button_state_t button_state = deckard_get_button_state( pdevices );
		if ( button_state == VOLUP_OFF__VOLDN_OFF__SELECT_ON )
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


LedColor get_led_color_for_boot_intention( boot_intention_t boot_intention )
{
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
		case BOOT_INTENTION_CHARGE_AND_WAIT:
		default:
			return LED_COLOR_OFF;
	}
}

const char * get_image_filename_for_boot_intention( boot_intention_t boot_intention )
{
	switch( boot_intention )
	{
		case BOOT_INTENTION_START_USERSPACE:
			return "logo.bmp.gz";
		case BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY:
			return "logo_battery_low.bmp.gz";
		case BOOT_INTENTION_BOOT_MENU:
		case BOOT_INTENTION_BOOT_FROM_USB:
			return "logo_error.bmp.gz"; // TODO: show something better
		case BOOT_INTENTION_FASTBOOT:
		case BOOT_INTENTION_CONSOLE_WAIT:
		case BOOT_INTENTION_BOOTLOADER:
		case BOOT_INTENTION_EDL:
		case BOOT_INTENTION_CHARGE_AND_WAIT:
		default:
			return NULL;
	}
}

////////////////////////////////////////////////////////



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