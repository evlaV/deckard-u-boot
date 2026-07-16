// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, Linaro Ltd. All rights reserved.
 */

#include <backlight.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <mipi_dsi.h>
#include <dm/lists.h>
#include <hexdump.h>
#include <panel.h>
#include <asm/gpio.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <power/regulator.h>
#include <drm/display/drm_dsc_helper.h>

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
#define LPM02648C_DISP_SET6			0xef
#define LPM02648C_TE_GPIO_CTL			0xbe
#define LPM02648C_PPS_SET			0xe6
#define LPM02648C_MFG_INFO			0xbf
#define 	VENDOR_OFFSET			27
#define 	VCOM120_OFFSET			13
#define 	VCOM90_OFFSET			15

// Brightness settings
#define BRIGHTNESS_DEFAULT 256
#define BRIGHTNESS_MAX_120 367

// Panel settings
#define VFP		700
#define VID_VS_DELAY	1112
#define VBP		23
#define GPO1_TES1_144_SHARP	150
#define GPO1_TES1_120_SHARP	100
#define GPO1_TES1_144_BOE	3435
#define GPO1_TES1_120_BOE	4068
#define GPO1_TES1_144_JDI	GPO1_TES1_144_BOE
#define GPO1_TES1_120_JDI	GPO1_TES1_120_BOE
#define GPO1_TES1_90		5446
#define GPO1_TEW1			0
#define RTN 59

// Panel Vendor types
#define VENDOR_SHARP	'S'
#define VENDOR_JDI		'J'
#define VENDOR_BOE		'B'

/*
 * Camera capture signal on panel tester is GPIO2 routed out TE pin
 * Need 100usec of offest on either side, so we use 50 lines at
 * 2usec / internal line time
 */
#define CAMERA_TRIGGER_OFFSET 50

#define PANEL_MFG_INFO_NUM_BYTES 40

enum {
	REGULATOR_VDDA = 0,
	REGULATOR_PREBOOST,
	REGULATOR_LEFT,
	REGULATOR_RIGHT,
	REGULATOR_BACKLIGHT_LEFT,
	REGULATOR_BACKLIGHT_RIGHT,
	REGULATOR_BACKLIGHT_LEFT_OLD,
	REGULATOR_BACKLIGHT_RIGHT_OLD,
	REGULATOR_COUNT,
};

// FIXME: the backlight pos/neg supplies are just grouped together right now
static const char *regulator_names[REGULATOR_COUNT] = {
	"vdda-supply",
	"preboost-supply",
	"left-pos-supply",
	// "left-neg-supply",
	"right-pos-supply",
	// "right-neg-supply",
	"backlight-left-supply",
	"backlight-right-supply",
	"backlight-left-supply-old",
	"backlight-right-supply-old"
};

static struct udevice *panels_regs[REGULATOR_COUNT];
static struct gpio_desc reset1_gpio;
static struct gpio_desc reset2_gpio;
static bool panels_power_probed = false;
static bool panels_powered_on = false;

struct deckard_panel_priv {
	struct udevice *dev;
	struct mipi_dsi_device *device;
	struct drm_dsc_config dsc_cfg;
	u8 panel_mfg_info[PANEL_MFG_INFO_NUM_BYTES];
	u8 panel_read_data[512];
	char panel_vendor;
};

/* 90Hz */
static const struct display_timing default_timing = {
	/* Dual DSI */
	.pixelclock.typ		= 2 * (2160 + 24 + 20 + 20) * (2160 + 1265 + 1 + 20) * 90,
	.hactive.typ		= 2 * 2160,
	.hfront_porch.typ	= 2 * 24,
	.hback_porch.typ	= 2 * 20,
	.hsync_len.typ		= 2 * 20,
	.vactive.typ		= 2160,
	.vfront_porch.typ	= 1265,
	.vback_porch.typ	= 20,
	.vsync_len.typ		= 1,
};

#define deckard_panel_dsi_write_seq(priv, cmd, seq...)					\
	do {										\
		u8 d[] = {cmd, seq};							\
		mipi_dsi_dcs_write_buffer(priv->device, d, ARRAY_SIZE(d));		\
	} while (0)

#define deckard_panel_dsi_write_buffer(priv, d)						\
	do {										\
		mipi_dsi_dcs_write_buffer(priv->device, d, ARRAY_SIZE(d));		\
	} while (0)

#define BE16(x) (x)>>8, (x)&0xff
#define LE16(x) (x)&0xff, (x)>>8

