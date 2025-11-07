// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro Ltd.
 */

#include <asm/gpio.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <i2c.h>
#include <linux/delay.h>
#include <power/regulator.h>

#define MP3317_MODE_CTL 0
#define MP3317_EN_CTL 1
#define MP3317_OVP_IMAX 0x02
#define MP3317_TPWM 0x03
#define MP3317_ILEDL_L 0x04
#define MP3317_ILEDL_H 0x05
#define MP3317_ILEDR_L 0x06
#define MP3317_ILEDR_H 0x07
#define MP3317_VOUT 0x08
#define MP3317_HR_TH 0x09
#define MP3317_HR_STEP 0x0A
#define MP3317_FAULT1 0x0B
#define MP3317_FAULT2 0x0C
#define MP3317_REG08_SET_VOUT 0x08

#define EN_MASK BIT(7)
#define CURRENT_STATE_MASK BIT(6)
#define BLMOD_MASK BIT(7)
#define CPHYST_MASK GENMASK(7,6)
#define IMAX_MASK GENMASK(2,0)
#define HR_L_MASK GENMASK(3,0)
#define TPWM_MASK GENMASK(3,0)
#define CBC_LIMIT_MASK GENMASK(5,3)
#define OVP_MASK GENMASK(5,3)
#define FAULT1_MASK GENMASK(6,0)
#define FAULT2_MASK GENMASK(2,0)
#define FSW_MASK GENMASK(3,2)
#define INCL_MASK GENMASK(2,0)

#define MAX_BRIGHTNESS 4095

// MODE_CTL (0x00) - b1000 1001
//    BLMOD - (b1)   - Adaptive control enabled
//    PWMLR - (b0)   - L for CH1-3, R for CH4-6
//    CBCL  - (b011) - 0.9A cycle-by-cycle limit
//    INCL  - (b001) - 0.8A input current limit
// #define CONFIG_MODE_CL_DEFAULT          0x89
#define CONFIG_MODE_CTL_DEFAULT          0xF7

// EN_CTRL (0x01)
//    EN - Chip Enable
//    ISET_EN - Sets ILED current to internal
//    CHEN - Enable channels 1,2,4,5
#define CONFIG_EN_CTRL_DEFAULT          0xBF

// OVP_MAX (0x02) - b0100 1111
//   DMSL    - (b0)    - Constant voltage mode
//   SBY_PWM - (b0)    - Disable standby
//   OVP     - (b001)  - 21V
//   IMAX    - (b111)  - 160mA max current
#define CONFIG_OVP_MAX_DEFAULT          0x9F
// #define CONFIG_OVP_MAX_DEFAULT          0x57 // OVP = 010 (25V)
// #define CONFIG_OVP_MAX_DEFAULT          0x5F // OVP = 011 (30V)
// #define CONFIG_OVP_MAX_DEFAULT          0x7F // OVP = 111 (34V)

// TPWM (0x03) - bxxxx 1010
//   TPWM    - (b1010) - 5ms max PWM on time
#define CONFIG_TPWM_DEFAULT             0x00

// ILEDL_H/L (0x05/0x04) - bxxxx 0100 1111 1111
//   ILEDL   - b0100 1111 1111 - Left current at 100mA of 160mA max
#define CONFIG_ILEDL_H_DEFAULT          0x09
#define CONFIG_ILEDL_L_DEFAULT          0xFF

// ILEDR_H/L (0x07/0x06) - bxxxx 0100 1111 1111
//   ILEDR   - b0100 1111 1111 - Right current at 100mA of 160mA max
#define CONFIG_ILEDR_H_DEFAULT          0x09
#define CONFIG_ILEDR_L_DEFAULT          0xFF

// VOUT (0x08) - b0001 1000
//   VOUT    - (b0001 1000) - VOUT = 9.544V
#define CONFIG_VOUT_DEFAULT             0x68
// Can set VOUT with this voltage define. Comment this to use CONFIG_VOUT_DEFAULT directly
#define CONFIG_VOUT_MV                  18000

// HR_TH (0x09) - b0100 1001
//   CPYHyst - (b00)   - 750mV headroom
//   EN_INUV - (b1)    - UVLO enabled
//   TCOMP   - (b0)    - 20msec Short-circuit comparator
//   FSW     - (b10)   - 1.25MHz switch frequency
//   LEDS    - (b01)   - 5V short-circuit
#define CONFIG_HR_TH_DEFAULT            0xfa

// STEP_HR (0x0A) - b0100 0001
//   Step_UP - (b01)   - 210mV Step-up size
//   Step_DWN- (b00)   - 105mV Step-down size
//   HeadR_L - (b0001) - 375mV comparison threshold
#define CONFIG_STEP_HR_DEFAULT          0x41

struct mp3317_regulator_pdata {
	struct udevice *i2c;
	struct gpio_desc en;
	bool enabled;
};

