// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright (C) 2025 Linaro, ltd
 * Author: Rui Miguel Silva <rui.silva@linaro.org>
 */


#include <clk.h>
#include <div64.h>
#include <dm.h>
#include <errno.h>
#include <log.h>
#include <nvmem.h>
#include <pwm.h>
#include <asm/io.h>
#include <dm/lists.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/time.h>
#include <power/pmic.h>

#define SDAM_REG_PBS_SEQ_EN		0x42
#define SDAM_PBS_TRIG_SET		0xe5
#define SDAM_PBS_TRIG_CLR		0xe6

#define PBS_SW_TRIG_BIT		BIT(0)

#define LPG_PWM_SRC_LUT		0
#define LPG_PWM_SRC_VALUE	1

#define REG_SIZE_PER_LPG	0x100

/* LPG module registers */
#define REG_LPG_PERPH_SUBTYPE		0x05
#define  LPG_SUBTYPE_LPG	0x2
#define  LPG_SUBTYPE_PWM	0xb
#define  LPG_SUBTYPE_HI_RES_PWM	0xc
#define  LPG_SUBTYPE_LPG_LITE	0x11
#define REG_LPG_PATTERN_CONFIG		0x40
#define REG_LPG_PWM_SIZE_CLK		0x41
#define  PWM_CLK_SELECT_MASK	GENMASK(1, 0)
#define  PWM_SIZE_SELECT_MASK	BIT(2)
#define  PWM_SIZE_HI_RES_MASK	GENMASK(6, 4)
#define REG_LPG_PWM_FREQ_PREDIV_CLK	0x42
#define  PWM_FREQ_PRE_DIV_MASK	GENMASK(6, 5)
#define  PWM_FREQ_EXP_MASK	GENMASK(2, 0)
#define REG_LPG_PWM_TYPE_CONFIG		0x43
#define REG_LPG_PWM_VALUE_LSB		0x44
#define REG_LPG_PWM_VALUE_MSB		0x45
#define REG_LPG_ENABLE_CONTROL		0x46
#define REG_LPG_PWM_SYNC		0x47
#define REG_LPG_RAMP_STEP_DURATION_LSB	0x50
#define REG_LPG_RAMP_STEP_DURATION_MSB	0x51
#define REG_LPG_PAUSE_HI_MULTIPLIER	0x52
#define REG_LPG_PAUSE_LO_MULTIPLIER	0x54
#define REG_LPG_HI_INDEX		0x56
#define REG_LPG_LO_INDEX		0x57

/* LPG per channel config settings for PPG */
#define SDAM_LUT_EN_OFFSET			0x0
#define SDAM_PATTERN_CONFIG_OFFSET		0x1
#define SDAM_END_INDEX_OFFSET			0x3
#define SDAM_START_INDEX_OFFSET			0x4
#define SDAM_PBS_SCRATCH_LUT_COUNTER_OFFSET	0x6
#define SDAM_PAUSE_HI_MULTIPLIER_OFFSET		0x8
#define SDAM_PAUSE_LO_MULTIPLIER_OFFSET		0x9

/* SDAM_REG_LUT_EN */
#define SDAM_LUT_EN_BIT			BIT(0)
#define LPG_PWM_VALUE_SYNC		BIT(0)

/* REG_LPG_PWM_SIZE_CLK */
#define LPG_PWM_SIZE_LPG_MASK		BIT(4)
#define LPG_PWM_SIZE_PWM_MASK		BIT(2)
#define LPG_PWM_SIZE_PWM_HI_RES_MASK	GENMASK(6, 4)
#define LPG_PWM_SIZE_LPG_SHIFT		4
#define LPG_PWM_SIZE_PWM_SHIFT		2
#define LPG_PWM_SIZE_PWM_HI_RES_SHIFT	4
#define LPG_PWM_CLK_FREQ_SEL_MASK	GENMASK(1, 0)
#define LPG_PWM_HI_RES_CLK_FREQ_SEL_MASK	GENMASK(2, 0)

/* REG_LPG_PWM_TYPE_CONFIG */
#define LPG_PWM_EN_GLITCH_REMOVAL_MASK	BIT(5)