static int get_panel_info(struct deckard_panel_priv *priv)
{
	u8 panel_info_regs[] = { 0xBF };
	u8 *info = priv->panel_mfg_info;
	int ret;

	// print_hex_dump_bytes("[REFERENCE PPS] ", DUMP_PREFIX_OFFSET, &dumped_pps_cmd[0], sizeof(dumped_pps_cmd));

	ret = mipi_dsi_generic_read(priv->device, panel_info_regs, 1,
			info, PANEL_MFG_INFO_NUM_BYTES);
	if (ret < 0)
		dev_err(priv->dev, "failed to read panel info\n");

	return ret;
}

static void __maybe_unused read_reg(struct deckard_panel_priv *priv, u8 reg, u8 len) {
	u8 panel_info_regs[] = { reg };
	u8 info[256];
	int ret;

	if(reg >= 0xB0) {
		ret = mipi_dsi_generic_read(priv->device, panel_info_regs, 1,
				info, len);
	} else {
		ret = mipi_dsi_dcs_read(priv->device, reg,
				info, len);
	}
	if (ret < 0) {
		dev_err(priv->dev, "failed to read panel info %x\n", reg);
	} else {
		printf("reg: %#x: ", reg);
		for(uint i = 0; i < len; i++) {
			printf("%#04x ", info[i]);
		}
		printf("\n");
	}
}

static int __maybe_unused panel_info_show(struct deckard_panel_priv *priv)
{
	unsigned int offset = 0;
	u8 *info = priv->panel_mfg_info;

	printf("****** %s info:\n", priv->dev->name);
	printf("  SupplierID  %x\n",
			info[1+offset] << 8 | info[0+offset]);
	printf("  DriverICProductCode  %x\n",
			info[3+offset] << 8 | info[2+offset]);
	printf("  brightnessDataW255  %x\n", info[5+offset]);
	printf("  colorPointofWhiteX  %x\n",  info[6+offset]);
	printf("  colorPointofWhiteY  %x\n",  info[7+offset]);
	printf("  contrast  %x\n",  info[8+offset]);
	printf("  gamma  %x\n",  info[9+offset]);
	printf("  brightnessDataW32  %x\n",  info[12+offset]);
	printf("  vcom1  %x\n",  info[13+offset]);
	printf("  vcom2  %x\n",  info[14+offset]);
	printf("  colorPointofRedX  %x\n",  info[21+offset]);
	printf("  colorPointofRedY  %x\n",  info[22+offset]);
	printf("  colorPointofGreenX  %x\n",  info[23+offset]);
	printf("  colorPointofGreenY  %x\n",  info[24+offset]);
	printf("  colorPointofBlueX  %x\n",  info[25+offset]);
	printf("  colorPointofBlueY  %x\n",  info[26+offset]);
	printf("  format  %x\n",  info[27+offset]);
	printf("  vendor1  %x\n",  info[28+offset]);
	printf("  vendor2  %x\n",  info[29+offset]);
	printf("  mdlBaseCode1  %x\n",  info[30+offset]);
	printf("  mdlBaseCode2  %x\n",  info[31+offset]);
	printf("  mdlBaseCode3  %x\n",  info[32+offset]);
	printf("  id  %x\n",  info[33+offset]);
	printf("  serialization3  %x\n",  info[34+offset]);
	printf("  serialization2  %x\n",  info[35+offset]);
	printf("  serialization1  %x\n",  info[36+offset]);
	printf("  year  %x\n",  info[37+offset]);
	printf("  week  %x\n",  info[38+offset]);
	printf("  day  %x\n",  info[39+offset]);
	printf("\n");

	return 0;
}

