/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard Devices
 *
 */

#ifndef __DECKARD_DEVICES_H
#define __DECKARD_DEVICES_H

#include <stdbool.h>

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
} boot_intention_t;

// TODO: Major bootloader bug: You cannot charge with DSP in the current bootloader implementation
// We drop to a shell before charge and wait completes, so we dont have charging!
// Refactor misc_init_r and deckard boot so that charging is enabled during console_wait?

/*
bootloader mode:  reboot and stop in uboot.  disable autoboot. like console wait, but instead of waiting 20s to
		autoboot, wait indefinitely.  TODO: should we bring up the console?  Intention is to give console.
		TODO: Do we really need console wait and bootloader?

question: does fastboot need to continue to have 2 stage processing?
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
    LED_COLOR_OFF,
    LED_COLOR_WHITE,
    LED_COLOR_YELLOW,
    LED_COLOR_RED,
} LedColor;

typedef struct
{
	struct udevice *pmk8550;
	struct udevice *pm8550vse;
	struct udevice *pm8550b;
	struct udevice *max17320;
	struct udevice *led_red;
	struct udevice *led_green;
	struct udevice *led_blue;
	struct udevice *btn_power;
	struct udevice *btn_vol_down;
	struct udevice *btn_vol_up;
	struct udevice *btn_select;
} deckard_devices_t;

int deckard_setup_devices( deckard_devices_t * pdevices );
boot_intention_t calc_boot_intention( deckard_devices_t * pdevices, int assume_boot_up_desired );
void deckard_setledcolor( LedColor color, deckard_devices_t * pdevices );
bool deckard_factory_mode_enabled( void );

#endif