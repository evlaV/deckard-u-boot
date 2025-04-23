// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro Ltd.
 */

#include <asm/gpio.h>
#include <dm.h>
#include <i2c.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <power/regulator.h>

#define NEX10001UB_REG_VPOS	0
#define NEX10001UB_REG_VNEG	1
#define NEX10001UB_REG_VID	2
#define NEX10001UB_VSEL_STEPSIZE 100000

#define NEX10001UB_I2C_CHIP	0x3e

struct nex100011ub_regulator_config {
	u32 vmin;
	u32 vmax;
};

struct nex100011ub_regulator_pdata {
	u8 vsel_offset;
	struct udevice *i2c;
	struct gpio_desc biasp_gpio;
	struct gpio_desc biasn_gpio;
	struct nex100011ub_regulator_config *config;
};

static struct nex100011ub_regulator_config nex100011ub_data = {
	.vmin = 4000000,
	.vmax = 6500000,
};

static int gpio_done = 0;

static int nex100011ub_regulator_set_value(struct udevice *dev, int uV)
{
	struct nex100011ub_regulator_pdata *pdata = dev_get_plat(dev);
	int ret;
	u8 regval;

	if (uV < pdata->config->vmin || uV > pdata->config->vmax)
		return -EINVAL;

	uV -= pdata->config->vmin;

	uV = DIV_ROUND_UP(uV, NEX10001UB_VSEL_STEPSIZE);

	if (uV > U8_MAX)
		return -EINVAL;

	regval = (uV - pdata->config->vmin) / NEX10001UB_VSEL_STEPSIZE;

	ret = dm_i2c_write(pdata->i2c, NEX10001UB_REG_VPOS, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c failed to set Vpos: %d\n", ret);
		return ret;
	}

	return dm_i2c_write(pdata->i2c, NEX10001UB_REG_VNEG, &regval, 1);
}

static int nex100011ub_regulator_get_value(struct udevice *dev)
{
	u8 regval;
	int ret;
	struct nex100011ub_regulator_pdata *pdata = dev_get_plat(dev);

	ret = dm_i2c_read(pdata->i2c, NEX10001UB_REG_VNEG, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read Vneg failed: %d\n", ret);
		return ret;
	}

	return (u32)regval * NEX10001UB_VSEL_STEPSIZE + pdata->config->vmin;
}

static int nex100011ub_regulator_probe(struct udevice *dev)
{
	struct nex100011ub_regulator_pdata *pdata = dev_get_plat(dev);
	u8 regval;
	int ret;

	pdata->config = (void *)dev_get_driver_data(dev);

	if (gpio_done)
		goto skip_gpio;

	gpio_done = 1;
	ret = gpio_request_by_name(dev, "biasp-gpios", 0, &pdata->biasp_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "biasp gpio get failed.\n");
		return ret;
	}

	ret = gpio_request_by_name(dev, "biasn-gpios", 0, &pdata->biasn_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "biasn gpio get failed.\n");
		return ret;
	}

	dm_gpio_set_value(&pdata->biasp_gpio, 1);
	dm_gpio_set_value(&pdata->biasn_gpio, 1);
	udelay(1000);

skip_gpio:
	ret = i2c_get_chip(dev->parent, NEX10001UB_I2C_CHIP, 1, &pdata->i2c);
	if (ret) {
		dev_err(dev, "i2c dev get failed.\n");
		return ret;
	}

	ret = dm_i2c_read(pdata->i2c, NEX10001UB_REG_VID, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read VID failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "i2c read vendor ID: 0x%x\n", regval);

	regval = 0x11;
	ret = dm_i2c_write(pdata->i2c, NEX10001UB_REG_VPOS, &regval, 1); //+5.7v
	if (ret) {
		dev_err(dev, "i2c failed to set Vpos: %d\n", ret);
		return ret;
	}

	return dm_i2c_write(pdata->i2c, NEX10001UB_REG_VNEG, &regval, 1); //-5.7v
	if (ret) {
		dev_err(dev, "i2c failed to set Vpos: %d\n", ret);
		return ret;
	}
	return 0;
}

static const struct dm_regulator_ops nex100011ub_regulator_ops = {
	.get_value  = nex100011ub_regulator_get_value,
	.set_value  = nex100011ub_regulator_set_value,
};

static const struct udevice_id nex100011ub_regulator_ids[] = {
	{ .compatible = "nexperia,nex100011ub", .data = (ulong)&nex100011ub_data },
	{ },
};

U_BOOT_DRIVER(nex100011ub_regulator) = {
	.name = "nex100011ub_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &nex100011ub_regulator_ops,
	.of_match = nex100011ub_regulator_ids,
	.plat_auto	= sizeof(struct nex100011ub_regulator_pdata),
	.probe = nex100011ub_regulator_probe,
};
