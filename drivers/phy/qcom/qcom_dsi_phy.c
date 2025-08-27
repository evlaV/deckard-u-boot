/* SPDX-License-Identifier: GPL-2.0-only */

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <div64.h>
#include <generic-phy.h>
#include <linux/kernel.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <linux/iopoll.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
//#include <lib.h>
#include <linux/math64.h>
#include <linux/time.h>
#include <phy-mipi-dphy.h>
#include <power/regulator.h>
#include <soc/qcom/rpmh.h>
#include <string.h>
#include <timer.h>

//#include "mdssreg.h"
//#include "qcom_display_resources.h"
//#include "qcom_mipi_dsi.h"
//#include "qcom_dsi_phy.h"

#include "dsi_phy.h"
#include "dsi.xml.h"
#include "dsi_phy_7nm.xml.h"

#define readl_poll_timeout_atomic readl_poll_timeout

#define S_DIV_ROUND_UP(n, d)	\
	(((n) >= 0) ? (((n) + (d) - 1) / (d)) : (((n) - (d) + 1) / (d)))

/*
 * DSI PLL 7nm - clock diagram (eg: DSI0): TODO: updated CPHY diagram
 *
 *           dsi0_pll_out_div_clk  dsi0_pll_bit_clk
 *                              |                |
 *                              |                |
 *                 +---------+  |  +----------+  |  +----+
 *  dsi0vco_clk ---| out_div |--o--| divl_3_0 |--o--| /8 |-- dsi0_phy_pll_out_byteclk
 *                 +---------+  |  +----------+  |  +----+
 *                              |                |
 *                              |                |         dsi0_pll_by_2_bit_clk
 *                              |                |          |
 *                              |                |  +----+  |  |\  dsi0_pclk_mux
 *                              |                |--| /2 |--o--| \   |
 *                              |                |  +----+     |  \  |  +---------+
 *                              |                --------------|  |--o--| div_7_4 |-- dsi0_phy_pll_out_dsiclk
 *                              |------------------------------|  /     +---------+
 *                              |          +-----+             | /
 *                              -----------| /4? |--o----------|/
 *                                         +-----+  |           |
 *                                                  |           |dsiclk_sel
 *                                                  |
 *                                                  dsi0_pll_post_out_div_clk
 */

#define VCO_REF_CLK_RATE		19200000
#define FRAC_BITS 18

/* Hardware is pre V4.1 */
#define DSI_PHY_7NM_QUIRK_PRE_V4_1	BIT(0)
/* Hardware is V4.1 */
#define DSI_PHY_7NM_QUIRK_V4_1		BIT(1)
/* Hardware is V4.2 */
#define DSI_PHY_7NM_QUIRK_V4_2		BIT(2)
/* Hardware is V4.3 */
#define DSI_PHY_7NM_QUIRK_V4_3		BIT(3)
/* Hardware is V5.2 */
#define DSI_PHY_7NM_QUIRK_V5_2		BIT(4)
/* Hardware is V7.0 */
#define DSI_PHY_7NM_QUIRK_V7_0		BIT(5)

/*
 * Global list of private DSI PLL struct pointers. We need this for bonded DSI
 * mode, where the master PLL's clk_ops needs access the slave's private data
 */
static struct dsi_pll_7nm *pll_7nm_list[DSI_MAX];

#if 1
static inline s32 linear_inter(s32 tmax, s32 tmin, s32 percent,
				s32 min_result, bool even)
{
	s32 v;

	v = (tmax - tmin) * percent;
	v = S_DIV_ROUND_UP(v, 100) + tmin;
	if (even && (v & 0x1))
		return max_t(s32, min_result, v - 1);
	else
		return max_t(s32, min_result, v);
}

int msm_dsi_cphy_timing_calc_v4(struct msm_dsi_dphy_timing *timing,
	struct msm_dsi_phy_clk_request *clk_req)
{
	const unsigned long bit_rate = clk_req->bitclk_rate;
	const unsigned long esc_rate = clk_req->escclk_rate;
	s32 ui, ui_x7;
	s32 tmax, tmin;
	s32 coeff = 1000; /* Precision, should avoid overflow */
	s32 temp;

	if (!bit_rate || !esc_rate) {
		printf("%s error: bit_rate %lu esc_rate %lu\n",
			__func__, bit_rate, esc_rate);
		return -EINVAL;
	}

	ui = mult_frac(NSEC_PER_MSEC, coeff, bit_rate / 1000);
	ui_x7 = ui * 7;

	temp = S_DIV_ROUND_UP(38 * coeff, ui_x7);
	tmin = max_t(s32, temp, 0);
	temp = (95 * coeff) / ui_x7;
	tmax = max_t(s32, temp, 0);
	timing->clk_prepare = linear_inter(tmax, tmin, 50, 0, false);

	tmin = DIV_ROUND_UP(50 * coeff, ui_x7);
	tmax = 255;
	timing->hs_rqst = linear_inter(tmax, tmin, 1, 0, false);

	tmin = DIV_ROUND_UP(100 * coeff, ui_x7) - 1;
	tmax = 255;
	timing->hs_exit = linear_inter(tmax, tmin, 10, 0, false);

	tmin = 1;
	tmax = 32;
	timing->shared_timings.clk_post = linear_inter(tmax, tmin, 80, 0, false);

	tmin = min_t(s32, 64, S_DIV_ROUND_UP(262 * coeff, ui_x7) - 1);
	tmax = 64;
	timing->shared_timings.clk_pre = linear_inter(tmax, tmin, 20, 0, false);

	printf("%d, %d, %d, %d, %d",
		timing->shared_timings.clk_pre, timing->shared_timings.clk_post,
		timing->clk_prepare, timing->hs_exit, timing->hs_rqst);

	return 0;
}

static void dsi_pll_setup_config(struct dsi_pll_config *config)
{
	config->ssc_freq = 31500;
	config->ssc_offset = 4800;
	config->ssc_adj_per = 2;

	/* TODO: ssc enable */
	config->enable_ssc = false;
	config->ssc_center = 0;
}

static void dsi_pll_calc_dec_frac(struct dsi_pll_7nm *pll, struct dsi_pll_config *config)
{
	u64 fref = VCO_REF_CLK_RATE;
	u64 pll_freq;
	u64 divider;
	u64 dec, dec_multiple;
	u32 frac;
	u64 multiplier;
	struct msm_dsi_phy_plat *plat = pll->plat;

	pll_freq = pll->vco_current_rate;

	divider = fref * 2;

	multiplier = 1 << FRAC_BITS;
	dec_multiple = div_u64(pll_freq * multiplier, divider);
	dec = div_u64_rem(dec_multiple, multiplier, &frac);

	if (plat->cfg->quirks & DSI_PHY_7NM_QUIRK_PRE_V4_1) {
		config->pll_clock_inverters = 0x28;
	} else if ((plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)) {
		if (pll_freq < 163000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq < 175000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq < 325000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq < 350000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq < 650000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq < 700000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq < 1300000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq < 2500000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq < 4000000000ULL)
			config->pll_clock_inverters = 0x00;
		else
			config->pll_clock_inverters = 0x40;
	} else if ((plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2)) {
		if (pll_freq <= 1300000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq <= 2500000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq <= 4000000000ULL)
			config->pll_clock_inverters = 0x00;
		else
			config->pll_clock_inverters = 0x40;
	} else if (plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_1) {
		if (pll_freq <= 1000000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq <= 2500000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq <= 3020000000ULL)
			config->pll_clock_inverters = 0x00;
		else
			config->pll_clock_inverters = 0x40;
	} else {
		/* 4.2, 4.3 */
		if (pll_freq <= 1000000000ULL)
			config->pll_clock_inverters = 0xa0;
		else if (pll_freq <= 2500000000ULL)
			config->pll_clock_inverters = 0x20;
		else if (pll_freq <= 3500000000ULL)
			config->pll_clock_inverters = 0x00;
		else
			config->pll_clock_inverters = 0x40;
	}

	config->decimal_div_start = dec;
	config->frac_div_start = frac;
}

#define SSC_CENTER		BIT(0)
#define SSC_EN			BIT(1)