static int dsi_write_panel_on(struct deckard_panel_priv *priv)
{
	u8 pps_cmd[1 + sizeof(struct drm_dsc_picture_parameter_set)];

	drm_dsc_pps_payload_pack((void *)&pps_cmd[1], &priv->dsc_cfg);
	pps_cmd[0] = LPM02648C_PPS_SET;

	// print_hex_dump_bytes("[PANEL PPS] ", DUMP_PREFIX_OFFSET, &pps_cmd[0], sizeof(pps_cmd));

	deckard_panel_dsi_write_seq(priv, LPM02648C_MF_CMD_ACCESS_PROTECT, 0x00);
	deckard_panel_dsi_write_seq(priv, LPM02648C_SEQ_CTL, 0x00);
	deckard_panel_dsi_write_seq(priv, LPM02648C_DSI_CTL,
				0x20, 0x6b, 0x80, 0x06, 0x33, 0x9a, 0x00,
				0x1a, 0x7a);
	deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_MODE, 0x54, 0x00, 0x00,
				0x00);
	// start with backlight off, will be set later
	deckard_panel_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				BE16(GPO1_TES1_120_BOE), BE16(0), 0x00, 0x00,
				0x00, 0x00,
				BE16(GPO1_TES1_120_BOE-CAMERA_TRIGGER_OFFSET),
				BE16(CAMERA_TRIGGER_OFFSET * 2 + 0));
	if ( priv->panel_vendor == VENDOR_SHARP ) {
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3, 0x08, 0x70, 0x28,
					0x48, 0x00, 0x00, 0x13, 0x21, 0xff, 0x00, 0x0f, 0x01,
					0x14, 0x17, 0x00, 0x00, 0x00, 0x02, 0x40, 0x0C, 0x00, 0x00,
					0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x70, 0x08,
					0xD0, 0x02, 0x21, 0x6F, 0x08, 0x5A, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET1, RTN, 0x86, LE16(VBP), 0x08, 0x70,
					BE16(VFP), 0x00, 0x00, 0x08, 0x3B, 0x00, 0x00, 0x19, 0x01, 0x22);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3_2, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET4, 0x8b, 0x00, 0x80,
					0x46, 0x61, 0x00, 0x8b);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET5, BE16(VID_VS_DELAY),
					0x00, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET6, 0x00, 0x24, 0x00, 0x00,
					0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00,
					0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x0A,
					0x00, 0x00, 0x00, 0x03, 0x1D, 0x01, 0x00, 0x00, 0x00, 0x01,
					0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_ADDRESS_MODE, 0x40);
	} else if ( priv->panel_vendor == VENDOR_BOE ) {
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3, 0x0B, 0x70, 0x08,
					  0x48, 0x00, 0x00, 0x13, 0x01, 0xFF, 0x00, 0x01, 0x09,
					  0x16, 0x16, 0x00, 0x00, 0x00, 0x01, 0x40, 0x0C, 0x00,
					  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
					  0x70, 0x08, 0xD0, 0x02, 0x21, 0x6F, 0x08, 0x5A, 0x00,
					  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET1, RTN, 0x86, LE16(VBP),
					  0x08, 0x70, BE16(VFP), 0x00, 0x00, 0x08, 0x3A, 0x00, 0x00,
					  0x17, 0x00, 0x17);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3_2, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET4, 0x8b, 0x00, 0x80,
					  0x46, 0x61, 0x00, 0x8b);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET5, BE16(VID_VS_DELAY),
					  0x00, 0x00, 0x00);
		// deckard_panel_dsi_write_seq(priv, LPM02648C_TE_GPIO_CTL, 0x00, 0x6A, 0x02);
	} else if ( priv->panel_vendor == VENDOR_JDI ) {
		deckard_panel_dsi_write_seq(priv, LPM02648C_MF_CMD_ACCESS_PROTECT, 0x04);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET2, 0x1e);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3, 0x70, 0x08, 0xd0,
					0x02, 0x21, 0x6f, 0x08, 0x5a, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET1, RTN, 0x86,
					LE16(VBP), 0x08, 0x70, BE16(VFP));
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET3_2, 0x00);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET4, 0x8b, 0x00, 0x80,
					0x46, 0x61, 0x00, 0x8b);
		deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_SET5, BE16(VID_VS_DELAY),
					0x00, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, MIPI_DCS_GET_COMPRESSION_MODE, 0x01);
		deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x00);
		deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_ADDRESS_MODE, 0x00);
		deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_PIXEL_FORMAT, 0x77);
	} else {
		dev_err(priv->dev, "unrecognized panel vendor ( %c / %d )\n", priv->panel_vendor, priv->panel_vendor );
		return -EIO;
	}
	deckard_panel_dsi_write_seq(priv, LPM02648C_TE_GPIO_CTL, 0x00, 0x6A, 0x02);
	deckard_panel_dsi_write_buffer(priv, pps_cmd);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_ON, 0x00);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	mdelay(170);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_DISPLAY_ON);

	return 0;
}

