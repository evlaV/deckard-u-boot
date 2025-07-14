// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023-2024, The Valve Ltd. All rights reserved.
 * Copyright (c) 2024, The Linaro Ltd. All rights reserved.
 */

#include <backlight.h>
#include <dm.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <asm/gpio.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <power/regulator.h>

/* panel specific MIPI DSI CMD */
#define LPM02648C_MF_CMD_ACCESS_PROTECT		0xb0
#define LPM02648C_SEQ_CTL			0xd6
#define LPM02648C_DSI_CTL			0xb6
#define LPM02648C_DISP_MODE			0xb7
#define LPM02648C_GEN_OUTPIN_SET		0xb9
#define LPM02648C_DISP_SET1			0xc0
#define LPM02648C_DISP_SET2			0xf1
#define LPM02648C_DISP_SET3			0xc6
#define LPM02648C_DISP_SET3_2			0xcd
#define LPM02648C_DISP_SET4			0xcf
#define LPM02648C_DISP_SET5			0xec
#define LPM02648C_TE_GPIO_CTL			0xbe
#define LPM02648C_PPS_SET			0xe6

// Brightness settings
#define BRIGHTNESS_DEFAULT 256
#define BRIGHTNESS_MAX_120 380

#define VFP		700
#define VID_VS_DELAY	400
#define VBP		800
#define GPO1_TES1	233
#define GPO1_TEW1	17

/*
 * Camera capture signal on panel tester is GPIO2 routed out TE pin
 * Need 100usec of offest on either side, so we use 50 lines at
 * 2usec / internal line time
 */
#define CAMERA_TRIGGER_OFFSET 50

#define PANEL_MFG_INFO_NUM_BYTES 40
#define RESET_NUM 2

struct lpm026m648c_priv {
	struct udevice *dev;
	struct udevice *vdd;
	struct udevice *backlight;

	struct gpio_desc reset1_gpio;
	struct gpio_desc reset2_gpio;
	struct gpio_desc enp_gpio;

	struct mipi_dsi_device *device;
	struct mipi_dsi_device *device2;
	u8 panel_mfg_info[PANEL_MFG_INFO_NUM_BYTES*2];
	u8 panel_read_data[512];

/*
	struct drm_dsc_config dsc_cfg;
*/
	bool is_dual_panel;
};

static const struct display_timing default_timing = {
	.pixelclock.typ		= (2160 + 24 + 20 + 20) * (2160 + 1697 + 1 + 20) * 80,
	.hactive.typ		= 2160,
	.hfront_porch.typ	= 24,
	.hback_porch.typ	= 20,
	.hsync_len.typ		= 20,
	.vactive.typ		= 2160,
	.vfront_porch.typ	= 1697,
	.vback_porch.typ	= 20,
	.vsync_len.typ		= 1,
};

#define lpm026m648c_dsi_write_seq(priv, cmd, seq...)					\
	do {										\
		u8 d[] = {cmd, seq};							\
		mipi_dsi_dcs_write_buffer(priv->device, d, ARRAY_SIZE(d));		\
		if (priv->is_dual_panel) {						\
			mipi_dsi_dcs_write_buffer(priv->device2, d, ARRAY_SIZE(d));	\
		}									\
	} while (0)

#define lpm026m648c_dsi_write_buffer(priv, d)						\
	do {										\
		mipi_dsi_dcs_write_buffer(priv->device, d, ARRAY_SIZE(d));		\
		if (priv->is_dual_panel) {						\
			mipi_dsi_dcs_write_buffer(priv->device2, d, ARRAY_SIZE(d));	\
		}									\
	} while (0)

#define BE16(x) (x)>>8, (x)&0xff
#define LE16(x) (x)&0xff, (x)>>8

#if 0
static int dsi_populate_dsc_params(struct lpm026m648c_priv *priv)
{
	int ret;
	struct drm_dsc_config *dsc = &priv->dsc_cfg;

	dsc->simple_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);

	/* handle only bpp = bpc = 8, pre-SCR panels */
	ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
	if (ret < 0)
		dev_err(priv->dev, "failed to setup dsc params\n");

	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
	dsc->line_buf_depth = dsc->bits_per_component + 1;

	return drm_dsc_compute_rc_parameters(dsc);
}

static int lpm026m648c_init_dsc_config(struct lpm026m648c_priv *priv)
{
	priv->dsc_cfg = (struct drm_dsc_config) {
		.dsc_version_major = 0x1,
		.dsc_version_minor = 0x2,
		.slice_height = 48,
		.slice_width = 540,
		.slice_count = 4,
		.bits_per_component = 8,
		.bits_per_pixel = 8 << 4,
		.block_pred_enable = true,

		.pic_width = 2160,
		.pic_height = 2160,
	};

	return dsi_populate_dsc_params(priv);
}
#endif

