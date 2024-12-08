/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PM8550VS Interface
 *
 */

#include "pm8550vs.h"

#define PM8550VS_GPIO_BASE        (0x8800)
#define PM8550VS_GPIO_BASE_OFFSET (0x0100)
#define PM8550VS_GPIO_DIG_VIN_CTL (PM8550VS_GPIO_BASE + 0x41)

int pm8550vs_set_gpio_vin_level(struct udevice *pmic, pm8550vs_gpio_t gpio, pm8550vs_gpio_vin_t vin)
{
	int      ret;
	uint16_t addr = PM8550VS_GPIO_DIG_VIN_CTL + gpio * PM8550VS_GPIO_BASE_OFFSET;

	ret = pmic_reg_write(pmic, addr, vin);
	if (ret != 0) {
		printf("Failed to set GPIO VIN.\n");
		return ret;
	}

	return 0;
}
