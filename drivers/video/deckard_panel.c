// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024, Linaro Ltd. All rights reserved.
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

static const char *regulator_names[REGULATOR_COUNT] = {
	"vdda-supply",
	"preboost-supply",
	"left-supply",
	"right-supply",
	"backlight-left-supply",
	"backlight-right-supply",
	"backlight-left-supply-old",
	"backlight-right-supply-old"
};

struct udevice *panels_regs[REGULATOR_COUNT];
struct gpio_desc reset1_gpio;
struct gpio_desc reset2_gpio;
static bool panels_power_probed;
static bool panels_powered_on;

struct deckard_panel_priv {
	struct udevice *dev;
	struct mipi_dsi_device *device;
	u8 panel_mfg_info[PANEL_MFG_INFO_NUM_BYTES];
	u8 panel_read_data[512];
	char panel_vendor;
};

/* 90Hz */
static const struct display_timing default_timing = {
	.pixelclock.typ		= (2160 + 24 + 20 + 20) * (2160 + 1265 + 1 + 20) * 80,
	.hactive.typ		= 2160,
	.hfront_porch.typ	= 24,
	.hback_porch.typ	= 20,
	.hsync_len.typ		= 20,
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

/* dump from Linux kernel */
static u8 pps_cmd[] = {
	LPM02648C_PPS_SET,
	0x12, 0x00, 0x00, 0x89, 0x30, 0x80, 0x08, 0x70, 0x08, 
	0x70, 0x00, 0x30, 0x02, 0x1c, 0x02, 0x1c, 0x02, 0x00, 
	0x02, 0x0e, 0x00, 0x20, 0x04, 0xa6, 0x00, 0x07, 0x00, 
	0x0c, 0x02, 0x0b, 0x02, 0x1f, 0x18, 0x00, 0x10, 0xf0, 

	0x03, 0x0c, 0x20, 0x00, 0x06, 0x0b, 0x0b, 0x33, 0x0e, 
	0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 
	0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09, 
	0x40, 0x09, 0xbe, 0x19, 0xfc, 0x19, 0xfa, 0x19, 0xf8,

	0x1a, 0x38, 0x1a, 0x78, 0x1a, 0xb6, 0x2a, 0xf6, 0x2b, 
	0x34, 0x2b, 0x74, 0x3b, 0x74, 0x6b, 0xf4, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
	0x00, 0x00, 
};

static void get_panel_info(struct deckard_panel_priv *priv)
{
	u8 panel_info_regs[] = { 0xBF };
	u8 *info = priv->panel_mfg_info;
	int ret;

	ret = mipi_dsi_generic_read(priv->device, panel_info_regs, 1,
			info, PANEL_MFG_INFO_NUM_BYTES);
	if (ret < 0)
		dev_err(priv->dev, "failed to read panel info\n");
}

#if 0
static void read_reg(struct deckard_panel_priv *priv, u8 reg, u8 len) {
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
		printf("reg: %x: ", reg);
		for(uint i = 0; i < len; i++) {
			printf("%02x ", info[i]);
		}
		printf("\n");
	}
}

static int panel_info_show(struct deckard_panel_priv *priv)
{
	unsigned int offset = 0;
	u8 *info = priv->panel_mfg_info;

	printf("****** Panel info:\n");
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

	return 0;
}
#endif

static void dsi_write_panel_on(struct deckard_panel_priv *priv)
{
	deckard_panel_dsi_write_seq(priv, LPM02648C_MF_CMD_ACCESS_PROTECT, 0x00);
	deckard_panel_dsi_write_seq(priv, LPM02648C_SEQ_CTL, 0x00);
	deckard_panel_dsi_write_seq(priv, LPM02648C_DSI_CTL,
				0x20, 0x6b, 0x80, 0x06, 0x33, 0x9a, 0x00,
				0x1a, 0x7a);
	deckard_panel_dsi_write_seq(priv, LPM02648C_DISP_MODE, 0x54, 0x00, 0x00,
				0x00);
	// start with backlight off, will be set later
	deckard_panel_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				BE16(GPO1_TES1_120_SHARP), BE16(0), 0x00, 0x00,
				0x00, 0x00,
				BE16(GPO1_TES1_120_SHARP-CAMERA_TRIGGER_OFFSET),
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
		deckard_panel_dsi_write_seq(priv, LPM02648C_TE_GPIO_CTL, 0x00, 0x6A, 0x02);
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
		dev_err(priv->dev, "unrecognized panel vendor ( %c )\n", priv->panel_vendor );
	}
	deckard_panel_dsi_write_seq(priv, LPM02648C_TE_GPIO_CTL, 0x00, 0x6A, 0x02);
	deckard_panel_dsi_write_buffer(priv, pps_cmd);
	//deckard_panel_dsi_write_seq(priv, MIPI_DCS_GET_COMPRESSION_MODE, 0x01);
	//deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_SCANLINE, 0x00, 0x00);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_TEAR_ON, 0x00);
	//deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_PIXEL_FORMAT, 0x77);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_EXIT_SLEEP_MODE);
	mdelay(170);
	deckard_panel_dsi_write_seq(priv, MIPI_DCS_SET_DISPLAY_ON);
}

