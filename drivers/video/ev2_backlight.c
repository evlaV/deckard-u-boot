// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2016 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 */

#define LOG_CATEGORY UCLASS_PANEL_BACKLIGHT

#include <dm.h>
#include <backlight.h>
#include <log.h>
#include <malloc.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <power/regulator.h>

#define REGULATOR_N 4
struct ev2_backlight_priv {
	struct udevice *reg[REGULATOR_N];
//	struct udevice *ev2;
	u32 *levels;
	int num_levels;
	uint default_level;
	int cur_level;
	uint min_level;
	uint max_level;
	bool enabled;
};

static char *vreg_names[REGULATOR_N] = {
	"bias0-supply", "bias1-supply",
	"backlight0-supply", "backlight1-supply",
};

static int ev2_backlight_enable(struct udevice *dev)
{
	struct ev2_backlight_priv *priv = dev_get_priv(dev);
	int i, ret = 0;

	printf("%s!\n", __func__);

	for (i = 0; i < REGULATOR_N; i++) {
		ret = regulator_set_enable_if_allowed(priv->reg[i], true);
		if (ret) {
			log_debug("fail to enable power supply %s: ret=%d\n",
				  priv->reg[i]->name, ret);
			return ret;
		}
	}

	priv->enabled = true;
	return 0;
}

static int ev2_backlight_set_brightness(struct udevice *dev, int percent)
{
	struct ev2_backlight_priv *priv = dev_get_priv(dev);
	return 0;
}

static int ev2_backlight_probe(struct udevice *dev)
{
	struct ev2_backlight_priv *priv = dev_get_priv(dev);
	int ret, i;

//printf("=========%s called\n", __func__);

	for (i = 0; i < REGULATOR_N; i++) {
		ret = device_get_supply_regulator(dev, vreg_names[i], &priv->reg[i]);
		if (ret)
			log_err("Cannot get power supply %s: ret=%d\n",
				  vreg_names[i], ret);

		ret = regulator_set_enable_if_allowed(priv->reg[i], false);
		if (ret)
			log_err("fail to enable power supply %s: ret=%d\n",
				  priv->reg[i]->name, ret);
	}

	return ret;
}

static const struct backlight_ops ev2_backlight_ops = {
	.enable		= ev2_backlight_enable,
	.set_brightness	= ev2_backlight_set_brightness,
};

static const struct udevice_id ev2_backlight_ids[] = {
	{ .compatible = "ev2-backlight" },
	{ }
};

U_BOOT_DRIVER(ev2_backlight) = {
	.name	= "ev2_backlight",
	.id	= UCLASS_PANEL_BACKLIGHT,
	.of_match = ev2_backlight_ids,
	.ops	= &ev2_backlight_ops,
	.probe		= ev2_backlight_probe,
	.priv_auto	= sizeof(struct ev2_backlight_priv),
};