static void dsi_pll_calc_ssc(struct dsi_pll_7nm *pll, struct dsi_pll_config *config)
{
	u32 ssc_per;
	u32 ssc_mod;
	u64 ssc_step_size;
	u64 frac;

	if (!config->enable_ssc) {
		printf("SSC not enabled\n");
		return;
	}

	ssc_per = DIV_ROUND_CLOSEST(VCO_REF_CLK_RATE, config->ssc_freq) / 2 - 1;
	ssc_mod = (ssc_per + 1) % (config->ssc_adj_per + 1);
	ssc_per -= ssc_mod;

	frac = config->frac_div_start;
	ssc_step_size = config->decimal_div_start;
	ssc_step_size *= (1 << FRAC_BITS);
	ssc_step_size += frac;
	ssc_step_size *= config->ssc_offset;
	ssc_step_size *= (config->ssc_adj_per + 1);
	ssc_step_size = div_u64(ssc_step_size, (ssc_per + 1));
	ssc_step_size = DIV_ROUND_CLOSEST_ULL(ssc_step_size, 1000000);

	config->ssc_div_per = ssc_per;
	config->ssc_stepsize = ssc_step_size;

	pr_debug("SCC: Dec:%d, frac:%llu, frac_bits:%d\n",
		 config->decimal_div_start, frac, FRAC_BITS);
	pr_debug("SSC: div_per:0x%X, stepsize:0x%X, adjper:0x%X\n",
		 ssc_per, (u32)ssc_step_size, config->ssc_adj_per);
}

static void dsi_pll_ssc_commit(struct dsi_pll_7nm *pll, struct dsi_pll_config *config)
{
	void __iomem *base = pll->plat->pll_base;

	if (config->enable_ssc) {
		pr_debug("SSC is enabled\n");

		writel(config->ssc_stepsize & 0xff,
		       base + REG_DSI_7nm_PHY_PLL_SSC_STEPSIZE_LOW_1);
		writel(config->ssc_stepsize >> 8,
		       base + REG_DSI_7nm_PHY_PLL_SSC_STEPSIZE_HIGH_1);
		writel(config->ssc_div_per & 0xff,
		       base + REG_DSI_7nm_PHY_PLL_SSC_DIV_PER_LOW_1);
		writel(config->ssc_div_per >> 8,
		       base + REG_DSI_7nm_PHY_PLL_SSC_DIV_PER_HIGH_1);
		writel(config->ssc_adj_per & 0xff,
		       base + REG_DSI_7nm_PHY_PLL_SSC_ADJPER_LOW_1);
		writel(config->ssc_adj_per >> 8,
		       base + REG_DSI_7nm_PHY_PLL_SSC_ADJPER_HIGH_1);
		writel(SSC_EN | (config->ssc_center ? SSC_CENTER : 0),
		       base + REG_DSI_7nm_PHY_PLL_SSC_CONTROL);
	}
}

static void dsi_pll_config_hzindep_reg(struct dsi_pll_7nm *pll)
{
	struct msm_dsi_phy_plat *plat = pll->plat;
	void __iomem *base = plat->pll_base;
	u8 analog_controls_five_1 = 0x01, vco_config_1 = 0x00;

	if (!(plat->cfg->quirks & DSI_PHY_7NM_QUIRK_PRE_V4_1))
		if (pll->vco_current_rate >= 3100000000ULL)
			analog_controls_five_1 = 0x03;

	if (plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_1) {
		if (pll->vco_current_rate < 1520000000ULL)
			vco_config_1 = 0x08;
		else if (pll->vco_current_rate < 2990000000ULL)
			vco_config_1 = 0x01;
	}

	if ((plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_2) ||
	    (plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_3)) {
		if (pll->vco_current_rate < 1520000000ULL)
			vco_config_1 = 0x08;
		else if (pll->vco_current_rate >= 2990000000ULL)
			vco_config_1 = 0x01;
	}

	if ((plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2) ||
	    (plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)) {
		if (pll->vco_current_rate < 1557000000ULL)
			vco_config_1 = 0x08;
		else
			vco_config_1 = 0x01;
	}

	writel(analog_controls_five_1, base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_FIVE_1);
	writel(vco_config_1, base + REG_DSI_7nm_PHY_PLL_VCO_CONFIG_1);
	writel(0x01, base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_FIVE);
	writel(0x03, base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_TWO);
	writel(0x00, base + REG_DSI_7nm_PHY_PLL_ANALOG_CONTROLS_THREE);
	writel(0x00, base + REG_DSI_7nm_PHY_PLL_DSM_DIVIDER);
	writel(0x4e, base + REG_DSI_7nm_PHY_PLL_FEEDBACK_DIVIDER);
	writel(0x40, base + REG_DSI_7nm_PHY_PLL_CALIBRATION_SETTINGS);
	writel(0xba, base + REG_DSI_7nm_PHY_PLL_BAND_SEL_CAL_SETTINGS_THREE);
	writel(0x0c, base + REG_DSI_7nm_PHY_PLL_FREQ_DETECT_SETTINGS_ONE);
	writel(0x00, base + REG_DSI_7nm_PHY_PLL_OUTDIV);
	writel(0x00, base + REG_DSI_7nm_PHY_PLL_CORE_OVERRIDE);
	writel(0x08, base + REG_DSI_7nm_PHY_PLL_PLL_DIGITAL_TIMERS_TWO);
	writel(0x0a, base + REG_DSI_7nm_PHY_PLL_PLL_PROP_GAIN_RATE_1);
	writel(0xc0, base + REG_DSI_7nm_PHY_PLL_PLL_BAND_SEL_RATE_1);
	writel(0x84, base + REG_DSI_7nm_PHY_PLL_PLL_INT_GAIN_IFILT_BAND_1);
	writel(0x82, base + REG_DSI_7nm_PHY_PLL_PLL_INT_GAIN_IFILT_BAND_1);
	writel(0x4c, base + REG_DSI_7nm_PHY_PLL_PLL_FL_INT_GAIN_PFILT_BAND_1);
	writel(0x80, base + REG_DSI_7nm_PHY_PLL_PLL_LOCK_OVERRIDE);
	writel(0x29, base + REG_DSI_7nm_PHY_PLL_PFILT);
	writel(0x2f, base + REG_DSI_7nm_PHY_PLL_PFILT);
	writel(0x2a, base + REG_DSI_7nm_PHY_PLL_IFILT);
	writel(!(plat->cfg->quirks & DSI_PHY_7NM_QUIRK_PRE_V4_1) ? 0x3f : 0x22,
	       base + REG_DSI_7nm_PHY_PLL_IFILT);

	if (!(plat->cfg->quirks & DSI_PHY_7NM_QUIRK_PRE_V4_1)) {
		writel(0x22, base + REG_DSI_7nm_PHY_PLL_PERF_OPTIMIZE);
		if (pll->slave)
			writel(0x22, pll->slave->plat->pll_base + REG_DSI_7nm_PHY_PLL_PERF_OPTIMIZE);
	}
}

static void dsi_pll_commit(struct dsi_pll_7nm *pll, struct dsi_pll_config *config)
{
	void __iomem *base = pll->plat->pll_base;

	writel(0x12, base + REG_DSI_7nm_PHY_PLL_CORE_INPUT_OVERRIDE);
	writel(config->decimal_div_start,
	       base + REG_DSI_7nm_PHY_PLL_DECIMAL_DIV_START_1);
	writel(config->frac_div_start & 0xff,
	       base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_LOW_1);
	writel((config->frac_div_start & 0xff00) >> 8,
	       base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_MID_1);
	writel((config->frac_div_start & 0x30000) >> 16,
	       base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_HIGH_1);
	writel(0x40, base + REG_DSI_7nm_PHY_PLL_PLL_LOCKDET_RATE_1);
	writel(0x06, base + REG_DSI_7nm_PHY_PLL_PLL_LOCK_DELAY);
	writel(pll->plat->cphy_mode ? 0x00 : 0x10,
	       base + REG_DSI_7nm_PHY_PLL_CMODE_1);
	writel(config->pll_clock_inverters,
	       base + REG_DSI_7nm_PHY_PLL_CLOCK_INVERTERS_1);
}