/* REG_LPG_ENABLE_CONTROL */
#define LPG_ENABLE_CONTROL_OUTPUT		BIT(7)
#define LPG_ENABLE_CONTROL_BUFFER_TRISTATE	BIT(5)
#define LPG_ENABLE_CONTROL_SRC_PWM		BIT(2)
#define LPG_ENABLE_CONTROL_RAMP_GEN		BIT(1)

/* REG_LPG_PWM_VALUE_MSB */
#define LPG_PWM_VALUE_MSB_MASK		GENMASK(15, 8)
#define LPG_PWM_VALUE_LSB_MASK		GENMASK(7, 0)

#define LPG_PATTERN_CONFIG_LO_TO_HI	BIT(4)
#define LPG_PATTERN_CONFIG_REPEAT	BIT(3)
#define LPG_PATTERN_CONFIG_TOGGLE	BIT(2)
#define LPG_PATTERN_CONFIG_PAUSE_HI	BIT(1)
#define LPG_PATTERN_CONFIG_PAUSE_LO	BIT(0)

#define LPG_RESOLUTION_9BIT	BIT(9)
#define LPG_RESOLUTION_15BIT	BIT(15)
#define PPG_MAX_LED_BRIGHTNESS	255

#define LPG_MAX_M		7
#define LPG_MAX_PREDIV		6

#define DEFAULT_TICK_DURATION_US	7800
#define RAMP_STEP_DURATION(x)		(((x) * 1000 / \
					DEFAULT_TICK_DURATION_US) & 0xff)

#define PWM_OUTPUT_FIXED	BIT(0)
#define PWM_OUTPUT_MODULATED	BIT(1)

#define QCOM_LPG_CHANNELS_MAX	4
#define QCOM_LPG_PATTERN_MAX	26

#define SDAM_START_BASE			0x40
#define SDAM_REG_RAMP_STEP_DURATION	0x47

struct qcom_lpg_ramp_config {
	u32 step_ms;
	u32 pause_hi_count;
	u32 pause_lo_count;
	u32 high_index;
	u32 low_index;
};
struct qcom_lpg_pwm_channel {
	u32 id;
	u32 reg_base;
	u32 lut_mask;
	u8 output_type;
	u8 enabled;
	u8 subtype;

	u8 clk_sel;
	u8 pre_div_sel;
	u8 pre_div_exp;
	u8 pwm_resolution_sel;
	u16 pwm_value;
	u32 pwm_size;
	u64 pwm_clk;
	u64 period;

	u32 period_ns_current;
	u32 duty_ns_current;

	u32 lpg_sdam_base;
	struct qcom_lpg_ramp_config ramp;
};

struct qcom_lpg_pwm {
	struct udevice *dev;
	struct udevice *pmic;
	struct nvmem_device channel_nvmem;
	struct nvmem_device lut_nvmem;

	u32 base;
	u32 channel_count;
	u32 pbs_en_bitmap;
	u32 lut_sdam_base;
	bool lut_written;
	u32 pattern_length;
	u32 pattern[QCOM_LPG_PATTERN_MAX];
	struct qcom_lpg_pwm_channel channels[QCOM_LPG_CHANNELS_MAX];
};

static void qcom_lpg_dump_registers(struct qcom_lpg_pwm *lpg,
				    struct qcom_lpg_pwm_channel *channel)
{
	int i;

	printf("LPG: registers\n");
	for (i = 0x40; i < 0x58; i++)
		printf("   LPG: register(0x%x) = 0x%x\n", i,
	 		pmic_reg_read(lpg->pmic, channel->reg_base + i));

	printf("LPG: SDAM \n");
	for (i = 0; i < 0x9; i++)
		printf("   LPG: SDAM: base [0x%x] (0x%i) = 0x%x\n", channel->lpg_sdam_base, i,
	 		pmic_reg_read(lpg->channel_nvmem.nvmem, channel->lpg_sdam_base + lpg->channel_nvmem.addr + i));

	printf("lPG: DUMP END\n");
}

static struct qcom_lpg_pwm_channel *channel_from_index(struct qcom_lpg_pwm *lpg,
						        u32 channel_index)
{
	if (channel_index > lpg->channel_count)
		return NULL;

	return &lpg->channels[channel_index];
}

