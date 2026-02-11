// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025 Ion Agorria <ion@agorria.com>
 */

#include <backlight.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <panel.h>
#include <log.h>
#include <mipi_dsi.h>
#include <mipi_display.h>
#include <linux/delay.h>
#include <power/regulator.h>
#include <asm/gpio.h>

#define VTDR_REGULATOR_COUNT 3
#define VTDR_MAX_BRIGHTNESS 4095

static const char *visionox_vtdr6130_supplies[VTDR_REGULATOR_COUNT] = {
	 "vddio-supply",
	 "vci-supply",
	 "vdd-supply",
};

struct vtdr_priv {
	struct udevice *dev;
	struct mipi_dsi_device *dsi;

	struct udevice *regulators[VTDR_REGULATOR_COUNT];

	struct gpio_desc reset_gpio;
};

static const struct display_timing default_timing = {
	.pixelclock.typ		= (1080 + 20 + 2 + 20) * (2400 + 20 + 2 + 18) * 120, // 144
	.hactive.typ		= 1080,
	/* CASEY: might be backwards? */
	.hfront_porch.typ	= 20,
	.hback_porch.typ	= 20,
	.hsync_len.typ		= 2,

	.vactive.typ		= 2400,
	.vfront_porch.typ	= 20,
	.vback_porch.typ	= 18,
	.vsync_len.typ		= 2,
};

#define panel_dsi_write_seq(priv, cmd, seq...)					\
	do {										\
		u8 d[] = {cmd, seq};							\
		int ret; \
		if ((ret = mipi_dsi_dcs_write_buffer(priv->dsi, d, ARRAY_SIZE(d))) < 0)		\
			return 0; \
	} while (0)

#define panel_dsi_write_buffer(priv, d)						\
	do {										\
		int ret; \
		if ((ret = mipi_dsi_dcs_write_buffer(priv->dsi, d, ARRAY_SIZE(d))) < 0)		\
			return 0; \
	} while (0)

#define BE16(x) (x)>>8, (x)&0xff
#define LE16(x) (x)&0xff, (x)>>8