// Parent clock should be RPMh CXO output which is typically xo_board/2 but according
// to the logs below it's actually the full TCXO rate (38.4MHz)??
#define TCXO_RATE	38400000

static unsigned long _dsi_pll_7nm_vco_set_rate(struct dsi_pll_7nm *pll_7nm, unsigned long rate)
{
	struct dsi_pll_config config;

	printf("DSI PLL%d rate=%lu\n", pll_7nm->plat->id, rate);

	/*
	80Hz:
	[drm:dsi_pll_7nm_vco_set_rate] DSI PLL0 rate=851297622, parent's=38400000
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851297607, dec = 16, frac = ad45

	90Hz: that we are using
	[drm:dsi_pll_7nm_vco_set_rate] DSI PLL0 rate=851023712, parent's=38400000
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851023681, dec = 16, frac = a5f7

	120Hz:
	[drm:dsi_pll_7nm_vco_set_rate] DSI PLL0 rate=851187806, parent's=38400000
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851187744, dec = 16, frac = aa57
	*/

	pll_7nm->vco_current_rate = rate;

	dsi_pll_setup_config(&config);

	dsi_pll_calc_dec_frac(pll_7nm, &config);

	dsi_pll_calc_ssc(pll_7nm, &config);

	dsi_pll_commit(pll_7nm, &config);

	dsi_pll_config_hzindep_reg(pll_7nm);

	dsi_pll_ssc_commit(pll_7nm, &config);

	/* flush, ensure all register writes are done*/
	wmb();

	return rate;
}

static unsigned long dsi_pll_7nm_vco_set_rate(struct clk *clk, unsigned long rate)
{
	struct dsi_pll_7nm *pll_7nm = dev_get_priv(clk->dev);

	return _dsi_pll_7nm_vco_set_rate(pll_7nm, rate);
}

static int dsi_pll_7nm_lock_status(struct dsi_pll_7nm *pll)
{
	int rc;
	u32 status = 0;
	//u32 const delay_us = 100;
	u32 const timeout_us = 5000;
	struct msm_dsi_phy_plat *plat = dev_get_plat(pll->dev);

	rc = readl_poll_timeout_atomic(pll->plat->pll_base +
				       REG_DSI_7nm_PHY_PLL_COMMON_STATUS_ONE,
				       status,
				       ((status & BIT(0)) > 0),
				       timeout_us);
	if (rc)
		pr_err("DSI PLL(%d) lock failed, status=0x%08x\n",
		       pll->plat->id, status);

	return rc;
}

static void dsi_pll_disable_pll_bias(struct dsi_pll_7nm *pll)
{
	u32 data = readl(pll->plat->base + REG_DSI_7nm_PHY_CMN_CTRL_0);

	writel(0, pll->plat->pll_base + REG_DSI_7nm_PHY_PLL_SYSTEM_MUXES);
	writel(data & ~BIT(5), pll->plat->base + REG_DSI_7nm_PHY_CMN_CTRL_0);
	ndelay(250);
}

static void dsi_pll_enable_pll_bias(struct dsi_pll_7nm *pll)
{
	u32 data = readl(pll->plat->base + REG_DSI_7nm_PHY_CMN_CTRL_0);

	writel(data | BIT(5), pll->plat->base + REG_DSI_7nm_PHY_CMN_CTRL_0);
	writel(0xc0, pll->plat->pll_base + REG_DSI_7nm_PHY_PLL_SYSTEM_MUXES);
	ndelay(250);
}

static void dsi_pll_cmn_clk_cfg0_write(struct dsi_pll_7nm *pll, u32 val)
{
	writel(val, pll->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG0);
}