static int qcom_lpg_pwm_reg_masked_write(struct qcom_lpg_pwm *lpg,
					 struct qcom_lpg_pwm_channel *channel,
					 u16 addr, u8 mask, u8 val)
{
	u8 val_cur = 0;
	int ret;

	addr += channel->reg_base;

	ret = pmic_read(lpg->pmic, addr, &val_cur, 1);
	if (ret < 0)
		return ret;

	val_cur &= ~mask;

	val_cur |= (val & mask);

	return pmic_write(lpg->pmic, addr, &val_cur, 1);
}

static int qcom_lpg_pwm_reg_read(struct qcom_lpg_pwm *lpg,
				 struct qcom_lpg_pwm_channel *channel,
				 u16 addr, u8 *val)
{
	addr += channel->reg_base;

	return pmic_read(lpg->pmic, addr, val, 1);
}

static int qcom_lpg_pwm_reg_write(struct qcom_lpg_pwm *lpg,
				  struct qcom_lpg_pwm_channel *channel,
				  u16 addr, u8 val)
{
	addr += channel->reg_base;

	return pmic_write(lpg->pmic, addr, &val, 1);
}

static int qcom_lpg_pwm_chan_write(struct qcom_lpg_pwm *lpg,
				   struct qcom_lpg_pwm_channel *channel,
				   u16 addr, u8 val)
{
	if (!channel)
		return -EINVAL;

	addr += channel->lpg_sdam_base;

	return nvmem_device_write(&lpg->channel_nvmem, addr, &val, 1);
}

static int qcom_lpg_pwm_write(struct qcom_lpg_pwm *lpg,
			      u16 addr, u8 val)
{
	return nvmem_device_write(&lpg->channel_nvmem, addr, &val, 1);
}

static int qcom_lpg_pwm_lut_write(struct qcom_lpg_pwm *lpg, u16 addr, u8 val)
{
	addr += lpg->lut_sdam_base;

	return nvmem_device_write(&lpg->lut_nvmem, addr, &val, 1);
}

static const unsigned int lpg_clk_rates[] = {0, 1024, 32768, 19200000};
static const unsigned int lpg_clk_rates_hi_res[] = {0, 1024, 32768, 19200000, 76800000};
static const unsigned int lpg_pre_divs[] = {1, 3, 5, 6};
static const unsigned int lpg_pwm_resolution[] = {6, 9};
static const unsigned int lpg_pwm_resolution_hi_res[] = {8, 9, 10, 11, 12, 13, 14, 15};

