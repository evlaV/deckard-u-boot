// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025, Kancy Joe <kancy2333@outlook.com>
 */

#define LOG_DEBUG

#include <backlight.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <panel.h>
#include <log.h>
#include <mipi_dsi.h>
#include <mipi_display.h>
#include <linux/delay.h>
#include <power/regulator.h>
#include <asm/gpio.h>

#define WT0600_REGULATOR_COUNT 4

static const char *wt0600_supplies[WT0600_REGULATOR_COUNT] = {
	"vddio-supply",
	"vci-supply",
	"vdd-supply",
	"avdd-supply",
};

struct wt0600_priv {
	struct udevice *dev;

	struct udevice *regulators[WT0600_REGULATOR_COUNT];
	struct udevice *backlight;

	struct gpio_desc reset_gpio;

	/* Dummy second panel device for dual-DSI */
	bool secondary;
};

static const struct display_timing default_timing = {
	.pixelclock.typ		= 2 * (720 + 100 + 8 + 40) * (2560 + 15 + 8 + 2) * 60,
	.hactive.typ		= 2 * 720,
	.hfront_porch.typ	= 2 * 100,
	.hback_porch.typ	= 2 * 40,
	.hsync_len.typ		= 2 * 8,

	.vactive.typ		= 2560,
	.vfront_porch.typ	= 15,
	.vback_porch.typ	= 8,
	.vsync_len.typ		= 2,
};

// Get panel_plat for the appropriate part of the panel (DSI0 or DSI1)
static struct mipi_dsi_panel_plat *wt0600_get_dsi1_plat(struct udevice *dev)
{
	struct udevice *dummy;
	int ret;

	ret = device_get_child(dev, 0, &dummy);
	if (ret) {
		dev_err(dev, "Couldn't find dummy device? %d\n", ret);
		return NULL;
	}

	return dev_get_plat(dummy);
}

// Only called for second interface (since in sync mode cmds sent to DSI0 are ignored and DSI1 triggers both)
static int wt0600_on(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	// struct mipi_dsi_panel_plat *plat_dsi1 = wt0600_get_dsi1_plat(dev);

	// log_debug("%s:%d: %s\n", __FILE__, __LINE__, __func__);

	// if (!plat_dsi1)
	// 	return -ENOENT;

	// Linux driver does set_display_on first?? wtf??
	mipi_dsi_dcs_set_display_on(plat->device);
	// mipi_dsi_dcs_set_display_on(plat_dsi1->device);
	mdelay(150);

	mipi_dsi_dcs_exit_sleep_mode(plat->device);
	// mipi_dsi_dcs_exit_sleep_mode(plat_dsi1->device);
	mdelay(50);

	return 0;
}

static void wt0600_reset(struct wt0600_priv *priv)
{
	dm_gpio_set_value(&priv->reset_gpio, 1);
	mdelay(11);
	dm_gpio_set_value(&priv->reset_gpio, 0);
	mdelay(11);
	dm_gpio_set_value(&priv->reset_gpio, 0);
	mdelay(55);
}

static int wt0600_set_regulators(struct wt0600_priv *priv, bool enable)
{
	int ret;

	for (int i = 0; i < WT0600_REGULATOR_COUNT; i++) {
		ret = regulator_set_enable_if_allowed(priv->regulators[i], true);
		if (ret) {
			dev_err(priv->dev, "Failed to enable regulator %s: %d\n", wt0600_supplies[i], ret);
			return ret;
		}
	}

	return 0;
}

static int wt0600_enable_backlight(struct udevice *dev)
{
	struct wt0600_priv *priv = dev_get_priv(dev);

	if (!priv->secondary)
		return 0;

	wt0600_on(dev);

	return backlight_enable(priv->backlight);
}

static int wt0600_set_backlight(struct udevice *dev, int percent)
{
	struct wt0600_priv *priv = dev_get_priv(dev);

	if (!priv->secondary)
		return 0;
	
	return backlight_set_brightness(priv->backlight, percent);
}

static int wt0600_get_display_timing(struct udevice *dev,
					       struct display_timing *timing)
{
	memcpy(timing, &default_timing, sizeof(*timing));
	return 0;
}

// Bind a dummy second panel device, it will be attached to the second DSI controller (via OF graph)
// We make it a child so we can easily find it to send DSI commands
static int wt0600_bind(struct udevice *dev)
{
	int ret = 0;
	const struct driver *drv;

	// /* Don't get in a recursive loop! */
	// if (dev_get_driver_data(dev))
	// 	return 0;

	// drv = lists_driver_lookup_name("wt0600");
	// if (!drv) {
	// 	dev_err(dev, "Couldn't find panel driver???\n");
	// 	return -ENOENT;
	// }

	// ret = device_bind_with_driver_data(dev, drv, "dummy_panel", 1, dev_ofnode(dev), NULL);
	// if (ret)
	// 	dev_err(dev, "Failed to bind dummy panel!\n");

	return ret;
}

static int wt0600_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct wt0600_priv *priv = dev_get_priv(dev);
	int ret;

	priv->dev = dev;
	priv->secondary = dev_get_driver_data(dev);

	/* fill characteristics of DSI data link */
	plat->lanes = 4;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	/* do all the important stuff on the second panel */
	if (!priv->secondary)
		return 0;

	ret = gpio_request_by_name(dev, "reset-gpios", 0,
				   &priv->reset_gpio, GPIOD_IS_OUT);
	if (ret) {
		log_debug("%s: cannot decode reset-gpios (%d)\n",
			  __func__, ret);
		return ret;
	}

	for (int i = 0; i < WT0600_REGULATOR_COUNT; i++) {
		ret = device_get_supply_regulator(dev, wt0600_supplies[i], &priv->regulators[i]);
		if (ret) {
			dev_err(dev, "Failed to get supply regulator %s: %d\n", wt0600_supplies[i], ret);
			dm_gpio_free(dev, &priv->reset_gpio);
			return ret;
		}
	}

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev, "backlight", &priv->backlight);
	if (ret) {
		dev_err(dev, "Couldn't get backlight device! %d\n", ret);
		return ret;
	}

	ret = wt0600_set_regulators(priv, true);
	if (ret)
		return ret;

	wt0600_reset(priv);

	return 0;
}

static const struct panel_ops wt0600_ops = {
	.enable_backlight	= wt0600_enable_backlight,
	.set_backlight		= wt0600_set_backlight,
	.get_display_timing	= wt0600_get_display_timing,
};

static const struct udevice_id wt0600_ids[] = {
	{ .compatible = "ayaneo,wt0630-2k" },
	{ }
};

U_BOOT_DRIVER(wt0600) = {
	.name		= "wt0600",
	.id		= UCLASS_PANEL,
	.of_match	= wt0600_ids,
	.ops		= &wt0600_ops,
	.probe		= wt0600_probe,
	.bind		= wt0600_bind,
	.priv_auto	= sizeof(struct wt0600_priv),
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
};
