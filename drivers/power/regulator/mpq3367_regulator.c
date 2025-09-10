// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro Ltd.
 */

#include <asm/gpio.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <linux/delay.h>
#include <power/regulator.h>

#define MPQ3367_REG_CTL0		0
#define OTID				BIT(7)
#define PWM_DIMMING			(1 << 5)
#define ANALOG_DIMMING			(2 << 5)
#define FREQ_SPREAD_DISABLE		(3 << 1)
#define CTL0_DEFAULT	(OTID | PWM_DIMMING | FREQ_SPREAD_DISABLE)

#define MPQ3367_REG_VID		3

#define MPQ3367_I2C_CHIP	0x38

struct mpq3367_regulator_pdata {
	struct udevice *i2c;
	struct gpio_desc en;
};

static int mpq3367_regulator_set_enable(struct udevice *dev, bool enable)
{
	u8 regval = CTL0_DEFAULT;
	int ret;
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);

	if (enable) {
		ret = dm_i2c_write(pdata->i2c, MPQ3367_REG_CTL0, &regval, 1);
		if (ret) {
			dev_err(dev, "i2c write ctl0 failed: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

#define RETRY_CNT 40
static int mpq3367_regulator_probe(struct udevice *dev)
{
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);
	u8 regval = CTL0_DEFAULT;
	int ret, cnt;

	ret = gpio_request_by_name(dev, "enable-gpios", 0, &pdata->en,
				   GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "get enable-gpio failed.\n");
		return ret;
	}

	ret = i2c_get_chip(dev->parent, MPQ3367_I2C_CHIP, 1, &pdata->i2c);
	if (ret) {
		dev_err(dev, "i2c dev get failed.\n");
		return ret;
	}
	
	dm_gpio_set_value(&pdata->en, 1);

	return 0;
}

static const struct dm_regulator_ops mpq3367_regulator_ops = {
	.set_enable  = mpq3367_regulator_set_enable,
};

static const struct udevice_id mpq3367_regulator_ids[] = {
	{ .compatible = "monolithicpower,mpq3367" },
	{ },
};

U_BOOT_DRIVER(mpq3367_regulator) = {
	.name = "mpq3367_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &mpq3367_regulator_ops,
	.of_match = mpq3367_regulator_ids,
	.plat_auto	= sizeof(struct mpq3367_regulator_pdata),
	.probe = mpq3367_regulator_probe,
};