static int qcom_lpg_calc_freq(struct qcom_lpg_pwm_channel *channel, uint64_t period)
{
	unsigned int i, pwm_resolution_count, best_pwm_resolution_sel = 0;
	const unsigned int *clk_rate_arr, *pwm_resolution_arr;
	unsigned int clk_sel, clk_len, best_clk = 0;
	unsigned int div, best_div = 0;
	unsigned int m, best_m = 0;
	unsigned int resolution;
	unsigned int error;
	unsigned int best_err = UINT_MAX;
	u64 max_period, min_period;
	u64 best_period = 0;
	u64 max_res;

	/*
	 * The PWM period is determined by:
	 *
	 *          resolution * pre_div * 2^M
	 * period = --------------------------
	 *                   refclk
	 *
	 * Resolution = 2^{6 or 9} bits for PWM or
	 *              2^{8, 9, 10, 11, 12, 13, 14, 15} bits for high resolution PWM
	 * pre_div = {1, 3, 5, 6} and
	 * M = [0..7].
	 *
	 * This allows for periods between 3uS and 384s for PWM channels and periods between
	 * 3uS and 24576s for high resolution PWMs.
	 * The PWM framework wants a period of equal or lower length than requested,
	 * reject anything below minimum period.
	 */
	if (channel->subtype == LPG_SUBTYPE_HI_RES_PWM) {
		clk_rate_arr = lpg_clk_rates_hi_res;
		clk_len = ARRAY_SIZE(lpg_clk_rates_hi_res);
		pwm_resolution_arr = lpg_pwm_resolution_hi_res;
		pwm_resolution_count = ARRAY_SIZE(lpg_pwm_resolution_hi_res);
		max_res = LPG_RESOLUTION_15BIT;
	} else {
		clk_rate_arr = lpg_clk_rates;
		clk_len = ARRAY_SIZE(lpg_clk_rates);
		pwm_resolution_arr = lpg_pwm_resolution;
		pwm_resolution_count = ARRAY_SIZE(lpg_pwm_resolution);
		max_res = LPG_RESOLUTION_9BIT;
	}

	min_period = div64_u64((u64)NSEC_PER_SEC * ((1 << pwm_resolution_arr[0]) - 1),
			       clk_rate_arr[clk_len - 1]);
	if (period <= min_period)
		return -EINVAL;

	/* Limit period to largest possible value, to avoid overflows */
	max_period = div64_u64((u64)NSEC_PER_SEC * max_res * LPG_MAX_PREDIV * (1 << LPG_MAX_M),
			       1024);
	if (period > max_period)
		period = max_period;

	/*
	 * Search for the pre_div, refclk, resolution and M by solving the rewritten formula
	 * for each refclk, resolution and pre_div value:
	 *
	 *                     period * refclk
	 * M = log2 -------------------------------------
	 *           NSEC_PER_SEC * pre_div * resolution
	 */

	for (i = 0; i < pwm_resolution_count; i++) {
		resolution = (1 << pwm_resolution_arr[i]) - 1;
		for (clk_sel = 1; clk_sel < clk_len; clk_sel++) {
			u64 numerator = period * clk_rate_arr[clk_sel];

			for (div = 0; div < ARRAY_SIZE(lpg_pre_divs); div++) {
				u64 denominator = (u64)NSEC_PER_SEC * lpg_pre_divs[div] *
						  resolution;
				u64 actual;
				u64 ratio;

				if (numerator < denominator)
					continue;

				ratio = div64_u64(numerator, denominator);
				m = ilog2(ratio);
				if (m > LPG_MAX_M)
					m = LPG_MAX_M;

				actual = DIV_ROUND_UP_ULL(denominator * (1 << m),
							  clk_rate_arr[clk_sel]);
				error = period - actual;
				if (error < best_err) {
					best_err = error;
					best_div = div;
					best_m = m;
					best_clk = clk_sel;
					best_period = actual;
					best_pwm_resolution_sel = i;
				}
			}
		}
	}

	channel->clk_sel = best_clk;
	channel->pre_div_sel = best_div;
	channel->pre_div_exp = best_m;
	channel->period = best_period;
	channel->pwm_resolution_sel = best_pwm_resolution_sel;

	return 0;
}

static void qcom_lpg_calc_duty(struct qcom_lpg_pwm_channel *channel, uint64_t duty)
{
	unsigned int max;
	unsigned int val;
	unsigned int clk_rate;

	if (channel->subtype == LPG_SUBTYPE_HI_RES_PWM) {
		max = BIT(lpg_pwm_resolution_hi_res[channel->pwm_resolution_sel]) - 1;
		clk_rate = lpg_clk_rates_hi_res[channel->clk_sel];
	} else {
		max = BIT(lpg_pwm_resolution[channel->pwm_resolution_sel]) - 1;
		clk_rate = lpg_clk_rates[channel->clk_sel];
	}

	val = div64_u64(duty * clk_rate,
			(u64)NSEC_PER_SEC * lpg_pre_divs[channel->pre_div_sel] *
			(1 << channel->pre_div_exp));

	channel->pwm_value = min(val, max);
}

static void qcom_lpg_sdam_apply_lut_control(struct qcom_lpg_pwm *lpg,
					    struct qcom_lpg_pwm_channel *channel)
{
	struct qcom_lpg_ramp_config *ramp = &channel->ramp;
	unsigned int lo_idx = ramp->low_index;
	unsigned int hi_idx = ramp->high_index;
	u8 val = 0, conf = 0, lut_offset = 0;

	/* only oneshot support at this time */
	conf |= LPG_PATTERN_CONFIG_REPEAT;

	lut_offset = lpg->lut_sdam_base - SDAM_START_BASE;
	hi_idx += lut_offset;
	lo_idx += lut_offset;

	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_PBS_SCRATCH_LUT_COUNTER_OFFSET, val);
	val = RAMP_STEP_DURATION(ramp->step_ms);
	qcom_lpg_pwm_write(lpg, SDAM_REG_RAMP_STEP_DURATION, val);
	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_END_INDEX_OFFSET, hi_idx);
	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_START_INDEX_OFFSET, lo_idx);
	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_PATTERN_CONFIG_OFFSET, conf);

	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_PAUSE_HI_MULTIPLIER_OFFSET, 0);
	qcom_lpg_pwm_chan_write(lpg, channel, SDAM_PAUSE_LO_MULTIPLIER_OFFSET, 0);
}