static int visionox_vtdr6130_on(struct vtdr_priv *priv)
{
	struct mipi_dsi_device *dsi = priv->dsi;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	panel_dsi_write_seq(priv, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_set_display_brightness_large(dsi, 0);
	panel_dsi_write_seq(priv, 0x59, 0x09);
	panel_dsi_write_seq(priv, 0x6c, 0x01);
	panel_dsi_write_seq(priv, 0x6d, 0x00);
	panel_dsi_write_seq(priv, 0x6f, 0x01);
	panel_dsi_write_seq(priv, 0x70, 0x12, 0x00, 0x00, 0xab,
				     0x30, 0x80, 0x09, 0x60, 0x04, 0x38, 0x00,
				     0x28, 0x02, 0x1c, 0x02, 0x1c, 0x02, 0x00,
				     0x02, 0x0e, 0x00, 0x20, 0x03, 0xdd, 0x00,
				     0x07, 0x00, 0x0c, 0x02, 0x77, 0x02, 0x8b,
				     0x18, 0x00, 0x10, 0xf0, 0x07, 0x10, 0x20,
				     0x00, 0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c,
				     0x2a, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70,
				     0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x02, 0x02,
				     0x22, 0x00, 0x2a, 0x40, 0x2a, 0xbe, 0x3a,
				     0xfc, 0x3a, 0xfa, 0x3a, 0xf8, 0x3b, 0x38,
				     0x3b, 0x78, 0x3b, 0xb6, 0x4b, 0xb6, 0x4b,
				     0xf4, 0x4b, 0xf4, 0x6c, 0x34, 0x84, 0x74,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	panel_dsi_write_seq(priv, 0xf0, 0xaa, 0x10);
	panel_dsi_write_seq(priv, 0xb1, 0x01, 0x38, 0x00, 0x14,
				     0x00, 0x1c, 0x00, 0x01, 0x66, 0x00, 0x14,
				     0x00, 0x14, 0x00, 0x01, 0x66, 0x00, 0x14,
				     0x05, 0xcc, 0x00);
	panel_dsi_write_seq(priv, 0xf0, 0xaa, 0x13);
	panel_dsi_write_seq(priv, 0xce, 0x09, 0x11, 0x09, 0x11,
				     0x08, 0xc1, 0x07, 0xfa, 0x05, 0xa4, 0x00,
				     0x3c, 0x00, 0x34, 0x00, 0x24, 0x00, 0x0c,
				     0x00, 0x0c, 0x04, 0x00, 0x35);
	panel_dsi_write_seq(priv, 0xf0, 0xaa, 0x14);
	panel_dsi_write_seq(priv, 0xb2, 0x03, 0x33);
	panel_dsi_write_seq(priv, 0xb4, 0x00, 0x33, 0x00, 0x00,
				     0x00, 0x3e, 0x00, 0x00, 0x00, 0x3e, 0x00,
				     0x00);
	panel_dsi_write_seq(priv, 0xb5, 0x00, 0x09, 0x09, 0x09,
				     0x09, 0x09, 0x09, 0x06, 0x01);
	panel_dsi_write_seq(priv, 0xb9, 0x00, 0x00, 0x08, 0x09,
				     0x09, 0x09);
	panel_dsi_write_seq(priv, 0xbc, 0x10, 0x00, 0x00, 0x06,
				     0x11, 0x09, 0x3b, 0x09, 0x47, 0x09, 0x47,
				     0x00);
	panel_dsi_write_seq(priv, 0xbe, 0x10, 0x10, 0x00, 0x08,
				     0x22, 0x09, 0x19, 0x09, 0x25, 0x09, 0x25,
				     0x00);
	panel_dsi_write_seq(priv, 0xff, 0x5a, 0x80);
	panel_dsi_write_seq(priv, 0x65, 0x14);
	panel_dsi_write_seq(priv, 0xfa, 0x08, 0x08, 0x08);
	panel_dsi_write_seq(priv, 0xff, 0x5a, 0x81);
	panel_dsi_write_seq(priv, 0x65, 0x05);
	panel_dsi_write_seq(priv, 0xf3, 0x0f);
	panel_dsi_write_seq(priv, 0xf0, 0xaa, 0x00);
	panel_dsi_write_seq(priv, 0xff, 0x5a, 0x82);
	panel_dsi_write_seq(priv, 0xf9, 0x00);
	panel_dsi_write_seq(priv, 0xff, 0x51, 0x83);
	panel_dsi_write_seq(priv, 0x65, 0x04);
	panel_dsi_write_seq(priv, 0xf8, 0x00);
	panel_dsi_write_seq(priv, 0xff, 0x5a, 0x00);
	panel_dsi_write_seq(priv, 0x65, 0x01);
	panel_dsi_write_seq(priv, 0xf4, 0x9a);
	panel_dsi_write_seq(priv, 0xff, 0x5a, 0x00);

	mipi_dsi_dcs_exit_sleep_mode(dsi);
	mdelay(120);

	mipi_dsi_dcs_set_display_on(dsi);
	mdelay(20);

	return 0;
}

static void visionox_vtdr6130_reset(struct vtdr_priv *priv)
{
	dm_gpio_set_value(&priv->reset_gpio, 0);
	mdelay(11);
	dm_gpio_set_value(&priv->reset_gpio, 1);
	mdelay(11);
	dm_gpio_set_value(&priv->reset_gpio, 0);
	mdelay(11);
}

static int visionox_vtdr6130_set_regulators(struct vtdr_priv *priv, bool enable)
{
	int ret;

	for (int i = 0; i < VTDR_REGULATOR_COUNT; i++) {
		ret = regulator_set_enable_if_allowed(priv->regulators[i], true);
		if (ret) {
			dev_err(priv->dev, "Failed to enable regulator %s: %d\n", visionox_vtdr6130_supplies[i], ret);
			return ret;
		}
	}

	return 0;
}

static int visionox_vtdr6130_enable_backlight(struct udevice *dev)
{
	struct vtdr_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	int ret;

	/* FIXME: shouldn't copy this... */
	priv->dsi = plat->device;

	ret = visionox_vtdr6130_set_regulators(priv, true);
	if (ret)
		return ret;

	visionox_vtdr6130_reset(priv);

	ret = visionox_vtdr6130_on(priv);
	if (ret < 0) {
		visionox_vtdr6130_set_regulators(priv, false);
		dm_gpio_set_value(&priv->reset_gpio, 1);
		return ret;
	}

	/* Now set a brightness so we can see the screen */
	if (mipi_dsi_dcs_set_display_brightness_large(priv->dsi, 3200)) {
		dev_err(dev, "Set default brightness failed!\n");
		visionox_vtdr6130_set_regulators(priv, false);
		dm_gpio_set_value(&priv->reset_gpio, 1);
		return -ENOSYS;
	}

	return 0;
}

static int visionox_vtdr6130_set_backlight(struct udevice *dev, int percent)
{
	struct vtdr_priv *priv = dev_get_priv(dev);
	int ret;

	ret = mipi_dsi_dcs_set_display_brightness_large(priv->dsi, (percent * VTDR_MAX_BRIGHTNESS) / 100);
	if (ret)
		dev_err(dev, "Set brightness failed: %d!\n", ret);

	return 0;
}

static int visionox_vtdr6130_get_display_timing(struct udevice *dev,
					       struct display_timing *timing)
{
	memcpy(timing, &default_timing, sizeof(*timing));
	return 0;
}

static int visionox_vtdr6130_of_to_plat(struct udevice *dev)
{
	struct vtdr_priv *priv = dev_get_priv(dev);
	int ret;
	
	priv->dev = dev;

	for (int i = 0; i < VTDR_REGULATOR_COUNT; i++) {
		ret = device_get_supply_regulator(dev, visionox_vtdr6130_supplies[i], &priv->regulators[i]);
		if (ret) {
			dev_err(dev, "Failed to get supply regulator %s: %d\n", visionox_vtdr6130_supplies[i], ret);
			return ret;
		}
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0,
				   &priv->reset_gpio, GPIOD_IS_OUT);
	if (ret) {
		log_debug("%s: cannot decode reset-gpios (%d)\n",
			  __func__, ret);
		return ret;
	}

	return 0;
}

static int visionox_vtdr6130_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);

	/* fill characteristics of DSI data link */
	plat->lanes = 4;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO |
			MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_EOT_PACKET;

	dev_info(dev, "%s!\n", __func__);

	return 0;
}

static const struct panel_ops visionox_vtdr6130_ops = {
	.enable_backlight	= visionox_vtdr6130_enable_backlight,
	.set_backlight		= visionox_vtdr6130_set_backlight,
	.get_display_timing	= visionox_vtdr6130_get_display_timing,
};

static const struct udevice_id visionox_vtdr6130_ids[] = {
	{ .compatible = "visionox,vtdr6130" },
	{ }
};

U_BOOT_DRIVER(visionox_vtdr6130) = {
	.name		= "visionox_vtdr6130",
	.id		= UCLASS_PANEL,
	.of_match	= visionox_vtdr6130_ids,
	.ops		= &visionox_vtdr6130_ops,
	.of_to_plat	= visionox_vtdr6130_of_to_plat,
	.probe		= visionox_vtdr6130_probe,
	.priv_auto	= sizeof(struct vtdr_priv),
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
};
