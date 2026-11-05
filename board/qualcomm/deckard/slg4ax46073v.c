// SPDX-License-Identifier: GPL-2.0+
/*
 * Silego SLG4AX46073V fan controller
 *
 * Only the reset and the fan supply enable from the kernel slg4ax46073v-fan driver. The kernel
 * driver takes over speed control, the tachometer and the watchdog.
 *
 * (C) Copyright 2026 Valve Corp
 */

#include <dm.h>
#include <i2c.h>
#include <asm/gpio.h>
#include <dm/device_compat.h>
#include <linux/delay.h>

#include "slg4ax46073v.h"

#define SLG4AX46073V_REG_VIRTUAL_INPUT		0x4C
#define SLG4AX46073V_VI_DISABLE_PWM_CLK0	0x08
#define SLG4AX46073V_REG_RESET			0xF5
#define SLG4AX46073V_RESET			0x01
#define SLG4AX46073V_REG_VERSION		0xFA

// The reset register was added in version 7
#define SLG4AX46073V_VERSION_7			0x07

// The kernel waits 100-150 ms with the fan supply off before resetting
#define SLG4AX46073V_SUPPLY_OFF_MS		100
#define SLG4AX46073V_VERSION_RETRIES		3
#define SLG4AX46073V_VERSION_RETRY_MS		5

/**
 * struct slg4ax46073v_priv - device data
 * @en_gpio: fan supply enable
 * @version: controller version, 0 if it couldn't be read
 * @enabled: fan supply is on
 */
struct slg4ax46073v_priv {
	struct gpio_desc en_gpio;
	int version;
	bool enabled;
};

static int slg4ax46073v_read_version(struct udevice *dev)
{
	int ret = -EIO;
	int i;

	for (i = 0; i < SLG4AX46073V_VERSION_RETRIES; i++) {
		ret = dm_i2c_reg_read(dev, SLG4AX46073V_REG_VERSION);
		if (ret >= 0)
			break;
		mdelay(SLG4AX46073V_VERSION_RETRY_MS);
	}

	return ret;
}

// Same sequence as slg4ax46073v_reset() in the kernel: disable the oscillator, watchdog and tach,
// wait with the fan supply off, then reset. The supply is left off.
static int slg4ax46073v_reset(struct udevice *dev)
{
	struct slg4ax46073v_priv *priv = dev_get_priv(dev);
	int ret;

	ret = dm_i2c_reg_write(dev, SLG4AX46073V_REG_VIRTUAL_INPUT, SLG4AX46073V_VI_DISABLE_PWM_CLK0);
	dm_gpio_set_value(&priv->en_gpio, 0);
	priv->enabled = false;
	if (ret)
		return ret;

	mdelay(SLG4AX46073V_SUPPLY_OFF_MS);

	return dm_i2c_reg_write(dev, SLG4AX46073V_REG_RESET, SLG4AX46073V_RESET);
}

int slg4ax46073v_set_enabled(struct udevice *dev, bool enable)
{
	struct slg4ax46073v_priv *priv = dev_get_priv(dev);
	int ret;

	if (enable == priv->enabled)
		return 0;

	// The kernel and the ADSP leave the PWM clock disabled when they turn the fan off, and the
	// supply coming back on in that state runs the fan at full speed. Reset first.
	if (enable && priv->version >= SLG4AX46073V_VERSION_7) {
		ret = slg4ax46073v_reset(dev);
		if (ret)
			dev_err(dev, "reset failed (%d), enabling anyway\n", ret);
	}

	ret = dm_gpio_set_value(&priv->en_gpio, enable);
	if (ret)
		return ret;
	priv->enabled = enable;

	return 0;
}

int slg4ax46073v_get(struct udevice **devp)
{
	return uclass_get_device_by_driver(UCLASS_MISC, DM_DRIVER_GET(slg4ax46073v), devp);
}

static int slg4ax46073v_probe(struct udevice *dev)
{
	struct slg4ax46073v_priv *priv = dev_get_priv(dev);
	int ret;

	// Requesting the GPIO as an output turns the fan supply off
	ret = gpio_request_by_name(dev, "en-gpios", 0, &priv->en_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "could not decode en-gpios (%d)\n", ret);
		return ret;
	}

	ret = slg4ax46073v_read_version(dev);
	if (ret < 0) {
		// The fan can still be switched on, just not reset
		dev_err(dev, "failed to read version (%d), not resetting\n", ret);
		priv->version = 0;
		return 0;
	}
	priv->version = ret;

	if (priv->version < SLG4AX46073V_VERSION_7) {
		printf("slg4ax46073v: version %x, no reset register\n", priv->version);
		return 0;
	}

	// The supply stays off until enabled
	ret = slg4ax46073v_reset(dev);
	if (ret) {
		dev_err(dev, "reset failed (%d)\n", ret);
		return 0;
	}

	printf("slg4ax46073v: version %x, reset\n", priv->version);

	return 0;
}

static const struct udevice_id slg4ax46073v_ids[] = {
	{ .compatible = "silego,slg4ax46073v" },
	{ }
};

U_BOOT_DRIVER(slg4ax46073v) = {
	.name		= "slg4ax46073v",
	.id		= UCLASS_MISC,
	.of_match	= slg4ax46073v_ids,
	.probe		= slg4ax46073v_probe,
	.priv_auto	= sizeof(struct slg4ax46073v_priv),
};
