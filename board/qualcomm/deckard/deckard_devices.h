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
	struct udevice *btn_vol_down;
	struct udevice *btn_vol_up;
	struct udevice *btn_select;
} deckard_devices_t;

int deckard_setup_devices( deckard_devices_t * pdevices );
boot_intention_t calc_boot_intention( deckard_devices_t * pdevices );
void deckard_setledcolor( LedColor color, deckard_devices_t * pdevices );


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

combined_button_state_t deckard_get_button_state( deckard_devices_t * pdevices );

#endif