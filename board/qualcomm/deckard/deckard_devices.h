/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard Devices
 *
 */

#ifndef __DECKARD_DEVICES_H
#define __DECKARD_DEVICES_H

#include <stdbool.h>
#include <linux/types.h>
#include <stdint.h>

#include "deckard_led.h"

// TODO: You cannot charge with DSP in the current bootloader implementation
// We drop to a shell before charge and wait completes, so we dont have charging!
// Refactor misc_init_r and deckard boot so that charging is enabled during console_wait?
// TODO: We also (likely) want the same charge behavior in BOOT_INTENTION_FASTBOOT

/*
- bootloader mode:  reboot and stop in uboot.  disable autoboot. like console wait, but instead of waiting 20s to
		autoboot, wait indefinitely.  TODO: should we bring up the console?  Intention is to give console.
		TODO: Do we really need console wait and bootloader?

- fastboot: device will boot up in gadget mode within uboot (not spl), and it exposes a fastboot device to your PC.
   		run `fastboot devices` on the PC to see it. Part of google platform tools.  Lets users boot a random kernel
		or erase specific partitions or run fastboot oem commands (vendor defined commands)
-
- EDL mode: Emergency Download Mode.  Same as holding down all buttons for 10s.  Very few people should
		be developer only
- USB mode: boot from USB media. 
*/

typedef enum
{
	BOOT_INTENTION_START_USERSPACE,
	BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY,
	BOOT_INTENTION_CHARGE_AND_WAIT,
	BOOT_INTENTION_BOOT_MENU,
	BOOT_INTENTION_BOOT_FROM_USB,
	BOOT_INTENTION_FASTBOOT,
	BOOT_INTENTION_CONSOLE_WAIT,
	BOOT_INTENTION_BOOTLOADER,  // like console wait, but indefinitely hold off autoboot
	BOOT_INTENTION_EDL
} boot_intention_t;

const char *boot_intention_to_string( boot_intention_t v );
bool deckard_should_wait_for_button_to_boot( boot_intention_t boot_intention );


typedef struct
{
	struct udevice *pmk8550;
	struct udevice *pm8550vse;
	struct udevice *pm8550b;
	struct udevice *max17320; // fuel gauge over i2c. cant use while adsp is loaded
	struct udevice *max17320_external; // removable (boulder) fuel gauge over i2c. cant use while adsp is loaded?
	struct udevice *led_red;
	struct udevice *led_green;
	struct udevice *led_blue;
	struct udevice *btn_power;
	struct udevice *btn_vol_down;
	struct udevice *btn_vol_up;
	struct udevice *btn_select;
} deckard_devices_t;

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

void console_state_set_flags(uint8_t console_mask);
void console_state_clear_flags(uint8_t console_mask);
void console_state_activate(void);

int deckard_setup_devices( deckard_devices_t * pdevices );

// Warning: do not call this while adsp is running
LedColor get_led_color_for_boot_intention( deckard_devices_t * pdevices, boot_intention_t boot_intention );
// Warning: do not call this while adsp is running
boot_intention_t calc_boot_intention( deckard_devices_t * pdevices );

void deckard_setledcolor( LedColor color, deckard_devices_t * pdevices );
bool deckard_factory_mode_enabled( void );
bool deckard_adb_mode_enabled( void );
bool deckard_pon_trigger_smpl( void );
bool deckard_steam_reset_enabled( void );

int deckard_unbind_usb(void);
int deckard_reboot_mode_usb(void);
void deckard_sysreset_prepare_edl(void);

void deckard_fastboot_enable(deckard_devices_t * pdevices);

bool display_enabled(void);
void set_display_enabled(bool state);
bool force_disable_display(void);
void set_force_disable_display(bool state);
int display_turn_on(void);
int display_turn_off(void);
int display_turn_off_force(void);
void deckard_display_image( boot_intention_t boot_intention );

void deckard_disable_watchdog(void);

#endif