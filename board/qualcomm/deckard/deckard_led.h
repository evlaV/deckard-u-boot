/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard LED
 *
 */

#ifndef __DECKARD_LED_H
#define __DECKARD_LED_H

#include <stdbool.h>
#include <linux/types.h>
#include <stdint.h>

typedef enum
{
    LED_COLOR_OFF,
    LED_COLOR_WHITE,
    LED_COLOR_YELLOW,
    LED_COLOR_RED,
    LED_COLOR_MAGENTA,
} LedColor;

const char *led_color_to_string(LedColor v);

#endif