static void dsi_phy_pll_cmn_clk_cfg1_update(struct msm_dsi_phy *phy, u32 mask,
					u32 val)
{
	u32 data;

	data = readl(phy->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	data &= ~mask;
	data |= val & mask;

	writel(data, phy->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
}

static void dsi_pll_cmn_clk_cfg1_update(struct dsi_pll_7nm *pll, u32 mask,
					u32 val)
{
	u32 data;

	data = readl(pll->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
	data &= ~mask;
	data |= val & mask;

	writel(data, pll->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);
}

static void dsi_pll_disable_global_clk(struct dsi_pll_7nm *pll)
{
	dsi_pll_cmn_clk_cfg1_update(pll, DSI_7nm_PHY_CMN_CLK_CFG1_CLK_EN, 0);
}

static void dsi_pll_enable_global_clk(struct dsi_pll_7nm *pll)
{
	u32 cfg_1 = DSI_7nm_PHY_CMN_CLK_CFG1_CLK_EN | DSI_7nm_PHY_CMN_CLK_CFG1_CLK_EN_SEL;

	writel(0x04, pll->plat->base + REG_DSI_7nm_PHY_CMN_CTRL_3);
	dsi_pll_cmn_clk_cfg1_update(pll, cfg_1, cfg_1);
}

static void dsi_pll_phy_dig_reset(struct dsi_pll_7nm *pll)
{
	/*
	 * Reset the PHY digital domain. This would be needed when
	 * coming out of a CX or analog rail power collapse while
	 * ensuring that the pads maintain LP00 or LP11 state
	 */
	writel(BIT(0), pll->plat->base + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE4);
	wmb(); /* Ensure that the reset is deasserted */
	writel(0, pll->plat->base + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE4);
	wmb(); /* Ensure that the reset is deasserted */
}


static unsigned long dsi_pll_7nm_vco_recalc_rate(struct dsi_pll_7nm *pll_7nm,
						  unsigned long parent_rate)
{
	void __iomem *base = pll_7nm->plat->pll_base;
	u64 ref_clk = VCO_REF_CLK_RATE;
	u64 vco_rate = 0x0;
	u64 multiplier;
	u32 frac;
	u32 dec;
	u64 pll_freq, tmp64;

	dec = readl(base + REG_DSI_7nm_PHY_PLL_DECIMAL_DIV_START_1);
	dec &= 0xff;

	frac = readl(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_LOW_1);
	frac |= ((readl(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_MID_1) &
		  0xff) << 8);
	frac |= ((readl(base + REG_DSI_7nm_PHY_PLL_FRAC_DIV_START_HIGH_1) &
		  0x3) << 16);

	/*
	 * TODO:
	 *	1. Assumes prescaler is disabled
	 */
	multiplier = 1 << FRAC_BITS;
	pll_freq = dec * (ref_clk * 2);
	tmp64 = (ref_clk * 2 * frac);
	pll_freq += div_u64(tmp64, multiplier);

	vco_rate = pll_freq;
	pll_7nm->vco_current_rate = vco_rate;

	/*
	80Hz:
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851297607, dec = 16, frac = ad45

	90Hz: that we are using
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851023681, dec = 16, frac = a5f7

	120Hz:
	[drm:dsi_pll_7nm_vco_recalc_rate] DSI PLL0 returning vco rate = 851187744, dec = 16, frac = aa57
	*/
	printf("DSI PLL%d returning vco rate = %lu, dec = %x, frac = %x for parent rate %lu\n",
	    pll_7nm->plat->id, (unsigned long)vco_rate, dec, frac, parent_rate);

	return (unsigned long)vco_rate;
}

static int _dsi_pll_7nm_vco_prepare(struct dsi_pll_7nm *pll_7nm)
{
	int rc;

	/* U-Boot doesn't have recalc_rate support so just set things up now */
	/* follow sequence of kernel */
	if (!pll_7nm->vco_desired_rate)
		pll_7nm->vco_desired_rate = dsi_pll_7nm_vco_recalc_rate(pll_7nm, TCXO_RATE);
	if (!pll_7nm->vco_desired_rate) {
		pr_err("DSI PLL(%d) recalc_rate returned 0 :( overriding...\n", pll_7nm->plat->id);
		pll_7nm->vco_desired_rate = 851023681;
	}
	_dsi_pll_7nm_vco_set_rate(pll_7nm, pll_7nm->vco_desired_rate);
	pll_7nm->vco_desired_rate = dsi_pll_7nm_vco_recalc_rate(pll_7nm, TCXO_RATE);
	_dsi_pll_7nm_vco_set_rate(pll_7nm, pll_7nm->vco_desired_rate);

	dsi_pll_enable_pll_bias(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_enable_pll_bias(pll_7nm->slave);

	/* Start PLL */
	writel(BIT(0), pll_7nm->plat->base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL);

	/*
	 * ensure all PLL configurations are written prior to checking
	 * for PLL lock.
	 */
	wmb();

	/* Check for PLL lock */
	rc = dsi_pll_7nm_lock_status(pll_7nm);
	if (rc) {
		pr_err("PLL(%d) lock failed\n", pll_7nm->plat->id);
		goto error;
	}
printf("--------- PLL(%d) lock ok\n", pll_7nm->plat->id);

	/*
	 * assert power on reset for PHY digital in case the PLL is
	 * enabled after CX of analog domain power collapse. This needs
	 * to be done before enabling the global clk.
	 */
	dsi_pll_phy_dig_reset(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_phy_dig_reset(pll_7nm->slave);

	dsi_pll_enable_global_clk(pll_7nm);
	if (pll_7nm->slave)
		dsi_pll_enable_global_clk(pll_7nm->slave);

	/* Hard coding downstream clock mnd for headset usage case, to avoid
	 * create parent clocks in pll_7nm_register() */
	writel(0xff, pll_7nm->plat->base + REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE);
	writel(0xf1, pll_7nm->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG0);
	writel(0x13, pll_7nm->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG1);

error:
	return rc;
}

static int dsi_pll_7nm_vco_prepare(struct clk *clk)
{
	struct dsi_pll_7nm *pll_7nm = dev_get_priv(clk->dev);

	return _dsi_pll_7nm_vco_prepare(pll_7nm);
}

static void dsi_pll_disable_sub(struct dsi_pll_7nm *pll)
{
	writel(0, pll->plat->base + REG_DSI_7nm_PHY_CMN_RBUF_CTRL);
	dsi_pll_disable_pll_bias(pll);
}

static int dsi_pll_7nm_vco_unprepare(struct clk *clk)
{
	struct dsi_pll_7nm *pll_7nm = dev_get_priv(clk->dev);

	/*
	 * To avoid any stray glitches while abruptly powering down the PLL
	 * make sure to gate the clock using the clock enable bit before
	 * powering down the PLL
	 */
	dsi_pll_disable_global_clk(pll_7nm);
	writel(0, pll_7nm->plat->base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL);
	dsi_pll_disable_sub(pll_7nm);
	if (pll_7nm->slave) {
		dsi_pll_disable_global_clk(pll_7nm->slave);
		dsi_pll_disable_sub(pll_7nm->slave);
	}
	/* flush, ensure all register writes are done */
	wmb();

	return 0;
}

static unsigned long dsi_pll_7nm_clk_round_rate(struct clk *clk,
		unsigned long rate)
{
	struct dsi_pll_7nm *pll_7nm = dev_get_priv(clk->dev);

	if      (rate < pll_7nm->plat->cfg->min_pll_rate)
		return  pll_7nm->plat->cfg->min_pll_rate;
	else if (rate > pll_7nm->plat->cfg->max_pll_rate)
		return  pll_7nm->plat->cfg->max_pll_rate;
	else
		return rate;
}

#endif

static const struct clk_ops clk_ops_dsi_pll_7nm_vco = {
	.round_rate = dsi_pll_7nm_clk_round_rate,
	.set_rate = dsi_pll_7nm_vco_set_rate,
	//.recalc_rate = dsi_pll_7nm_vco_recalc_rate,
	.enable = dsi_pll_7nm_vco_prepare,
	.disable = dsi_pll_7nm_vco_unprepare,
};

/*
 * PLL Callbacks
 */
static int dsi_7nm_set_usecase(struct msm_dsi_phy *phy)
{
	void __iomem *base = phy->plat->base;
	u32 data = 0x0;	/* internal PLL */

	printf("DSI PLL%d", phy->plat->id);

	switch (phy->plat->usecase) {
	case MSM_DSI_PHY_STANDALONE:
		break;
	case MSM_DSI_PHY_MASTER:
		//pll_7nm->slave = pll_7nm_list[(pll_7nm->phy->id + 1) % DSI_MAX];
		/* v7.0: Enable ATB_EN0 and alternate clock output to external phy */
		if (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)
			writel(0x07, base + REG_DSI_7nm_PHY_CMN_CTRL_5);
		break;
	case MSM_DSI_PHY_SLAVE:
		data = 0x1; /* external PLL */
		break;
	default:
		return -EINVAL;
	}

	/* set PLL src */
	dsi_phy_pll_cmn_clk_cfg1_update(phy, DSI_7nm_PHY_CMN_CLK_CFG1_BITCLK_SEL__MASK,
				    DSI_7nm_PHY_CMN_CLK_CFG1_BITCLK_SEL(data));

	return 0;
}

#if 0
/*
 * The post dividers and mux clocks are created using the standard divider and
 * mux API. Unlike the 14nm PHY, the slave PLL doesn't need its dividers/mux
 * state to follow the master PLL's divider/mux state. Therefore, we don't
 * require special clock ops that also configure the slave PLL registers
 */
static int pll_7nm_register(struct dsi_pll_7nm *pll_7nm, struct clk_hw **provided_clocks)
{
	char clk_name[32];
	struct clk_init_data vco_init = {
		.parent_data = &(const struct clk_parent_data) {
			.fw_name = "ref",
		},
		.num_parents = 1,
		.name = clk_name,
		.flags = CLK_IGNORE_UNUSED,
		.ops = &clk_ops_dsi_pll_7nm_vco,
	};
	struct device *dev = &pll_7nm->phy->dev;
	struct clk *clk, *pll_out_div, *pll_bit, *pll_by_2_bit;
	struct clk *pll_post_out_div, *phy_pll_out_dsi_parent;
	int ret;

	printf("DSI%d", pll_7nm->phy->id);

	snprintf(clk_name, sizeof(clk_name), "dsi%dvco_clk", pll_7nm->phy->id);
	pll_7nm->clk.init = &vco_init;

	/* vco_clk is the parent of pll_out_div:
	 * vco_clk=>pll_out_div=>pll_bit=>phy_pll_out_byteclk
	 * vco_clk=>pll_out_div=>pll_post_out_div
	 */
	ret = clk_register(dev, &pll_7nm->clk_hw);
	if (ret)
		return ret;

	snprintf(clk_name, sizeof(clk_name), "dsi%d_pll_out_div_clk", pll_7nm->phy->id);

	pll_out_div = devm_clk_hw_register_divider_parent_hw(dev, clk_name,
			&pll_7nm->clk_hw, CLK_SET_RATE_PARENT,
			pll_7nm->phy->pll_base +
				REG_DSI_7nm_PHY_PLL_PLL_OUTDIV_RATE,
			0, 2, CLK_DIVIDER_POWER_OF_TWO, NULL);
	if (IS_ERR(pll_out_div)) {
		ret = PTR_ERR(pll_out_div);
		goto fail;
	}

	snprintf(clk_name, sizeof(clk_name), "dsi%d_pll_bit_clk", pll_7nm->phy->id);

	/* BIT CLK: DIV_CTRL_3_0 */
	pll_bit = devm_clk_hw_register_divider_parent_hw(dev, clk_name,
			pll_out_div, CLK_SET_RATE_PARENT,
			pll_7nm->phy->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG0,
			0, 4, CLK_DIVIDER_ONE_BASED, &pll_7nm->postdiv_lock);
	if (IS_ERR(pll_bit)) {
		ret = PTR_ERR(pll_bit);
		goto fail;
	}

	snprintf(clk_name, sizeof(clk_name), "dsi%d_phy_pll_out_byteclk", pll_7nm->phy->id);

	/* DSI Byte clock = VCO_CLK / OUT_DIV / BIT_DIV / 8 */
	hw = devm_clk_hw_register_fixed_factor_parent_hw(dev, clk_name,
			pll_bit, CLK_SET_RATE_PARENT, 1,
			pll_7nm->phy->cphy_mode ? 7 : 8);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto fail;
	}

	provided_clocks[DSI_BYTE_PLL_CLK] = hw;

	snprintf(clk_name, sizeof(clk_name), "dsi%d_pll_by_2_bit_clk", pll_7nm->phy->id);

	pll_by_2_bit = devm_clk_hw_register_fixed_factor_parent_hw(dev,
			clk_name, pll_bit, 0, 1, 2);
	if (IS_ERR(pll_by_2_bit)) {
		ret = PTR_ERR(pll_by_2_bit);
		goto fail;
	}

	snprintf(clk_name, sizeof(clk_name), "dsi%d_pll_post_out_div_clk", pll_7nm->phy->id);

	if (pll_7nm->phy->cphy_mode)
		pll_post_out_div = devm_clk_hw_register_fixed_factor_parent_hw(
				dev, clk_name, pll_out_div, 0, 2, 7);
/*
	else
		pll_post_out_div = devm_clk_hw_register_fixed_factor_parent_hw(
				dev, clk_name, pll_out_div, 0, 1, 4);
*/
	if (IS_ERR(pll_post_out_div)) {
		ret = PTR_ERR(pll_post_out_div);
		goto fail;
	}

	/* in CPHY mode, pclk_mux will always have post_out_div as parent
	 * don't register a pclk_mux clock and just use post_out_div instead
	 */
	if (pll_7nm->phy->cphy_mode) {
		dsi_pll_cmn_clk_cfg1_update(pll_7nm,
					    DSI_7nm_PHY_CMN_CLK_CFG1_DSICLK_SEL__MASK,
					    DSI_7nm_PHY_CMN_CLK_CFG1_DSICLK_SEL(3));
		phy_pll_out_dsi_parent = pll_post_out_div;
/*
	} else {
		snprintf(clk_name, sizeof(clk_name), "dsi%d_pclk_mux", pll_7nm->phy->id);

		hw = devm_clk_hw_register_mux_parent_hws(dev, clk_name,
				((const struct clk_hw *[]){
					pll_bit,
					pll_by_2_bit,
				}), 2, 0, pll_7nm->phy->plat->base +
					REG_DSI_7nm_PHY_CMN_CLK_CFG1,
				0, 1, 0, &pll_7nm->pclk_mux_lock);
		if (IS_ERR(hw)) {
			ret = PTR_ERR(hw);
			goto fail;
		}

		phy_pll_out_dsi_parent = hw;
*/
	}

	snprintf(clk_name, sizeof(clk_name), "dsi%d_phy_pll_out_dsiclk", pll_7nm->phy->id);

	/* PIX CLK DIV : DIV_CTRL_7_4*/
	hw = devm_clk_hw_register_divider_parent_hw(dev, clk_name,
			phy_pll_out_dsi_parent, 0,
			pll_7nm->phy->plat->base + REG_DSI_7nm_PHY_CMN_CLK_CFG0,
			4, 4, CLK_DIVIDER_ONE_BASED, &pll_7nm->postdiv_lock);
	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		goto fail;
	}

	provided_clocks[DSI_PIXEL_PLL_CLK] = hw;

	return 0;

fail:

	return ret;
}
#endif

static int dsi_pll_7nm_of_to_plat(struct udevice *dev)
{
	// Can't use dev_get_parent_plat() because dev->parent_plat_ hasn't been set yet since we're in
	// next device_probe() calls... yay
	struct msm_dsi_phy_plat *plat = dev_get_plat(dev), *pplat = dev_get_plat(dev_get_parent(dev));

	if (!pplat) {
		printf("%s: %s Parent plat not allocated?!\n", dev->name, __func__);
		return -ENODEV;
	}

	/* Copy plat data from parent */
	memcpy(plat, pplat, sizeof(*plat));

	printf("%s: %s exit\n", dev->name, __func__);

	return 0;
}

static int dsi_pll_7nm_probe(struct udevice *dev)
{
	struct dsi_pll_7nm *pll_7nm = dev_get_priv(dev);
	int index = 0;
	struct msm_dsi_phy_plat *plat = dev_get_plat(dev);

printf("========%s enter parent %s priv is %p\n", __func__, dev->parent->name, plat);

	pll_7nm->dev = dev;
	pll_7nm->plat = plat;

	pll_7nm_list[index] = pll_7nm;

	/* move clks registration from pll_7nm_register. But how
	 * to model the clk tree, or configure them at once. Or
	 * we hack with hard coding the data from kernel for temp.
	 * vco_clk(851024121 Hz) is the parent of pll_out_div:
	 * vco_clk(851024121) => pll_out_div(851024121) => pll_bit(851024121) => phy_pll_out_byteclk(121574874)
	 * vco_clk(851024121) => pll_out_div(851024121) => pll_bit(851024121) => pll_by_2_bit(425512060) for DPHY, not current CPHY
	 * vco_clk(851024121) => pll_out_div(851024121) => pll_post_out_div(243149748) => phy_pll_out_dsi(243149748)
	 */
/*
	clk = clk_register_divider(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags,
		void __iomem *reg, u8 shift, u8 width,
		u8 clk_divider_flags)
	clk_dm(clk_id, clk);
*/

	_dsi_pll_7nm_vco_prepare(pll_7nm);
	printf("%s: %s exit\n", dev->name, __func__);
	return 0;
}

#define DSI_PLL_DRV_NAME "qcom_dsi_phy_pll"

U_BOOT_DRIVER(dsi_pll) = {
	.name = DSI_PLL_DRV_NAME,
	.id = UCLASS_CLK,
	.of_to_plat = dsi_pll_7nm_of_to_plat,
	.probe = dsi_pll_7nm_probe,
	.ops = &clk_ops_dsi_pll_7nm_vco,
	.priv_auto = sizeof(struct dsi_pll_7nm),
	.plat_auto = sizeof(struct msm_dsi_phy_plat),
	.flags = DM_FLAG_NAME_ALLOCED,
};


static int dsi_phy_hw_v4_0_is_pll_on(struct msm_dsi_phy *phy)
{
	void __iomem *base = phy->plat->base;
	u32 data = 0;

	data = readl(base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL);
	mb(); /* make sure read happened */

	return (data & BIT(0));
}

static void dsi_phy_hw_v4_0_config_lpcdrx(struct msm_dsi_phy *phy, bool enable)
{
	void __iomem *lane_base = phy->plat->lane_base;
	int phy_lane_0 = 0;	/* TODO: Support all lane swap configs */

	/*
	 * LPRX and CDRX need to enabled only for physical data lane
	 * corresponding to the logical data lane 0
	 */
	if (enable)
		writel(0x3, lane_base + REG_DSI_7nm_PHY_LN_LPRX_CTRL(phy_lane_0));
	else
		writel(0, lane_base + REG_DSI_7nm_PHY_LN_LPRX_CTRL(phy_lane_0));
}

static void dsi_phy_hw_v4_0_lane_settings(struct msm_dsi_phy *phy)
{
	int i;
	const u8 tx_dctrl_0[] = { 0x00, 0x00, 0x00, 0x04, 0x01 };
	const u8 tx_dctrl_1[] = { 0x40, 0x40, 0x40, 0x46, 0x41 };
	const u8 *tx_dctrl = tx_dctrl_0;
	void __iomem *lane_base = phy->plat->lane_base;

	if (!(phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_PRE_V4_1))
		tx_dctrl = tx_dctrl_1;

	/* Strength ctrl settings */
	for (i = 0; i < 5; i++) {
		/*
		 * Disable LPRX and CDRX for all lanes. And later on, it will
		 * be only enabled for the physical data lane corresponding
		 * to the logical data lane 0
		 */
		writel(0, lane_base + REG_DSI_7nm_PHY_LN_LPRX_CTRL(i));
		writel(0x0, lane_base + REG_DSI_7nm_PHY_LN_PIN_SWAP(i));
	}

	dsi_phy_hw_v4_0_config_lpcdrx(phy, true);

	/* other settings */
	for (i = 0; i < 5; i++) {
		writel(0x0, lane_base + REG_DSI_7nm_PHY_LN_CFG0(i));
		writel(0x0, lane_base + REG_DSI_7nm_PHY_LN_CFG1(i));
		writel(i == 4 ? 0x8a : 0xa, lane_base + REG_DSI_7nm_PHY_LN_CFG2(i));
		writel(tx_dctrl[i], lane_base + REG_DSI_7nm_PHY_LN_TX_DCTRL(i));
	}
}

static int dsi_7nm_phy_enable(struct msm_dsi_phy *phy,
			      struct msm_dsi_phy_clk_request *clk_req)
{
	int ret = 0;
	u32 status;
	//u32 const delay_us = 5;
	u32 const timeout_us = 1000;
	struct msm_dsi_dphy_timing *timing = &phy->timing;
	struct udevice *pll_dev;
	void __iomem *base = phy->plat->base;
	u8 buf[64];
	bool less_than_1500_mhz;
	u32 vreg_ctrl_0, vreg_ctrl_1, lane_ctrl0;
	u32 glbl_pemph_ctrl_0;
	u32 glbl_str_swi_cal_sel_ctrl, glbl_hstx_str_ctrl_0;
	u32 glbl_rescode_top_ctrl, glbl_rescode_bot_ctrl;
	u32 data;
	unsigned long timeout;
	u32 val;

	if (phy->plat->cphy_mode)
		ret = msm_dsi_cphy_timing_calc_v4(timing, clk_req);
/*
	else
		ret = msm_dsi_dphy_timing_calc_v4(timing, clk_req);
*/
	if (ret) {
		dev_err(phy->dev,
			      "%s: PHY timing calculation failed\n", __func__);
//		return -EINVAL;
	}

	if (dsi_phy_hw_v4_0_is_pll_on(phy))
		pr_warn("PLL turned on before configuring PHY\n");

	/* Request for REFGEN READY */
	if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_3) ||
	    (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2) ||
	    (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)) {
		writel(0x1, phy->plat->base + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE10);
		udelay(500);
	}

	/* wait for REFGEN READY */
/*
	ret = readl_poll_timeout_atomic(base + REG_DSI_7nm_PHY_CMN_PHY_STATUS,
					status, (status & BIT(0)),
					timeout_us);
*/
	timeout = timer_get_us() + timeout_us;
	for (;;) {
		val = readl(base + REG_DSI_7nm_PHY_CMN_PHY_STATUS);
		if (val & BIT(0))
			break;
		if (timeout_us && time_after(timer_get_us(), timeout)) {
			val = readl(base + REG_DSI_7nm_PHY_CMN_PHY_STATUS);
			break;
		}
	}
	if (!(val & BIT(0))) {
		pr_err("Ref gen not ready. Aborting\n");
		return -EINVAL;
	}

	/* TODO: CPHY enable path (this is for DPHY only) */

printf("----%s line %d\n", __func__, __LINE__);
	/* Alter PHY configurations if data rate less than 1.5GHZ*/
	less_than_1500_mhz = (clk_req->bitclk_rate <= 1500000000);

	glbl_str_swi_cal_sel_ctrl = 0x00;
	if (phy->plat->cphy_mode) {
		vreg_ctrl_0 = 0x51;
		vreg_ctrl_1 = 0x55;
		glbl_hstx_str_ctrl_0 = 0x00;
		glbl_pemph_ctrl_0 = 0x11;
		lane_ctrl0 = 0x17;
	} else {
		vreg_ctrl_0 = less_than_1500_mhz ? 0x53 : 0x52;
		vreg_ctrl_1 = 0x5c;
		glbl_hstx_str_ctrl_0 = 0x88;
		glbl_pemph_ctrl_0 = 0x00;
		lane_ctrl0 = 0x1f;
	}

	if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)) {
		if (phy->plat->cphy_mode) {
			/* TODO: different for second phy */
			vreg_ctrl_0 = 0x57;
			vreg_ctrl_1 = 0x41;
			glbl_rescode_top_ctrl = 0x3d;
			glbl_rescode_bot_ctrl = 0x38;
		} else {
			vreg_ctrl_0 = 0x56;
			vreg_ctrl_1 = 0x19;
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3c :  0x03;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x3c;
		}
	} else if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2)) {
		if (phy->plat->cphy_mode) {
			vreg_ctrl_0 = 0x45;
			vreg_ctrl_1 = 0x41;
			glbl_rescode_top_ctrl = 0x00;
			glbl_rescode_bot_ctrl = 0x00;
		} else {
			vreg_ctrl_0 = 0x44;
			vreg_ctrl_1 = 0x19;
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3c :  0x03;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x3c;
		}
	} else if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_3)) {
		if (phy->plat->cphy_mode) {
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3d :  0x01;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x3b;
		} else {
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3d :  0x01;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x39;
		}
	} else if (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_2) {
		if (phy->plat->cphy_mode) {
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3d :  0x01;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x3b;
		} else {
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3c :  0x00;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x38 :  0x39;
		}
	} else if (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_1) {
		if (phy->plat->cphy_mode) {
			glbl_hstx_str_ctrl_0 = 0x88;
			glbl_rescode_top_ctrl = 0x00;
			glbl_rescode_bot_ctrl = 0x3c;
		} else {
			glbl_rescode_top_ctrl = less_than_1500_mhz ? 0x3d :  0x00;
			glbl_rescode_bot_ctrl = less_than_1500_mhz ? 0x39 :  0x3c;
		}
	} else {
		if (phy->plat->cphy_mode) {
			glbl_str_swi_cal_sel_ctrl = 0x03;
			glbl_hstx_str_ctrl_0 = 0x66;
		} else {
			vreg_ctrl_0 = less_than_1500_mhz ? 0x5B : 0x59;
			glbl_str_swi_cal_sel_ctrl = less_than_1500_mhz ? 0x03 : 0x00;
			glbl_hstx_str_ctrl_0 = less_than_1500_mhz ? 0x66 : 0x88;
		}
		glbl_rescode_top_ctrl = 0x03;
		glbl_rescode_bot_ctrl = 0x3c;
	}

	/* de-assert digital and pll power down */
	data = BIT(6) | BIT(5);
	writel(data, base + REG_DSI_7nm_PHY_CMN_CTRL_0);

	/* Assert PLL core reset */
	writel(0x00, base + REG_DSI_7nm_PHY_CMN_PLL_CNTRL);

	/* turn off resync FIFO */
	writel(0x00, base + REG_DSI_7nm_PHY_CMN_RBUF_CTRL);

	/* program CMN_CTRL_4 for minor_ver 2 chipsets*/
	if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2) ||
	    (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0) ||
	    (readl(base + REG_DSI_7nm_PHY_CMN_REVISION_ID0) & (0xf0)) == 0x20)
		writel(0x04, base + REG_DSI_7nm_PHY_CMN_CTRL_4);

	/* Configure PHY lane swap (TODO: we need to calculate this) */
	writel(0x21, base + REG_DSI_7nm_PHY_CMN_LANE_CFG0);
	writel(0x84, base + REG_DSI_7nm_PHY_CMN_LANE_CFG1);

	if (phy->plat->cphy_mode)
		writel(BIT(6), base + REG_DSI_7nm_PHY_CMN_GLBL_CTRL);

	/* Enable LDO */
	writel(vreg_ctrl_0, base + REG_DSI_7nm_PHY_CMN_VREG_CTRL_0);
	writel(vreg_ctrl_1, base + REG_DSI_7nm_PHY_CMN_VREG_CTRL_1);

	writel(0x00, base + REG_DSI_7nm_PHY_CMN_CTRL_3);
	writel(glbl_str_swi_cal_sel_ctrl,
	       base + REG_DSI_7nm_PHY_CMN_GLBL_STR_SWI_CAL_SEL_CTRL);
	writel(glbl_hstx_str_ctrl_0,
	       base + REG_DSI_7nm_PHY_CMN_GLBL_HSTX_STR_CTRL_0);
	writel(glbl_pemph_ctrl_0,
	       base + REG_DSI_7nm_PHY_CMN_GLBL_PEMPH_CTRL_0);
	if (phy->plat->cphy_mode)
		writel(0x01, base + REG_DSI_7nm_PHY_CMN_GLBL_PEMPH_CTRL_1);
	writel(glbl_rescode_top_ctrl,
	       base + REG_DSI_7nm_PHY_CMN_GLBL_RESCODE_OFFSET_TOP_CTRL);
	writel(glbl_rescode_bot_ctrl,
	       base + REG_DSI_7nm_PHY_CMN_GLBL_RESCODE_OFFSET_BOT_CTRL);
	writel(0x55, base + REG_DSI_7nm_PHY_CMN_GLBL_LPTX_STR_CTRL);

