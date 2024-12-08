/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PM8550VS Interface
 *
 */

#ifndef __PM8550VS_H
#define __PM8550VS_H

#include <stdint.h>
#include <stdbool.h>

#include <power/pmic.h>

typedef enum
{
	PM8550VS_GPIO_1,
	PM8550VS_GPIO_2,
	PM8550VS_GPIO_3,
	PM8550VS_GPIO_4,
	PM8550VS_GPIO_5,
	PM8550VS_GPIO_6
} pm8550vs_gpio_t;

typedef enum
{
	PM8550VS_GPIO_VIN_0 = 0x00,
	PM8550VS_GPIO_VIN_1 = 0x01,
	PM8550VS_GPIO_VIN_2 = 0x02
} pm8550vs_gpio_vin_t;

int pm8550vs_set_gpio_vin_level(struct udevice *pmic, pm8550vs_gpio_t gpio, pm8550vs_gpio_vin_t vin);

#endif /* __PM8550VS_H */