static void qcom_lpg_pwm_lut_pattern_set(struct qcom_lpg_pwm *lpg,
					 struct qcom_lpg_pwm_channel *channel)
{
	struct qcom_lpg_ramp_config *ramp;
	u8 val;
	int i;

	ramp = &channel->ramp;

	if (channel->output_type != PWM_OUTPUT_MODULATED)
		return;

	/* only need to writte it once */
	if (!lpg->lut_written) {
		for (i = 0; i < lpg->pattern_length; i++) {
			val = lpg->pattern[i] * 255 / 100;

			qcom_lpg_pwm_lut_write(lpg, ramp->low_index + i, val);
		}

		lpg->lut_written = true;
	}

	qcom_lpg_sdam_apply_lut_control(lpg, channel);
}

static int qcom_lpg_pwm_pbs_set(struct qcom_lpg_pwm *lpg,
				struct qcom_lpg_pwm_channel *channel)
{
	int ret;

	if (!lpg->pbs_en_bitmap) {
		ret = qcom_lpg_pwm_write(lpg, SDAM_REG_PBS_SEQ_EN,
			 		 PBS_SW_TRIG_BIT);
		if (ret < 0)
			return ret;

		ret = qcom_lpg_pwm_write(lpg, SDAM_PBS_TRIG_SET,
					 PBS_SW_TRIG_BIT);
		if (ret < 0)
			return ret;
	}
	lpg->pbs_en_bitmap |= channel->lut_mask;

	return 0;
}

static int qcom_lpg_pwm_pbs_clear(struct qcom_lpg_pwm *lpg,
				  struct qcom_lpg_pwm_channel *channel)
{
	int ret;

	lpg->pbs_en_bitmap &= (~channel->lut_mask);
	if (!lpg->pbs_en_bitmap) {
		ret = qcom_lpg_pwm_write(lpg, SDAM_REG_PBS_SEQ_EN, 0);
		if (ret < 0)
			return ret;

		ret = qcom_lpg_pwm_write(lpg, SDAM_PBS_TRIG_CLR,
					 PBS_SW_TRIG_BIT);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int qcom_lpg_pwm_config(struct qcom_lpg_pwm *lpg,
			       struct qcom_lpg_pwm_channel *channel);

static int qcom_lpg_pwm_src_control(struct qcom_lpg_pwm *lpg,
				    struct qcom_lpg_pwm_channel *channel,
				    bool enable)
{
	bool pbs_set = false;
	u8 val = 0;
	int ctrl;
	int ret;

	ctrl = 0;

	if (enable)
		ctrl |= LPG_ENABLE_CONTROL_OUTPUT;

	if (channel->output_type == PWM_OUTPUT_FIXED)
		ctrl |= LPG_ENABLE_CONTROL_SRC_PWM;

	ret = qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_ENABLE_CONTROL,
				      ctrl);
	if (ret < 0) {
		printf("qcom_lpg: failed to set source control\n");
		return ret;
	}

	if (channel->output_type == PWM_OUTPUT_MODULATED && enable) {
		val = SDAM_LUT_EN_BIT;
		pbs_set = true;
	}

	ret = qcom_lpg_pwm_chan_write(lpg, channel,
				      SDAM_LUT_EN_OFFSET, val);
	if (ret < 0) {
		printf("qcom_lpg: failed to set trigger\n");
		return ret;
	}

	if (pbs_set)
		ret = qcom_lpg_pwm_pbs_set(lpg, channel);
	else
		ret = qcom_lpg_pwm_pbs_clear(lpg, channel);
	if (ret < 0) {
		printf("qcom_lpg: failed to set/clear pbs\n");
		return ret;
	}

	if (channel->output_type == PWM_OUTPUT_MODULATED)
		qcom_lpg_pwm_config(lpg, channel);

	return 0;
}

static int qcom_lpg_pwm_src_enable(struct qcom_lpg_pwm *lpg,
				   struct qcom_lpg_pwm_channel *channel)
{
	return qcom_lpg_pwm_src_control(lpg, channel, true);
}

static int qcom_lpg_pwm_src_disable(struct qcom_lpg_pwm *lpg,
				    struct qcom_lpg_pwm_channel *channel)
{
	return qcom_lpg_pwm_src_control(lpg, channel, false);
}

static int qcom_lpg_pwm_channel_src_control_init(struct qcom_lpg_pwm *lpg)
{
	struct qcom_lpg_pwm_channel *channel;
	int ret;
	int i;

	for (i = 0; i < lpg->channel_count; i++) {
		channel = &lpg->channels[i];

		channel->output_type = PWM_OUTPUT_MODULATED;

		ret = qcom_lpg_pwm_src_disable(lpg, channel);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int qcom_lpg_pwm_config(struct qcom_lpg_pwm *lpg,
			       struct qcom_lpg_pwm_channel *channel)
{
	u8 val;

	val = channel->clk_sel;

	/* specify resolution, based on the subtype of the channel */
	switch (channel->subtype) {
	case LPG_SUBTYPE_LPG:
		val |= GENMASK(5, 4);
		break;
	case LPG_SUBTYPE_PWM:
		val |= FIELD_PREP(PWM_SIZE_SELECT_MASK,
				  channel->pwm_resolution_sel);
		break;
	case LPG_SUBTYPE_HI_RES_PWM:
		val |= FIELD_PREP(PWM_SIZE_HI_RES_MASK,
				  channel->pwm_resolution_sel);
		break;
	case LPG_SUBTYPE_LPG_LITE:
	default:
		val |= BIT(4);
		break;
	}

	qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_PWM_SIZE_CLK, val);

	val = FIELD_PREP(PWM_FREQ_PRE_DIV_MASK, channel->pre_div_sel) |
		FIELD_PREP(PWM_FREQ_EXP_MASK, channel->pre_div_exp);

	qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_PWM_FREQ_PREDIV_CLK,
				val);

	if (channel->output_type == PWM_OUTPUT_MODULATED)
		return 0;

	val = channel->pwm_value >> 8;
	qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_PWM_VALUE_MSB, val);