printf("----%s line %d\n", __func__, __LINE__);
	/* Remove power down from all blocks */
	writel(0x7f, base + REG_DSI_7nm_PHY_CMN_CTRL_0);

	writel(lane_ctrl0, base + REG_DSI_7nm_PHY_CMN_LANE_CTRL0);

	/* Select full-rate mode */
	if (!phy->plat->cphy_mode)
		writel(0x40, base + REG_DSI_7nm_PHY_CMN_CTRL_2);

printf("----%s line %d\n", __func__, __LINE__);
	/* pll is touched inside dsi_7nm_set_usecase, so we need to init
	 * pll struct first, i.e. init the pll driver.
	 * The pll struct is init during dsi_phy_driver_probe in kernel.
	 * But pll is another driver in uboot.
	 * And phy should be enabled before PLL, per comments where
	 * dsi_phy_hw_v4_0_is_pll_on() is called */
	snprintf(buf, sizeof(buf), "dsi_phy%d-pll", phy->plat->id);
	ret = uclass_get_device_by_name(UCLASS_CLK, buf, &pll_dev);
	if (ret) {
		dev_err(phy->dev, "%s: failed to get %s\n",
			__func__, buf);
		return ret;
	}

	ret = dsi_7nm_set_usecase(phy);
	if (ret) {
		dev_err(phy->dev, "%s: set pll usecase failed, %d\n",
			__func__, ret);
		return ret;
	}
