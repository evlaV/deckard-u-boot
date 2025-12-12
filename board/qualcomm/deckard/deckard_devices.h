/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard Devices
 *
 */

#ifndef __DECKARD_DEVICES_H
#define __DECKARD_DEVICES_H

typedef enum
{
	BOOT_INTENTION_START_USERSPACE,
	BOOT_INTENTION_CANNOT_BOOT_BATTERY_NOT_READY,
	BOOT_INTENTION_CHARGE_AND_WAIT
} boot_intention_t;

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
} deckard_devices_t;

int deckard_setup_devices( deckard_devices_t * pdevices );
boot_intention_t calc_boot_intention( deckard_devices_t * pdevices );
void deckard_setledcolor( LedColor color, deckard_devices_t * pdevices );

#endif