	val = channel->pwm_value & 0xff;
	qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_PWM_VALUE_LSB, val);

	return 0;
}

int qcom_lpg_pwm_output_type_set(struct qcom_lpg_pwm *lpg,
				 struct qcom_lpg_pwm_channel *channel,
				 u32 output_type)
{
	int ret;

	if (channel->enabled) {
		ret = qcom_lpg_pwm_src_disable(lpg, channel);
		if (ret < 0)
			return ret;
	}

	channel->output_type = output_type;

	qcom_lpg_pwm_lut_pattern_set(lpg, channel);

	if (channel->enabled) {
		ret = qcom_lpg_pwm_config(lpg, channel);
		ret = qcom_lpg_pwm_src_enable(lpg, channel);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void qcom_lpg_pwm_glitch_enable(struct qcom_lpg_pwm *lpg,
				       struct qcom_lpg_pwm_channel *channel)
{
	qcom_lpg_pwm_reg_masked_write(lpg, channel, REG_LPG_PWM_TYPE_CONFIG,
					LPG_PWM_EN_GLITCH_REMOVAL_MASK, 0);
}

static void qcom_lpg_pwm_glitch_disable(struct qcom_lpg_pwm *lpg,
				       struct qcom_lpg_pwm_channel *channel)
{
	qcom_lpg_pwm_reg_masked_write(lpg, channel, REG_LPG_PWM_TYPE_CONFIG,
					LPG_PWM_EN_GLITCH_REMOVAL_MASK,
					LPG_PWM_EN_GLITCH_REMOVAL_MASK);
}

static void qcom_lpg_enable(struct qcom_lpg_pwm *lpg,
			   struct qcom_lpg_pwm_channel *channel)
{
	if (channel->output_type == PWM_OUTPUT_FIXED)
		qcom_lpg_pwm_reg_write(lpg, channel, REG_LPG_PWM_SYNC,
			 		LPG_PWM_VALUE_SYNC);

	qcom_lpg_pwm_src_enable(lpg, channel);

	qcom_lpg_pwm_glitch_disable(lpg, channel);

	channel->enabled = true;
}

static void qcom_lpg_disable(struct qcom_lpg_pwm *lpg,
			     struct qcom_lpg_pwm_channel *channel)
{
	qcom_lpg_pwm_src_disable(lpg, channel);

	qcom_lpg_pwm_glitch_enable(lpg, channel);

	channel->enabled = false;
}

static int qcom_lpg_config(struct udevice *dev, uint channel_index,
			   uint period_ns, uint duty_ns)
{
	struct qcom_lpg_pwm *lpg = dev_get_priv(dev);
	struct qcom_lpg_pwm_channel *channel;
	int ret;

	channel = channel_from_index(lpg, channel_index);
	if (!channel) {
		printf("qcom_lpg: did not found channel with index: %d\n",
			channel_index);
		return -EINVAL;
	}

	if (period_ns)
		qcom_lpg_pwm_output_type_set(lpg, channel, PWM_OUTPUT_MODULATED);
	else
		qcom_lpg_pwm_output_type_set(lpg, channel, PWM_OUTPUT_FIXED);

	if (!period_ns)
		period_ns = USEC_PER_SEC;

	if (channel->period_ns_current != period_ns) {
		ret = qcom_lpg_calc_freq(channel, period_ns);
		if (ret < 0)
			return ret;

		qcom_lpg_pwm_lut_pattern_set(lpg, channel);
	}

	if (channel->period_ns_current != period_ns ||
	    channel->duty_ns_current != duty_ns)
		qcom_lpg_calc_duty(channel, duty_ns);


	qcom_lpg_pwm_config(lpg, channel);

	channel->period_ns_current = period_ns;
	channel->duty_ns_current = duty_ns;

	return 0;
}

static int qcom_lpg_set_enable(struct udevice *dev, uint channel_index, bool enable)
{
	struct qcom_lpg_pwm *lpg = dev_get_priv(dev);
	struct qcom_lpg_pwm_channel *channel;

	channel = channel_from_index(lpg, channel_index);
	if (!channel)
		return -EINVAL;

	if (enable)
		qcom_lpg_enable(lpg, channel);
	else
		qcom_lpg_disable(lpg, channel);


	if (channel->output_type == PWM_OUTPUT_MODULATED)
		qcom_lpg_sdam_apply_lut_control(lpg, channel);

	return 0;
}

static int qcom_lpg_pwm_sdam_init(struct qcom_lpg_pwm *lpg)
{
	struct qcom_lpg_pwm_channel *channel;
	int ret;
	int i;

	for (i = 0; i < lpg->channel_count; i++) {
		channel = &lpg->channels[i];
		/* disable trigger */
		ret = qcom_lpg_pwm_chan_write(lpg, channel,
					      SDAM_LUT_EN_OFFSET, 0);
		if (ret < 0)
			return ret;

		ret = qcom_lpg_pwm_chan_write(lpg, channel,
				SDAM_PBS_SCRATCH_LUT_COUNTER_OFFSET,
				0);
		if (ret < 0)
			return ret;

		/* clear pbs trigger */
		ret = qcom_lpg_pwm_pbs_clear(lpg, channel);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int qcom_lpg_pwm_channel_init(struct qcom_lpg_pwm *lpg)
{
	struct qcom_lpg_pwm_channel *channel;
	struct qcom_lpg_ramp_config *ramp;
	ofnode node;
	int i = 0;
	int ret;

	dev_for_each_subnode(node, lpg->dev) {
		if (!ofnode_is_enabled(node))
			continue;

		channel = &lpg->channels[i];
		ramp = &channel->ramp;

		ret = ofnode_read_u32(node, "qcom,lpg-chan-id", &channel->id);
		if (ret < 0)
			return -EINVAL;

		ret = ofnode_read_u32(node, "qcom,lpg-sdam-base",
					&channel->lpg_sdam_base);
		if (ret < 0)
			return -EINVAL;

		channel->reg_base = dev_read_addr(lpg->dev) +
					((channel->id - 1) * REG_SIZE_PER_LPG);

		channel->lut_mask = BIT(i);

		ret = ofnode_read_u32(node, "qcom,ramp-step-ms",
				      &ramp->step_ms);
		ret |= ofnode_read_u32(node, "qcom,ramp-low-index",
					&ramp->low_index);
		ret |= ofnode_read_u32(node, "qcom,ramp-high-index",
					&ramp->high_index);
		if (ret)
			return -EINVAL;

		ret = qcom_lpg_pwm_reg_read(lpg, channel, REG_LPG_PERPH_SUBTYPE,
					     &channel->subtype);
		if (ret)
			return -EINVAL;
		i++;
	}

	return 0;
}

static int qcom_lpg_pwm_parse_dt(struct qcom_lpg_pwm *lpg)
{
	struct udevice *dev = lpg->dev;
	int pattern_length;
	ofnode node;
	int ret;

	node = dev_ofnode(dev);

	ret = ofnode_read_u32(node, "qcom,num-lpg-channels", &lpg->channel_count);
	if (ret < 0) {
		printf("qcom_lpg: did not found lpg number of channels\n");
		return -EINVAL;
	}

	if (lpg->channel_count > QCOM_LPG_CHANNELS_MAX) {
		printf("qcom_lpg: %d only support a max of %d channels\n",
			lpg->channel_count, QCOM_LPG_CHANNELS_MAX);
		return -EINVAL;
	}

	ret = nvmem_get_by_name(dev, "lpg_chan_sdam", &lpg->channel_nvmem);
	if (ret < 0) {
		printf("qcom_lpg: did not found lpg nvmem chan sdam\n");
		return -EINVAL;
	}

	ret = nvmem_get_by_name(dev, "lut_sdam", &lpg->lut_nvmem);
	if (ret < 0) {
		printf("qcom_lpg: did not found lpg nvmem lut sdam\n");
		return -EINVAL;
	}

	ret = ofnode_read_u32(node, "qcom,lut-sdam-base", &lpg->lut_sdam_base);
	if (ret < 0) {
		printf("qcom_lpg: did not found lpg lut sdam base\n");
		return -EINVAL;
	}

	pattern_length = ofnode_read_size(node, "qcom,lut-patterns");
	pattern_length /= sizeof(fdt32_t);
	if (pattern_length < 0 || pattern_length > QCOM_LPG_PATTERN_MAX) {
		printf("qcom_lpg: patternlength %d or bigger than max: %d\n",
	 		pattern_length, QCOM_LPG_PATTERN_MAX);
		return -EINVAL;
	}
	lpg->pattern_length = pattern_length - 1;

	ret = ofnode_read_u32_array(node, "qcom,lut-patterns", lpg->pattern,
				    pattern_length);
	if (ret < 0) {
		printf("qcom_lpg: did not found pattern\n");
		return -EINVAL;
	}

	return 0;
}

static int qcom_lpg_pwm_probe(struct udevice *dev)
{
	struct qcom_lpg_pwm *lpg = dev_get_priv(dev);
	int ret;

	lpg->dev = dev;
	lpg->pmic = dev->parent;

	ret = dev_read_addr(dev->parent);

	ret = qcom_lpg_pwm_parse_dt(lpg);
	if (ret < 0) {
		printf("qcom_lpg: parse dt failed\n");
		return -EINVAL;
	}

	ret = qcom_lpg_pwm_channel_init(lpg);
	if (ret < 0) {
		printf("qcom_lpg: failed to init channels\n");
		return -EINVAL;
	}

	ret = qcom_lpg_pwm_sdam_init(lpg);
	if (ret < 0) {
		printf("qcom_lpg: failed to init sdam\n");
		return -EINVAL;
	}

	ret = qcom_lpg_pwm_channel_src_control_init(lpg);
	if (ret < 0) {
		printf("qcom_lpg: failed to init control\n");
		return -EINVAL;
	}

	return 0;
}

static const struct pwm_ops qcom_lpg_pwm_ops = {
	.set_config = qcom_lpg_config,
	.set_enable = qcom_lpg_set_enable,
};

static const struct udevice_id qcom_lpg_pwm_ids[] = {
	{ .compatible = "qcom,pwm-lpg" },
	{ },
};

U_BOOT_DRIVER(qpmp_lpg_pwm) = {
	.name		= "qcom_lpg_pwm",
	.id		= UCLASS_PWM,
	.of_match	= qcom_lpg_pwm_ids,
	.ops		= &qcom_lpg_pwm_ops,
	.probe		= qcom_lpg_pwm_probe,
	.priv_auto	= sizeof(struct qcom_lpg_pwm),
};