static void dsi_write_panel_on(struct lpm026m648c_priv *priv)
{
/*
	u8 pps_cmd[1 + sizeof(struct drm_dsc_picture_parameter_set)];

	drm_dsc_pps_payload_pack((void *)&pps_cmd[1], &priv->dsc_cfg);
	pps_cmd[0] = LPM02648C_PPS_SET;
*/

	lpm026m648c_dsi_write_seq(priv, LPM02648C_MF_CMD_ACCESS_PROTECT, 0x04);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_SEQ_CTL, 0x00);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DSI_CTL,
				  0x20, 0x6b, 0x80, 0x06, 0x33, 0x8a, 0x00,
				  0x1a, 0x7a);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_MODE, 0x54, 0x00, 0x00,
				  0x00);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				  BE16(GPO1_TES1), BE16(GPO1_TEW1), 0x00, 0x00,
				  0x00, 0x00,
				  BE16(GPO1_TES1-CAMERA_TRIGGER_OFFSET),
				  BE16(CAMERA_TRIGGER_OFFSET * 2 + GPO1_TEW1));
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET2, 0x1e);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET3, 0x70, 0x08, 0xd0,
				  0x02, 0x21, 0x6f, 0x08, 0x5a, 0x00, 0x00,
				  0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET1, 0x42, 0x86,
				  LE16(VBP), 0x08, 0x70, BE16(VFP));
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET3_2, 0x00);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET4, 0x8b, 0x00, 0x80,
				  0x46, 0x61, 0x00, 0x8b);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_DISP_SET5, BE16(VID_VS_DELAY),
				  0x00, 0x00, 0x00);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_TE_GPIO_CTL, 0x00, 0x6A, 0x02);
	/* TODO: add hard coded dsc cmd first, then function routine */
//	lpm026m648c_dsi_write_buffer(priv, pps_cmd);
//	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_GET_COMPRESSION_MODE, 0x01);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x00);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_ON, 0x00);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_SET_ADDRESS_MODE, 0x00);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_SET_PIXEL_FORMAT, 0x77);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	mdelay(170);
	lpm026m648c_dsi_write_seq(priv, MIPI_DCS_SET_DISPLAY_ON);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_SEQ_CTL, 0x80);
	mdelay(200);
	lpm026m648c_dsi_write_seq(priv, LPM02648C_MF_CMD_ACCESS_PROTECT, 0x03);
	mdelay(200);
}

static int panel_info_show(struct lpm026m648c_priv *priv)
{
	unsigned int id, offset, panel_num;
	u8 *info = priv->panel_mfg_info;

	panel_num = priv->is_dual_panel ? 2 : 1;
	for (id = 0; id < panel_num; id++) {
		offset =  id*PANEL_MFG_INFO_NUM_BYTES;

		printf("****** JDI Panel info for %s panel:\n",
			   id ? "right" : "left");
		printf(" SupplierID  %x\n",
			   info[1+offset] << 8 | info[0+offset]);
		printf(" DriverICProductCode  %x\n",
			   info[3+offset] << 8 | info[2+offset]);
		printf(" brightnessDataW255  %x\n", info[5+offset]);
		printf(" colorPointofWhiteX  %x\n",  info[6+offset]);
		printf(" colorPointofWhiteY  %x\n",  info[7+offset]);
		printf(" contrast  %x\n",  info[8+offset]);
		printf(" gamma  %x\n",  info[9+offset]);
		printf(" brightnessDataW32  %x\n",  info[12+offset]);
		printf(" vcom1  %x\n",  info[13+offset]);
		printf(" vcom2  %x\n",  info[14+offset]);
		printf(" colorPointofRedX  %x\n",  info[21+offset]);
		printf(" colorPointofRedY  %x\n",  info[22+offset]);
		printf(" colorPointofGreenX  %x\n",  info[23+offset]);
		printf(" colorPointofGreenY  %x\n",  info[24+offset]);
		printf(" colorPointofBlueX  %x\n",  info[25+offset]);
		printf(" colorPointofBlueY  %x\n",  info[26+offset]);
		printf(" format  %x\n",  info[27+offset]);
		printf(" vendor1  %x\n",  info[28+offset]);
		printf(" vendor2  %x\n",  info[29+offset]);
		printf(" mdlBaseCode1  %x\n",  info[30+offset]);
		printf(" mdlBaseCode2  %x\n",  info[31+offset]);
		printf(" mdlBaseCode3  %x\n",  info[32+offset]);
		printf(" id  %x\n",  info[33+offset]);
		printf(" serialization3  %x\n",  info[34+offset]);
		printf(" serialization2  %x\n",  info[35+offset]);
		printf(" serialization1  %x\n",  info[36+offset]);
		printf(" year  %x\n",  info[37+offset]);
		printf(" week  %x\n",  info[38+offset]);
		printf(" day  %x\n",  info[39+offset]);
	}
	return 0;
}