printf("----%s line %d\n", __func__, __LINE__);

	/* DSI PHY timings */
	if (phy->plat->cphy_mode) {
		writel(0x00, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_0);
		writel(timing->hs_exit, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_4);
		writel(timing->shared_timings.clk_pre,
		       base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_5);
		writel(timing->clk_prepare, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_6);
		writel(timing->shared_timings.clk_post,
		       base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_7);
		writel(timing->hs_rqst, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_8);
		writel(0x02, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_9);
		writel(0x04, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_10);
		writel(0x00, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_11);
	} else {
		writel(0x00, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_0);
		writel(timing->clk_zero, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_1);
		writel(timing->clk_prepare, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_2);
		writel(timing->clk_trail, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_3);
		writel(timing->hs_exit, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_4);
		writel(timing->hs_zero, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_5);
		writel(timing->hs_prepare, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_6);
		writel(timing->hs_trail, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_7);
		writel(timing->hs_rqst, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_8);
		writel(0x02, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_9);
		writel(0x04, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_10);
		writel(0x00, base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_11);
		writel(timing->shared_timings.clk_pre,
		       base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_12);
		writel(timing->shared_timings.clk_post,
		       base + REG_DSI_7nm_PHY_CMN_TIMING_CTRL_13);
	}

printf("----%s line %d\n", __func__, __LINE__);
	/* DSI lane settings */
	dsi_phy_hw_v4_0_lane_settings(phy);

	printf("DSI%d PHY enabled", phy->plat->id);

	return 0;
}

