// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Linaro Ltd.
 * Casey Connolly <casey.connolly@linaro.org>
 */

#include <asm-generic/gpio.h>
#include <dm.h>
#include <i2c.h>
#include <dm/device_compat.h>
#include <power/regulator.h>
#include <linux/delay.h>

#define N_RESET_GPIO 2

struct sgm3804_regulator_plat {
	struct gpio_desc reset_gpio[N_RESET_GPIO];
};

static int sgm3804_regulator_set_enable(struct udevice *dev, bool enable)
{
	struct sgm3804_regulator_plat *plat = dev_get_plat(dev);
	int ret;

	dm_gpio_set_value(&plat->reset_gpio[0], enable);
	dm_gpio_set_value(&plat->reset_gpio[1], enable);

	udelay(100 * 1000);

	if (!enable)
		return 0;

	ret = dm_i2c_reg_write(dev, 0x00, 0x12);
	if (ret) {
		dev_err(dev, "sgm3804_enable: regmap_write 0x00 failed: %d\n", ret);
		return ret;
	}

	ret = dm_i2c_reg_write(dev, 0x01, 0x12);
	if (ret) {
		dev_err(dev, "sgm3804_enable: regmap_write 0x01 failed: %d\n", ret);
		return ret;
	}

	ret = dm_i2c_reg_write(dev, 0x03, 0x03);
	if (ret) {
		dev_err(dev, "sgm3804_enable: regmap_write 0x03 failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sgm3804_regulator_get_value(struct udevice *dev)
{
	return 5000000;
}

static int sgm3804_regulator_probe(struct udevice *dev)
{
	struct sgm3804_regulator_plat *plat = dev_get_plat(dev);
	int ret;

	for (int i = 0; i < N_RESET_GPIO; i++) {
		ret = gpio_request_by_name(dev, "reset-gpios", i,
					&plat->reset_gpio[i], GPIOD_IS_OUT_ACTIVE);
		if (ret) {
			dev_err(dev, "cannot decode reset-gpios (%d)\n",
				ret);
			return ret;
		}
	}

	return 0;
}

static const struct dm_regulator_ops sgm3804_regulator_ops = {
	.get_value  = sgm3804_regulator_get_value,
	.set_enable  = sgm3804_regulator_set_enable,
};

static const struct udevice_id sgm3804_regulator_ids[] = {
	{ .compatible = "sgmicro,sgm3804", },
	{ },
};

U_BOOT_DRIVER(sgm3804_regulator) = {
	.name = "sgm3804_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &sgm3804_regulator_ops,
	.of_match = sgm3804_regulator_ids,
	.probe = sgm3804_regulator_probe,
	.plat_auto = sizeof(struct sgm3804_regulator_plat),
};