static int dsi_panel_on(struct deckard_panel_priv *priv)
{
	int ret;
	// Read panel MFG info
	// if we can't read info from the panels, don't continue to try initialization
	// we mainly hit this when bringing up new boards or on testers, where panels
	// are not connected, resulting in long boot times as each init command times out
	ret = get_panel_info(priv);
	if (ret < 0)
		return ret;

	// panel_info_show(priv);

	// need to support old format JDI displays
	if ( priv->panel_mfg_info[VENDOR_OFFSET] == 1 ) {
		priv->panel_vendor = priv->panel_mfg_info[VENDOR_OFFSET + 1];
	} else {
		priv->panel_vendor = priv->panel_mfg_info[VENDOR_OFFSET];
	}
	if ( priv->panel_vendor == VENDOR_SHARP || priv->panel_vendor == VENDOR_BOE || priv->panel_vendor == VENDOR_JDI ) {
		log_debug("panel vendor: %c\n", priv->panel_vendor);
	} else {
		dev_err(priv->dev, "Couldn't determine panel vendor %c (%x)", priv->panel_vendor, priv->panel_vendor);
		return -ENOENT;
		// priv->panel_vendor = VENDOR_SHARP;
		// log_debug("unrecognized panel vendor ( %c ), default to %c\n", priv->panel_mfg_info[VENDOR_OFFSET], priv->panel_vendor );
	}

	return dsi_write_panel_on(priv);
}

static int set_brightness(struct deckard_panel_priv *priv, u16 brightness, bool video_mode)
{
	if (video_mode)
		priv->device->mode_flags &= ~MIPI_DSI_MODE_LPM;

	read_reg(priv, LPM02648C_GEN_OUTPIN_SET, 12);

	// if (brightness) {
	// 	cam_trig_start = (gpo1_tes1 > CAMERA_TRIGGER_OFFSET) ?
	// 			 (gpo1_tes1 - CAMERA_TRIGGER_OFFSET) : 0;
	// 	cam_trig_width = gpo1_tew1 + CAMERA_TRIGGER_OFFSET*2;
	// }

	dev_info(priv->dev, "%s: %u\n", __func__, brightness);

	// deckard_panel_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
	// 			  BE16(gpo1_tes1), BE16(gpo1_tew1), 0x00, 0x00,
	// 			  0x00, 0x00, BE16(cam_trig_start),
	// 			  BE16(cam_trig_width));
	deckard_panel_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				    0x0f, 0xe4,
				    BE16(brightness),
				    0x0, 0x0,
				    0x0, 0x0,
				    0x0f, 0xb2,
				    BE16(brightness));

	read_reg(priv, LPM02648C_GEN_OUTPIN_SET, 12);

	if (video_mode)
		priv->device->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

static int dsi_populate_dsc_params(struct deckard_panel_priv *ctx)
{
	int ret;
	struct drm_dsc_config *dsc = &ctx->dsc_cfg;

	dsc->simple_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);

	/* handle only bpp = bpc = 8, pre-SCR panels */
	ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
	if (ret < 0)
		dev_err(ctx->dev, "failed to setup dsc params\n");

	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
	dsc->line_buf_depth = dsc->bits_per_component + 1;

	return drm_dsc_compute_rc_parameters(dsc);
}

static int lpm026m648c_init_dsc_config(struct deckard_panel_priv *ctx)
{
	ctx->dsc_cfg = (struct drm_dsc_config) {
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

	dev_dbg(ctx->dev, "%s!\n", __func__);

	return dsi_populate_dsc_params(ctx);
}

static int deckard_panel_panel_enable_backlight(struct udevice *dev)
{
	struct deckard_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);

	dev_info(dev, "%s panels_powered_on=%d\n", __func__, panels_powered_on);

	if (!panels_powered_on) {
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_PREBOOST], true);

		mdelay(70);

		regulator_set_enable_if_allowed(panels_regs[REGULATOR_BACKLIGHT_LEFT], true);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_BACKLIGHT_RIGHT], true);

		mdelay(2);

		regulator_set_enable_if_allowed(panels_regs[REGULATOR_VDDA], true);

		mdelay(2);

		regulator_set_enable_if_allowed(panels_regs[REGULATOR_LEFT], true);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_RIGHT], true);

		mdelay(2);

		dm_gpio_set_value(&reset1_gpio, 1);
		dm_gpio_set_value(&reset2_gpio, 1);

		mdelay(20);

		panels_powered_on = 1;
	}

	plat->device->mode_flags |= MIPI_DSI_MODE_LPM;

	return dsi_panel_on(priv);
}

static void deckard_panel_panel_power_off(void)
{
	if (panels_powered_on) {
		dm_gpio_set_value(&reset1_gpio, 0);
		dm_gpio_set_value(&reset2_gpio, 0);

		mdelay(20);

		regulator_set_enable_if_allowed(panels_regs[REGULATOR_PREBOOST], false);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_BACKLIGHT_LEFT], false);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_BACKLIGHT_RIGHT], false);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_VDDA], false);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_LEFT], false);
		regulator_set_enable_if_allowed(panels_regs[REGULATOR_RIGHT], false);

		panels_powered_on = 0;
	}
}