static bool dsi_7nm_set_continuous_clock(struct msm_dsi_phy *phy, bool enable)
{
	void __iomem *base = phy->plat->base;
	u32 data;

	data = readl(base + REG_DSI_7nm_PHY_CMN_LANE_CTRL1);
	if (enable)
		data |= BIT(5) | BIT(6);
	else
		data &= ~(BIT(5) | BIT(6));
	writel(data, base + REG_DSI_7nm_PHY_CMN_LANE_CTRL1);

	return enable;
}

static void dsi_7nm_phy_disable(struct msm_dsi_phy *phy)
{
	void __iomem *base = phy->plat->base;
	u32 data;

	printf("");

	if (dsi_phy_hw_v4_0_is_pll_on(phy))
		pr_warn("Turning OFF PHY while PLL is on\n");

	dsi_phy_hw_v4_0_config_lpcdrx(phy, false);

	/* Turn off REFGEN Vote */
	if ((phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V4_3) ||
	    (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V5_2) ||
	    (phy->plat->cfg->quirks & DSI_PHY_7NM_QUIRK_V7_0)) {
		writel(0x0, base + REG_DSI_7nm_PHY_CMN_GLBL_DIGTOP_SPARE10);
		wmb();
		/* Delay to ensure HW removes vote before PHY shut down */
		udelay(2);
	}

	data = readl(base + REG_DSI_7nm_PHY_CMN_CTRL_0);

	/* disable all lanes */
	data &= ~0x1F;
	writel(data, base + REG_DSI_7nm_PHY_CMN_CTRL_0);
	writel(0, base + REG_DSI_7nm_PHY_CMN_LANE_CTRL0);

	/* Turn off all PHY blocks */
	writel(0x00, base + REG_DSI_7nm_PHY_CMN_CTRL_0);
	/* make sure phy is turned off */
	wmb();

	printf("DSI%d PHY disabled", phy->plat->id);
}


/* below are copied from other soc driver for ref */
static void msm_dsi_phy_mipi_mode_enable(struct msm_dsi_phy *qcom)
{
//	struct phy_configure_opts_mipi_dphy *cfg = &qcom->dphy_cfg;
	const struct qcom_mipi_dphy_timing *timings;
/*
	u32 t_txbyteclkhs, t_txclkesc;
	u32 txbyteclkhs, txclkesc, esc_clk_div;
	u32 hs_exit, clk_post, clk_pre, wakeup, lpx, ta_go, ta_sure, ta_wait;
	u32 hs_prepare, hs_trail, hs_zero, clk_lane_hs_zero, data_lane_hs_zero;
	unsigned int i;
*/

return;
	//timings = qcom->pdata->qcom_mipi_dphy_timing_table;

#if 0
	//msm_dsi_phy_pll_calc_rate(qcom, cfg->hs_clk_rate);

	txbyteclkhs = qcom->pll.rate / 8;
	t_txbyteclkhs = div_u64(PSEC_PER_SEC, txbyteclkhs);

	esc_clk_div = DIV_ROUND_UP(txbyteclkhs, 20000000);
	txclkesc = txbyteclkhs / esc_clk_div;
	t_txclkesc = div_u64(PSEC_PER_SEC, txclkesc);

	/*
	 * The value of counter for HS Ths-exit
	 * Ths-exit = Tpin_txbyteclkhs * value
	 */
	hs_exit = DIV_ROUND_UP(cfg->hs_exit, t_txbyteclkhs);
	/*
	 * The value of counter for HS Tclk-post
	 * Tclk-post = Tpin_txbyteclkhs * value
	 */
	clk_post = DIV_ROUND_UP(cfg->clk_post, t_txbyteclkhs);
	/*
	 * The value of counter for HS Tclk-pre
	 * Tclk-pre = Tpin_txbyteclkhs * value
	 */
	clk_pre = DIV_ROUND_UP(cfg->clk_pre, BITS_PER_BYTE);

	/*
	 * The value of counter for HS Tta-go
	 * Tta-go for turnaround
	 * Tta-go = Ttxclkesc * value
	 */
	ta_go = DIV_ROUND_UP(cfg->ta_go, t_txclkesc);
	/*
	 * The value of counter for HS Tta-sure
	 * Tta-sure for turnaround
	 * Tta-sure = Ttxclkesc * value
	 */
	ta_sure = DIV_ROUND_UP(cfg->ta_sure, t_txclkesc);
	/*
	 * The value of counter for HS Tta-wait
	 * Tta-wait for turnaround
	 * Tta-wait = Ttxclkesc * value
	 */
	ta_wait = DIV_ROUND_UP(cfg->ta_get, t_txclkesc);

	for (i = 0; i < qcom->pdata->num_timings; i++)
		if (qcom->pll.rate <= timings[i].rate)
			break;

	if (i == qcom->pdata->num_timings)
		--i;

	/*
	 * The value of counter for HS Tlpx Time
	 * Tlpx = Tpin_txbyteclkhs * (2 + value)
	 */
	if (qcom->pdata->max_rate == MAX_1GHZ) {
		lpx = DIV_ROUND_UP(cfg->lpx, t_txbyteclkhs);
		if (lpx >= 2)
			lpx -= 2;
	} else {
		lpx = timings[i].lpx;
	}

	hs_prepare = timings[i].hs_prepare;
	hs_trail = timings[i].hs_trail;
	clk_lane_hs_zero = timings[i].clk_lane_hs_zero;
	data_lane_hs_zero = timings[i].data_lane_hs_zero;
	wakeup = 0x3ff;

	for (i = REGISTER_PART_CLOCK_LANE; i <= REGISTER_PART_DATA3_LANE; i++) {
		if (i == REGISTER_PART_CLOCK_LANE)
			hs_zero = clk_lane_hs_zero;
		else
			hs_zero = data_lane_hs_zero;
		/* configure registers below */
	}
#endif
}