static void dsi_panel_on(struct deckard_panel_priv *priv)
{
	// Read panel MFG info
	get_panel_info(priv);

	//panel_info_show(priv);

	// need to support old format JDI displays
	if ( priv->panel_mfg_info[VENDOR_OFFSET] == 1 ) {
		priv->panel_vendor = priv->panel_mfg_info[VENDOR_OFFSET + 1];
	} else {
		priv->panel_vendor = priv->panel_mfg_info[VENDOR_OFFSET];
	}
	if ( priv->panel_vendor == VENDOR_SHARP || priv->panel_vendor == VENDOR_BOE || priv->panel_vendor == VENDOR_JDI ) {
		dev_err(priv->dev, "panel vendor: %c\n", priv->panel_vendor);
	} else {
		dev_err(priv->dev, "unrecognized panel vendor ( %c ), defualt to sharp\n", priv->panel_mfg_info[VENDOR_OFFSET] );
		priv->panel_vendor = VENDOR_SHARP;
	}

	dsi_write_panel_on(priv);
}

static int set_brightness(struct deckard_panel_priv *priv, u16 brightness)
{
	u16 gpo1_tes1 = GPO1_TES1_120_BOE;
	u16 gpo1_tew1;
	u16 cam_trig_start = 0, cam_trig_width = 0;
	u16 votal_120 = 2160 + 404 + 1 + 20;
	u16 votal_90 = 2160 + 404 + 1 + 20;

	gpo1_tew1 = brightness > BRIGHTNESS_MAX_120 ?
		    BRIGHTNESS_MAX_120 : brightness;

	// Scale brightness based on current mode
	gpo1_tew1 = gpo1_tew1 * votal_90 / votal_120;
	//gpo1_tes1 = GPO1_TES1_90;

#if 0
	priv->device->mode_flags &= ~MIPI_DSI_MODE_LPM;
#endif

	if (brightness) {
		cam_trig_start = (gpo1_tes1 > CAMERA_TRIGGER_OFFSET) ?
				 (gpo1_tes1 - CAMERA_TRIGGER_OFFSET) : 0;
		cam_trig_width = gpo1_tew1 + CAMERA_TRIGGER_OFFSET*2;
	}

	deckard_panel_dsi_write_seq(priv, LPM02648C_GEN_OUTPIN_SET,
				  BE16(gpo1_tes1), BE16(gpo1_tew1), 0x00, 0x00,
				  0x00, 0x00, BE16(cam_trig_start),
				  BE16(cam_trig_width));

#if 0
	priv->device->mode_flags |= MIPI_DSI_MODE_LPM;
#endif

	return 0;
}

static int deckard_panel_panel_enable_backlight(struct udevice *dev)
{
	struct deckard_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);

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
	priv->device = plat->device;

	dsi_panel_on(priv);

	set_brightness(priv, BRIGHTNESS_DEFAULT);

	return 0;
}

static int deckard_panel_panel_set_backlight(struct udevice *dev, int percent)
{
	return set_brightness(dev_get_priv(dev), percent * BRIGHTNESS_MAX_120 / 100);
}

static int deckard_panel_panel_get_display_timing(struct udevice *dev,
						struct display_timing *timing)
{
	memcpy(timing, &default_timing, sizeof(*timing));
	return 0;
}

static int deckard_panel_panel_of_to_plat(struct udevice *dev)
{
	int ret, i;

	if (!panels_power_probed) {
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
			dev_err(dev, "Cannot get backlight power supplies, try old version: ret=%d\n", ret);
			// look for DV1- backlight drivers
			ret = device_get_supply_regulator(dev, "backlight-left-supply-old", &panels_regs[REGULATOR_BACKLIGHT_LEFT]);
			ret= device_get_supply_regulator(dev, "backlight-right-supply-old", &panels_regs[REGULATOR_BACKLIGHT_RIGHT]);
			if (ret) {
				dev_err(dev, "Cannot get backlight power supplies ret=%d\n", ret);
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

	return 0;
}

static const struct panel_ops deckard_panel_panel_ops = {
	.enable_backlight = deckard_panel_panel_enable_backlight,
	.set_backlight = deckard_panel_panel_set_backlight,
	.get_display_timing = deckard_panel_panel_get_display_timing,
};

static const struct udevice_id deckard_panel_panel_ids[] = {
	{ .compatible = "deckard_panel" },
	{ }
};

U_BOOT_DRIVER(deckard_panel_panel) = {
	.name		= "deckard_panel_panel",
	.id		= UCLASS_PANEL,
	.of_match	= deckard_panel_panel_ids,
	.ops		= &deckard_panel_panel_ops,
	.of_to_plat	= deckard_panel_panel_of_to_plat,
	.probe		= deckard_panel_panel_probe,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct deckard_panel_priv),
};