/* Configure and enable the MP3317 with the desired Deckard settings */
static int deckard_config_mp3317(struct udevice *dev)
{
	struct mp3317_regulator_pdata *pdata = dev_get_plat(dev);
	int ret;
	u8 regval;

	// Disable MP3317 before setting config, just disable all EN bits for config
	regval = 0;
 	ret = dm_i2c_write(pdata->i2c, MP3317_EN_CTL, &regval, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_EN_CTL disable error\n");
		return ret;
	}

	// Clear BLMOD_MASK before updating VOUT
	ret = dm_i2c_read(pdata->i2c, MP3317_MODE_CTL, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read MP3317_MODE_CTL failed: %d\n", ret);
		return ret;
	}

	regval &= ~(BLMOD_MASK);

	ret = dm_i2c_write(pdata->i2c, MP3317_MODE_CTL, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c write MP3317_MODE_CTL failed: %d\n", ret);
		return ret;
	}

	// VOUT
	u8 vout = CONFIG_VOUT_DEFAULT;

/*
	TODO why isnt this working
	if(vout < 7000){
		vout = 0;
	}else if(vout > 34030){
		vout = 0xFF;
	}else{
		vout = (vout - 7000 + 106 / 2) / 106;
	}
*/

	// Apply VOUT
	ret = dm_i2c_write(pdata->i2c, MP3317_VOUT, &vout, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_VOUT write error\n");
		return ret;
	}

	// OVP_MAX
	u8 ovp_max = CONFIG_OVP_MAX_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_OVP_IMAX, &ovp_max, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_OVP_IMAX update error\n");
		return ret;
	}

	// TPWM
	u8 tpwm = CONFIG_TPWM_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_TPWM, &tpwm, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_TPWM update error\n");
		return ret;
	}

	// ILEDL_L - Write LSB (L) before MSB (H)
	u8 iledl = CONFIG_ILEDL_L_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_ILEDL_L, &iledl, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_ILEDL_L update error\n");
		return ret;
	}

	// ILEDL_H
	iledl = CONFIG_ILEDL_H_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_ILEDL_H, &iledl, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_ILEDL_H update error\n");
		return ret;
	}

	// ILEDL_L - Write LSB (L) before MSB (H)
	u8 iledr = CONFIG_ILEDR_L_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_ILEDR_L, &iledr, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_ILEDL_L update error\n");
		return ret;
	}

	// ILEDL_H
	iledr = CONFIG_ILEDR_H_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_ILEDR_H, &iledr, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_ILEDL_H update error\n");
		return ret;
	}

	// HR_TH
	u8 hr = CONFIG_HR_TH_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_HR_TH, &hr, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_HR_TH update error\n");
		return ret;
	}

	// STEP_HR
	u8 step_hr = CONFIG_STEP_HR_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_HR_STEP, &step_hr, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_HR_STEP update error\n");
		return ret;
	}

	// Re-enable MP3317 with our desired mode control and enable fields LAST
	// MODE_CTL
	u8 mode_ctl = CONFIG_MODE_CTL_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_MODE_CTL, &mode_ctl, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_MODE_CTL update error\n");
		return ret;
	}

	// EN_CTL
	u8 en_ctl = CONFIG_EN_CTRL_DEFAULT;
	ret = dm_i2c_write(pdata->i2c, MP3317_EN_CTL, &en_ctl, 1);
	if(ret)
	{
		dev_err(dev, "MP3317_EN_CTL update error\n");
		return ret;
	}

	return 0;
}

static int mp3317_regulator_set_enable(struct udevice *dev, bool enable)
{
	struct mp3317_regulator_pdata *pdata = dev_get_plat(dev);

	if (enable) {
		if (pdata->enabled)
			return 0;

		dm_gpio_set_value(&pdata->en, 1);

		udelay(20000);

		deckard_config_mp3317(dev);

		pdata->enabled = 1;

		return 0;
	}

	if (!pdata->enabled)
		return 0;

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

	return 0;
}

static int mp3317_regulator_probe(struct udevice *dev)
{
	struct mp3317_regulator_pdata *pdata = dev_get_plat(dev);
	ofnode node = dev_ofnode(dev);
	int ret, slave_addr;
	u8 regval;

	ret = gpio_request_by_name(dev, "enable-gpios", 0, &pdata->en,
				   GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "get enable-gpio failed.\n");
		return ret;
	}

	ret = ofnode_read_u32(node, "reg", &slave_addr);
	if (ret)
		return ret;

	ret = i2c_get_chip(dev->parent, slave_addr, 1, &pdata->i2c);
	if (ret) {
		dev_err(dev, "i2c dev get failed.\n");
		return ret;
	}

	// Check i2c by reading ctl
	dm_gpio_set_value(&pdata->en, 1);
	udelay(2000);
	ret = dm_i2c_read(pdata->i2c, MP3317_MODE_CTL, &regval, 1);
	if (ret) {
		dev_err(dev, "i2c read ctl failed: %d\n", ret);
		dm_gpio_free(dev, &pdata->en);
		return ret;
	}

	dm_gpio_set_value(&pdata->en, 0);

	pdata->enabled = 0;

	return 0;
}

static const struct dm_regulator_ops mp3317_regulator_ops = {
	.set_enable  = mp3317_regulator_set_enable,
};

static const struct udevice_id mp3317_regulator_ids[] = {
	{ .compatible = "monolithicpower,mp3317" },
	{ },
};

U_BOOT_DRIVER(mp3317_regulator) = {
	.name = "mp3317_regulator",
	.id = UCLASS_REGULATOR,
	.ops = &mp3317_regulator_ops,
	.of_match = mp3317_regulator_ids,
	.plat_auto	= sizeof(struct mp3317_regulator_pdata),
	.probe = mp3317_regulator_probe,
};