static int deckard_panel_panel_set_backlight(struct udevice *dev, int percent)
{
	struct deckard_panel_priv *priv = dev_get_priv(dev);

	if (!percent)
		deckard_panel_panel_power_off();
	else
		set_brightness(priv, DIV_ROUND_UP(percent * BRIGHTNESS_MAX_120, 100), true);

	return 0;
}

static int deckard_panel_panel_get_display_timing(struct udevice *dev,
						struct display_timing *timing)
{
	struct deckard_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	int ret;
	memcpy(timing, &default_timing, sizeof(*timing));

	/* We have to do this after DSI attach so that plat->device is valid */
	if (!plat->device->dsc) {
		priv->device = plat->device;
		ret = lpm026m648c_init_dsc_config(priv);
		if (ret) {
			dev_err(dev, "Couldn't initialize DSC configuration\n");
			return ret;
		}
		priv->device->dsc = &priv->dsc_cfg;
		/* Assuming dual-panel ? */
		priv->device->dsc_slice_per_pkt = 4;
	}

	return 0;
}

static int deckard_panel_panel_of_to_plat(struct udevice *dev)
{
	int ret, i;

	dev_info(dev, "%s: probed %d, seq %d\n", __func__, panels_power_probed, dev_seq(dev));

	if (!panels_power_probed) {
		printf("%s getting resources\n", __func__);
		ret = gpio_request_by_name(dev, "reset-gpios", 0,
					   &reset1_gpio, GPIOD_IS_OUT);
		if (ret) {
			dev_err(dev, "could not decode reset-gpios (%d)\n", ret);
			return ret;
		}

		ret = gpio_request_by_name(dev, "reset-gpios", 1,
					   &reset2_gpio, GPIOD_IS_OUT);
		if (ret) {
			dev_err(dev, "could not decode reset-gpios (%d)\n", ret);
			return ret;
		}

		// First try to get DV2+ backlight drivers
		ret = device_get_supply_regulator(dev, "backlight-left-supply", &panels_regs[REGULATOR_BACKLIGHT_LEFT]);
		ret |= device_get_supply_regulator(dev, "backlight-right-supply", &panels_regs[REGULATOR_BACKLIGHT_RIGHT]);
		if ( ret ) {
			//dev_err(dev, "error getting dv backlight power supplies, try ev version: ret=%d\n", ret);
			// look for DV1- backlight drivers
			ret = device_get_supply_regulator(dev, "backlight-left-supply-old", &panels_regs[REGULATOR_BACKLIGHT_LEFT]);
			ret= device_get_supply_regulator(dev, "backlight-right-supply-old", &panels_regs[REGULATOR_BACKLIGHT_RIGHT]);
			if (ret) {
				dev_err(dev, "error getting backlight power supplies ret=%d\n", ret);
				return ret;
			}
		}

		for (i = 0; i < REGULATOR_BACKLIGHT_LEFT; i++) {
			ret = device_get_supply_regulator(dev, regulator_names[i], &panels_regs[i]);
			if (ret) {
				dev_err(dev, "Cannot get power supply %s: ret=%d\n",
					  regulator_names[i], ret);
				return ret;
			}
		}

		panels_power_probed = 1;
	}

	return 0;
}

static int deckard_panel_panel_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct deckard_panel_priv *priv = dev_get_priv(dev);

	/* fill characteristics of DSI data link */
	plat->lanes = 3;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM |
			MIPI_DSI_CLOCK_NON_CONTINUOUS;

	priv->dev = dev;

	dm_gpio_set_value(&reset1_gpio, 0);
	dm_gpio_set_value(&reset2_gpio, 0);

	printf("%s\n", __func__);

	return 0;
}

static int deckard_panel_panel_remove(struct udevice *dev)
{
	deckard_panel_panel_power_off();

	return 0;
}

static const struct panel_ops deckard_panel_panel_ops = {
	.enable_backlight = deckard_panel_panel_enable_backlight,
	.set_backlight = deckard_panel_panel_set_backlight,
	.get_display_timing = deckard_panel_panel_get_display_timing,
};

static const struct udevice_id deckard_panel_panel_ids[] = {
	{ .compatible = "deckard-panel" },
	{ }
};

U_BOOT_DRIVER(deckard_panel) = {
	.name		= "deckard_panel",
	.id		= UCLASS_PANEL,
	.of_match	= deckard_panel_panel_ids,
	.ops		= &deckard_panel_panel_ops,
	.of_to_plat	= deckard_panel_panel_of_to_plat,
	.probe		= deckard_panel_panel_probe,
	.remove		= deckard_panel_panel_remove,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct deckard_panel_priv),
};
