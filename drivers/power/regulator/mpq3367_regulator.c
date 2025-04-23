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
	struct gpio_desc preboost;
	struct gpio_desc en;
};

static int preboost_done = 0;

static int mpq3367_regulator_set_enable(struct udevice *dev, bool enable)
{
	u8 regval = CTL0_DEFAULT;
	int ret;
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);


	udelay(1);

	ret = dm_i2c_write(pdata->i2c, MPQ3367_REG_CTL0, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c write ctl0 failed: %d\n", ret);
		return ret;
	}

	return 0;
}

#define RETRY_CNT 20
static int mpq3367_regulator_probe(struct udevice *dev)
{
	struct mpq3367_regulator_pdata *pdata = dev_get_plat(dev);
	u8 regval = CTL0_DEFAULT;
	int ret, cnt;

	if (preboost_done)
		goto skip_preboost;

	preboost_done = 1;
	ret = gpio_request_by_name(dev, "preboost-gpios", 0, &pdata->preboost,
				   GPIOD_IS_OUT);
	if (ret)
		return ret;

	for (cnt = RETRY_CNT; cnt; cnt--) {
		dm_gpio_set_value(&pdata->preboost, 0);
		udelay(300);
		dm_gpio_set_value(&pdata->preboost, 1);
		udelay(1300);
	}

	udelay(250000);

skip_preboost:
	ret = gpio_request_by_name(dev, "enable-gpios", 0, &pdata->en,
				   GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "get enable-gpio failed.\n");
		return ret;
	}
	dm_gpio_set_value(&pdata->en, 1);
	udelay(10);

	ret = i2c_get_chip(dev->parent, MPQ3367_I2C_CHIP, 1, &pdata->i2c);
	if (ret) {
		dev_err(dev, "i2c dev get failed.\n");
		return ret;
	}

	for (cnt = RETRY_CNT; cnt; cnt--) {
		ret = dm_i2c_write(pdata->i2c, MPQ3367_REG_CTL0, &regval, 1);
		if (ret >= 0)
			break;
		udelay(80000);
	}
	if (!cnt)
		dev_err(dev, "init retry %d ret %d\n", RETRY_CNT - cnt, ret);

	ret = dm_i2c_read(pdata->i2c, MPQ3367_REG_VID, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read VID failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "i2c read vendor ID: 0x%x\n", regval);

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
