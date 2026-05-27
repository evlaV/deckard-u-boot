// SPDX-License-Identifier: GPL-2.0+
/*
 */

#define LOG_CATEGORY UCLASS_PANEL_BACKLIGHT

#include <backlight.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <log.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <asm/gpio.h>
#include <power/regulator.h>

#define DEFAULT_BRIGHTNESS 40 // percent
#define MAX_BRIGHTNESS 4080
#define REG_MAX 0xa9

/* reg */
#define BL_BRT_L 0x10
#define BL_BRT_H 0x11

struct sy7758_priv {
	bool led_on;
};

static int sy7758_write(int ret, struct udevice *dev, u32 addr, u32 val)
{
	if (ret)
		return ret;

	return dm_i2c_reg_write(dev, addr, val);
}

static int sy7758_init(struct udevice *dev)
{
	struct sy7758_priv *priv = dev_get_priv(dev);
	int ret = 0;

	ret = sy7758_write(ret, dev, 0x01, 0x85);
	ret = sy7758_write(ret, dev, 0x10, 0x00);
	ret = sy7758_write(ret, dev, 0x11, 0x00);
	ret = sy7758_write(ret, dev, 0xa5, 0x64);
	ret = sy7758_write(ret, dev, 0xa0, 0x55);
	ret = sy7758_write(ret, dev, 0xa1, 0x9a);
	ret = sy7758_write(ret, dev, 0xa9, 0x80);
	ret = sy7758_write(ret, dev, 0xa2, 0x28);
	/* Wait init done */
	if (!ret)
		udelay(10 * 1000);

	priv->led_on = ret ? false : true;

	if (ret)
		dev_err(dev, "init failed: %d\n", ret);

	return ret;
}

static int sy7758_backlight_set_brightness(struct udevice *dev, int percent)
{
	struct sy7758_priv *priv = dev_get_priv(dev);
	u16 brightness = DIV_ROUND_UP(MAX_BRIGHTNESS * percent, 100);
	int ret = 0;

	if (!priv->led_on && brightness > 0) {
		priv->led_on = true;
	} else if (brightness == 0) {
		priv->led_on = false;
	}

	/* Set brightness */
	ret = sy7758_write(ret, dev, BL_BRT_L, brightness & 0xf0);
	ret = sy7758_write(ret, dev, BL_BRT_H, (brightness >> 8) & 0xf);

	return ret;
}

static int sy7758_backlight_enable(struct udevice *dev)
{
	int ret;

	ret = sy7758_init(dev);
	if (ret)
		return ret;

	return sy7758_backlight_set_brightness(dev, DEFAULT_BRIGHTNESS);
}

static int sy7758_backlight_probe(struct udevice *dev)
{
	if (device_get_uclass_id(dev->parent) != UCLASS_I2C)
		return -EPROTONOSUPPORT;

	return 0;
}

static const struct backlight_ops sy7758_backlight_ops = {
	.enable = sy7758_backlight_enable,
	.set_brightness = sy7758_backlight_set_brightness,
};

static const struct udevice_id sy7758_backlight_ids[] = {
	{ .compatible = "silergy,sy7758" },
	{ }
};

U_BOOT_DRIVER(sy7758_backlight) = {
	.name		= "sy7758_backlight",
	.id		= UCLASS_PANEL_BACKLIGHT,
	.of_match	= sy7758_backlight_ids,
	.probe		= sy7758_backlight_probe,
	.ops		= &sy7758_backlight_ops,
	.priv_auto	= sizeof(struct sy7758_priv),
};