static void get_panel_info(struct lpm026m648c_priv *priv)
{
	struct mipi_dsi_device *device;
	unsigned int id, offset, panel_num;
	int ret;
	u8 panel_info_regs[] = { 0xBF };
	u8 *info = priv->panel_mfg_info;

	panel_num = priv->is_dual_panel ? 2 : 1;
	for (id = 0; id < panel_num; id++) {
		offset =  id*PANEL_MFG_INFO_NUM_BYTES;
		device = id ? priv->device2 : priv->device;
		ret = mipi_dsi_generic_read(device, panel_info_regs, 1,
				&info[offset], PANEL_MFG_INFO_NUM_BYTES);
		if (ret < 0)
			dev_err(priv->dev, "failed to read panel%d info\n", id);
	}

	panel_info_show(priv);
}

static int set_brightness(struct lpm026m648c_priv *priv, u16 brightness,
			  u16 pulse_offset_rows)
{
	u16 gpo1_tes1 = pulse_offset_rows;
	u16 gpo1_tew1;
	u16 cam_trig_start = 0, cam_trig_width = 0;

	gpo1_tew1 = brightness > BRIGHTNESS_MAX_120 ?
		    BRIGHTNESS_MAX_120 : brightness;

	priv->device->mode_flags &= ~MIPI_DSI_MODE_LPM;
	if (priv->is_dual_panel)
		priv->device2->mode_flags &= ~MIPI_DSI_MODE_LPM;

	if (brightness) {
		cam_trig_start = (gpo1_tes1 > CAMERA_TRIGGER_OFFSET) ?
				 (gpo1_tes1 - CAMERA_TRIGGER_OFFSET) : 0;
		cam_trig_width = gpo1_tew1 + CAMERA_TRIGGER_OFFSET*2;
	}

	lpm026m648c_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				  BE16(gpo1_tes1), BE16(gpo1_tew1), 0x00, 0x00,
				  0x00, 0x00, BE16(cam_trig_start),
				  BE16(cam_trig_width));

	priv->device->mode_flags |= MIPI_DSI_MODE_LPM;
	if (priv->is_dual_panel)
		priv->device2->mode_flags |= MIPI_DSI_MODE_LPM;
	return 0;
}

static int lpm026m648c_panel_enable_backlight(struct udevice *dev)
{
	struct lpm026m648c_priv *priv = dev_get_priv(dev);

	priv->device->mode_flags |= MIPI_DSI_MODE_LPM;
	if (priv->is_dual_panel)
		priv->device2->mode_flags |= MIPI_DSI_MODE_LPM;

	mdelay(20);
	dsi_write_panel_on(priv);
	get_panel_info(priv);

	set_brightness(priv, 200, GPO1_TES1);

	return 0;
}

static int lpm026m648c_panel_get_display_timing(struct udevice *dev,
						struct display_timing *timing)
{
	memcpy(timing, &default_timing, sizeof(*timing));
	return 0;
}

static int lpm026m648c_panel_of_to_plat(struct udevice *dev)
{
	struct lpm026m648c_priv *priv = dev_get_priv(dev);
	int ret;

printf("============%s called\n", __func__);
/*
	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &priv->backlight);
	if (ret) {
		log_err("cannot get backlight: ret = %d\n", ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "vdda-supply", &priv->vdd);
	if (ret) {
		dev_err(dev, "cannot get vdd-supply: ret = %d\n", ret);
		return ret;
	}
*/

	ret = gpio_request_by_name(dev, "enp-gpios", 0,
				   &priv->enp_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "could not decode enp-gpios (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset1-gpios", 0,
				   &priv->reset1_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "could not decode reset1-gpios (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset2-gpios", 0,
				   &priv->reset2_gpio, GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "could not decode reset2-gpios (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int lpm026m648c_panel_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct lpm026m648c_priv *priv = dev_get_priv(dev);
	int ret;

printf("============%s called\n", __func__);
	/* fill characteristics of DSI data link */
	plat->lanes = 3;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM |
			MIPI_DSI_CLOCK_NON_CONTINUOUS;

	priv->dev = dev;
/*
	ret = regulator_set_enable_if_allowed(priv->vdd, 1);
	if (ret) {
		dev_err(dev, "%s: enabling vdd-supply failed (%d)\n",
			  __func__, ret);
		return ret;
	}
*/

	mdelay(2);
        dm_gpio_set_value(&priv->enp_gpio, 1);
	mdelay(20);
	dm_gpio_set_value(&priv->reset1_gpio, 1);
	dm_gpio_set_value(&priv->reset2_gpio, 1);

printf("============%s ret 0\n", __func__);
	return 0;
}

static const struct panel_ops lpm026m648c_panel_ops = {
	.enable_backlight = lpm026m648c_panel_enable_backlight,
	.get_display_timing = lpm026m648c_panel_get_display_timing,
};

static const struct udevice_id lpm026m648c_panel_ids[] = {
	{ .compatible = "jdi,lpm026m648c" },
	{ }
};

U_BOOT_DRIVER(lpm026m648c_panel) = {
	.name		= "lpm026m648c_panel",
	.id		= UCLASS_PANEL,
	.of_match	= lpm026m648c_panel_ids,
	.ops		= &lpm026m648c_panel_ops,
	.of_to_plat	= lpm026m648c_panel_of_to_plat,
	.probe		= lpm026m648c_panel_probe,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct lpm026m648c_priv),
};