static int msm_dsi_phy_power_on_internal(struct msm_dsi_phy *qcom)
{
	/* TODO: need to calculate it via panel/lane/dsc data */
	struct msm_dsi_phy_clk_request clk_req = {
		.bitclk_rate = 851187806,
		.escclk_rate = 19200000,
	 };

	clk_prepare_enable(qcom->ahb_clk);
	clk_prepare_enable(qcom->ref_clk);
	/* the rate is from linux test */
	clk_set_rate(qcom->ahb_clk, 19200000);

	return dsi_7nm_phy_enable(qcom, &clk_req);
return 0;
	//return dsi_pll_7nm_init(phy);
	//clk_prepare_enable(qcom->ref_clk);

	msm_dsi_phy_mipi_mode_enable(qcom);

	return 0;
}

static int msm_dsi_phy_power_on(struct phy *phy)
{
	struct msm_dsi_phy *qcom = dev_get_priv(phy->dev);

printf("======%s enter with struct phy->id %lu (internal id %d), skipped so far\n", __func__, phy->id, qcom->plat->id);
return 0;
	return msm_dsi_phy_power_on_internal(qcom);
}

static int msm_dsi_phy_power_off(struct phy *phy)
{
	struct msm_dsi_phy *qcom = dev_get_priv(phy->dev);

return 0;
	//clk_disable_unprepare(qcom->ref_clk);
	clk_disable_unprepare(qcom->ahb_clk);

	return 0;
}

static int msm_dsi_phy_configure(struct phy *phy, void *params)
{
	struct msm_dsi_phy *qcom = dev_get_priv(phy->dev);
	struct phy_configure_opts_mipi_dphy *config = params;
	int ret;

	/* We finish configuration in power_on so far */
return 0;
	ret = phy_mipi_dphy_config_validate(config);
	if (ret)
		return ret;

	//memcpy(&qcom->dphy_cfg, config, sizeof(qcom->dphy_cfg));

	return 0;
}

static const struct phy_ops msm_dsi_phy_ops = {
	.configure = msm_dsi_phy_configure,
	.power_on = msm_dsi_phy_power_on,
	.power_off = msm_dsi_phy_power_off,
};

const struct msm_dsi_phy_cfg dsi_phy_4nm_8650_cfgs = {
	.has_phy_lane = true,
	.ops = {
		.set_continuous_clock = dsi_7nm_set_continuous_clock,
	},
	.min_pll_rate = 600000000UL,
#ifdef CONFIG_64BIT
	.max_pll_rate = 5000000000UL,
#else
	.max_pll_rate = ULONG_MAX,
#endif
	.io_start = { 0xae95000, 0xae97000 },
	.num_dsi_phy = 2,
	.quirks = DSI_PHY_7NM_QUIRK_V5_2,
};

static int dsi_phy_get_id(struct msm_dsi_phy_plat *plat)
{
	const struct msm_dsi_phy_cfg *cfg = plat->cfg;
	int i;

	for (i = 0; i < cfg->num_dsi_phy; i++) {
		if (cfg->io_start[i] == (phys_addr_t)plat->base)
			return i;
	}

	return -EINVAL;
}

static int qcom_dsi_phy_bind(struct udevice *dev)
{
	static int dsi_phy_id = 0;
	struct driver *drv;
	int ret;
	char *buf;
	void *driver_data = NULL;

	buf = malloc(64);
	if (!buf)
		return -ENOMEM;

	/* FIXME: dsi_phy_id should be aligned with dsi_phy_get_id() */
	snprintf(buf, 64, "dsi_phy%d-pll", dsi_phy_id++);

	drv = lists_driver_lookup_name(DSI_PLL_DRV_NAME);

	/// FIXME: CASEY: populate driver_data with configs necessary for the PLL.
	/// IMPORTANT: we can't access data from the phy device itself since it may not be
	/// probed when clk ops are called!
printf("------- binding device %s\n", buf);
	ret = device_bind_with_driver_data(dev, drv, buf, (ulong)driver_data,
					 dev_ofnode(dev), NULL);
	if (ret)
		dev_err(dev, "failed to bind pll driver: %d\n", ret);

	return ret;
}

static int qcom_dsi_phy_of_to_plat(struct udevice *dev)
{
	struct msm_dsi_phy_plat *plat = dev_get_plat(dev);

	printf("%s: dev is %p\n", __func__, dev);

	plat->cfg = &dsi_phy_4nm_8650_cfgs;
	// FIXME: read from DTS
	plat->cphy_mode = true;

	plat->base = dev_remap_addr_name(dev, "dsi_phy");
	if (IS_ERR(plat->base))
		return PTR_ERR(plat->base);

	plat->pll_base = dev_remap_addr_name(dev, "dsi_pll");
	if (IS_ERR(plat->pll_base))
		return PTR_ERR(plat->pll_base);

	plat->lane_base = dev_remap_addr_name(dev, "dsi_phy_lane");
	if (IS_ERR(plat->lane_base))
		return PTR_ERR(plat->lane_base);

printf("========%s udev %p base %p pll_base %p lane_base %p\n", __func__, dev, plat->base, plat->pll_base, plat->lane_base);
	plat->id = dsi_phy_get_id(plat);
	if (plat->id)
		plat->usecase = MSM_DSI_PHY_SLAVE;
	else
		plat->usecase = MSM_DSI_PHY_MASTER;

	return 0;
}

static int msm_dsi_phy_probe(struct udevice *dev)
{
	struct msm_dsi_phy *qcom = dev_get_priv(dev);
	int ret;

	qcom->dev = dev;
	qcom->plat = dev_get_plat(dev);

	qcom->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(qcom->ref_clk)) {
		ret = PTR_ERR(qcom->ref_clk);
		dev_err(dev, "failed to get ref clock: %d\n", ret);
		return ret;
	}

	qcom->ahb_clk = devm_clk_get(dev, "iface");
	if (IS_ERR(qcom->ahb_clk)) {
		ret = PTR_ERR(qcom->ahb_clk);
		dev_err(dev, "failed to get phy iface clk: %d\n", ret);
		return ret;
	}

	ret =  device_get_supply_regulator(dev, "vdds-supply",
					   &qcom->vdds_reg);
	if (ret) {
		dev_err(dev, "failed to get phy regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_set_enable(qcom->vdds_reg, true);
	if (ret) {
		dev_err(dev, "failed to enable phy regulator: %d\n", ret);
		return ret;
	}

	ret = regulator_set_mode(qcom->vdds_reg, REGULATOR_MODE_HPM);
	if (ret) {
		dev_err(dev, "failed to set regulator mode: %d\n", ret);
		return ret;
	}

	/* Enable phy by default, so that phy pll works when clk ops is called */
	msm_dsi_phy_power_on_internal(qcom);

printf("========%s ret ok\n", __func__);
	return ret;
}

static const struct udevice_id msm_dsi_phy_of_match[] = {
	{ .compatible = "qcom,sm8650-dsi-phy-4nm" },
	{}
};

U_BOOT_DRIVER(qcom_mdss_dsidphy) = {
	.name = "qcom-dsidphy",
	.id = UCLASS_PHY,
	.of_match = msm_dsi_phy_of_match,
	.bind = qcom_dsi_phy_bind,
	.of_to_plat = qcom_dsi_phy_of_to_plat,
	.probe = msm_dsi_phy_probe,
	.ops = &msm_dsi_phy_ops,
	.priv_auto = sizeof(struct msm_dsi_phy),
	.plat_auto = sizeof(struct msm_dsi_phy_plat),
};
