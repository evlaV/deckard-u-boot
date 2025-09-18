// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro Ltd.
 */

#include <asm/gpio.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <power/regulator.h>

struct tps61377_regulator_pdata {
	struct gpio_desc en;
	struct gpio_desc good;
	bool enabled;
};

static int tps61377_regulator_set_enable(struct udevice *dev, bool enable)
{
	int cnt = 1000;
	int ret;
	struct tps61377_regulator_pdata *pdata = dev_get_plat(dev);

	if (enable) {
		if (pdata->enabled)
			return 0;

		dm_gpio_set_value(&pdata->en, 1);

		/* Wait until GPIO goes to 1 */
		do {
			ret = dm_gpio_get_value(&pdata->good);
			if (ret > 0)
				break;

			udelay(1000);
		} while (cnt--);

		if (!cnt)
			printf("%s: preboost-good failed.\n", __func__);
		
		pdata->enabled = 1;

		return 0;
	}

	if (!pdata->enabled)
		return 0;

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

	return 0;
}

static int tps61377_regulator_probe(struct udevice *dev)
{
	struct tps61377_regulator_pdata *pdata = dev_get_plat(dev);
	int ret;

	ret = gpio_request_by_name(dev, "en-gpio", 0, &pdata->en,
				   GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "get enable-gpio failed.\n");
		return ret;
	}

	ret = gpio_request_by_name(dev, "pgood-gpio", 0,
				   &pdata->good, GPIOD_IS_IN);
	if (ret) {
		dev_err(dev, "get good-gpios failed\n");
		return ret;
	}

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

	return 0;
}

static const struct dm_regulator_ops tps61377_regulator_ops = {
	.set_enable  = tps61377_regulator_set_enable,
};

static const struct udevice_id tps61377_regulator_ids[] = {
	{ .compatible = "ti,tps61377" },
	{ },
};

U_BOOT_DRIVER(tps61377_regulator) = {
	.name = "tps61377_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &tps61377_regulator_ops,
	.of_match = tps61377_regulator_ids,
	.plat_auto	= sizeof(struct tps61377_regulator_pdata),
	.probe = tps61377_regulator_probe,
};
