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

// MPQ3367 register addresses
#define MPQ3367_FUNC1_SET_REG   0x00
#define MPQ3367_FUNC2_SET_REG   0x01
#define MPQ3367_FAULT_REG       0x02
#define MPQ3367_ID_REG          0x03

#define MPQ3367_DIMMING_SHIFT   5
#define MPQ3367_DIMMING_MASK    0x03
#define MPQ3367_PWM_DIMMING     0x01

#define MPQ3367_I2C_CHIP	0x38

struct mpq3367_regulator_pdata {
	struct udevice *i2c;
	struct gpio_desc en;
	bool enabled;
};

static int mpq3367_regulator_set_enable(struct udevice *dev, bool enable)
{
	u8 regval;
	int ret;
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);

	if (enable) {
		if (pdata->enabled)
			return 0;

		dm_gpio_set_value(&pdata->en, 1);

		udelay(20000);

		ret = dm_i2c_read(pdata->i2c, MPQ3367_FUNC1_SET_REG, &regval, 1);
		if (ret) {
			dev_err(dev, "i2c read ctl0 failed: %d\n", ret);
			return ret;
		}

		regval &= ~(MPQ3367_DIMMING_MASK << MPQ3367_DIMMING_SHIFT);
		regval |= MPQ3367_PWM_DIMMING << MPQ3367_DIMMING_SHIFT;

		ret = dm_i2c_write(pdata->i2c, MPQ3367_FUNC1_SET_REG, &regval, 1);
		if (ret) {
			dev_err(dev, "i2c write ctl0 failed: %d\n", ret);
			return ret;
		}

		pdata->enabled = 1;

		return 0;
	}

	if (!pdata->enabled)
		return 0;

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

	return 0;
}

static int mpq3367_regulator_probe(struct udevice *dev)
{
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);
	int ret;
	u8 regval;

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

	// Check i2c by reading id
	ret = dm_i2c_read(pdata->i2c, MPQ3367_ID_REG, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read id failed: %d\n", ret);
		dm_gpio_free(dev, &pdata->en);
		return ret;
	}

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

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
