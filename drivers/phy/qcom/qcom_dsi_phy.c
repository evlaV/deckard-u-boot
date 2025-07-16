/* SPDX-License-Identifier: GPL-2.0-only */

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <div64.h>
#include <generic-phy.h>
#include <linux/kernel.h>
#include <linux/iopoll.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
//#include <lib.h>
#include <linux/math64.h>
#include <linux/time.h>
#include <phy-mipi-dphy.h>
#include <string.h>
#include <timer.h>

//#include "mdssreg.h"
//#include "qcom_display_resources.h"
//#include "qcom_mipi_dsi.h"
//#include "qcom_dsi_phy.h"

#define HAL_DSI_PHY_PLL_READY_TIMEOUT_MS               150           /* ~15 ms */
#define HAL_DSI_PHY_REFGEN_TIMEOUT_MS                  150           /* ~15 ms */

#define DSI_MAX_REFRESH_RATE       95
#define DSI_MIN_REFRESH_RATE       15

#define HAL_DSI_PLL_VCO_MIN_MHZ_2_2_0               1000

#define S_DIV_ROUND_UP(n, d)	\
	(((n) >= 0) ? (((n) + (d) - 1) / (d)) : (((n) - (d) + 1) / (d)))

#define mult_frac(x, numer, denom)(			\
{							\
	typeof(x) quot = (x) / (denom);			\
	typeof(x) rem  = (x) % (denom);			\
	(quot * (numer)) + ((rem * (numer)) / (denom));	\
}							\
)

#define write32(addr, val) writel(val, addr)
#define read32(addr) readl(addr)

struct clk_hw {
	struct clk_core *core;
	struct clk *clk;
	const struct clk_init_data *init;
};

struct qcom_video_phy_plat_data {
	const struct qcom_mipi_dphy_timing *qcom_mipi_dphy_timing_table;
	const unsigned int num_timings;
	//enum phy_max_rate max_rate;
};

struct qcom_dsidphy {
	struct udevice *dev;
	struct clk *ref_clk;
	struct clk *ahb_clk;
	struct clk *pclk_host;
	const struct qcom_video_phy_plat_data *pdata;
	void __iomem *base;
	void __iomem *pll_base;
	void __iomem *reg_base;
	void __iomem *lane_base;
	struct phy_configure_opts_mipi_dphy dphy_cfg;

	struct clk *pll_clk;
	struct {
		struct clk_hw hw;
		u8 prediv;
		u16 fbdiv;
		unsigned long rate;
	} pll;
};


#if 0
struct dsi_phy_divider_lut_entry_type {
	uint16_t        pll_post_div;
	uint16_t        phy_post_div;
};

/* PLL divider LUTs */
static struct dsi_phy_divider_lut_entry_type pll_dividerlut_dphy[] = {
/* pll post div will always be power of 2 */
	{ 2, 11 },
	{ 4, 5 },
	{ 2, 9 },
	{ 8, 2 },
	{ 1, 15 },
	{ 2, 7 },
	{ 1, 13 },
	{ 4, 3 },
	{ 1, 11 },
	{ 2, 5 },
	{ 1, 9 },
	{ 8, 1 },
	{ 1, 7 },
	{ 2, 3 },
	{ 1, 5 },
	{ 4, 1 },
	{ 1, 3 },
	{ 2, 1 },
	{ 1, 1 }
};

enum dsi_laneid_type {
	DSI_LANEID_0 = 0,
	DSI_LANEID_1,
	DSI_LANEID_2,
	DSI_LANEID_3,
	DSI_LANEID_CLK,
	DSI_LANEID_MAX,
	DSI_LANEID_FORCE_32BIT = 0x7FFFFFFF
};

struct dsi_phy_configtype {
	uint32_t desired_bitclk_freq;
	uint32_t bits_per_pixel;
	uint32_t num_data_lanes;
	uint32_t pclk_divnumerator;
	uint32_t pclk_divdenominator;

	/* pixel clk source select */
	uint32_t phy_post_div;
	uint32_t pll_post_div;
};

static inline s32 linear_inter(s32 tmax, s32 tmin, s32 percent,
				s32 min_result, bool even)
{
	s32 v;

	v = (tmax - tmin) * percent;
	v = S_DIV_ROUND_UP(v, 100) + tmin;
	if (even && (v & 0x1))
		return MAX(min_result, v - 1);

	return MAX(min_result, v);
}

static void mdss_dsi_phy_reset(struct dsi_phy_regs *phy_reg)
{
	write32(phy_reg->phy_cmn_ctrl1, 0x40);
	udelay(100);
	write32(phy_reg->phy_cmn_ctrl1, 0x0);
}

static void mdss_dsi_power_down(struct dsi_phy_regs *phy_reg)
{
	/* power up DIGTOP & PLL */
	write32(phy_reg->phy_cmn_ctrl0, 0x60);

	/* Disable PLL */
	write32(phy_reg->phy_cmn_pll_ctrl, 0x0);

	/* Resync re-time FIFO OFF*/
	write32(phy_reg->phy_cmn_rbuf_ctrl, 0x0);
}

static void mdss_dsi_phy_setup_lanephy(struct dsi_phy_regs *phy_reg,
	    enum dsi_laneid_type lane)
{
	uint32_t reg_val = 0;
	uint32_t lprx_ctrl = 0;
	uint32_t hstx_strength = 0x88;
	uint32_t data_strength_lp_n = 0x5;
	uint32_t data_strength_lp_p = 0x5;
	uint32_t pemph_bottom = 0;
	uint32_t pemph_top = 0;
	uint32_t strength_override = 0;
	uint32_t clk_lane = 0;

	if (lane == DSI_LANEID_CLK)
		clk_lane = 1;
	else
		clk_lane = 0;

	if (lane == DSI_LANEID_0)
		lprx_ctrl = 3;

	/*
	 * DSIPHY_STR_LP_N
	 * DSIPHY_STR_LP_P
	 */
	reg_val = ((data_strength_lp_n << 0x4) & 0xf0) |
		   (data_strength_lp_p & 0x0f);

	write32(phy_reg->phy_ln_regs[lane].dln0_lptx_str_ctrl, reg_val);

	/*
	 * DSIPHY_LPRX_EN
	 * DSIPHY_CDRX_EN
	 * Transition from 0 to 1 for DLN0-3 CLKLN stays 0
	 */
	write32(phy_reg->phy_ln_regs[lane].dln0_lprx_ctrl, 0x0);
	write32(phy_reg->phy_ln_regs[lane].dln0_lprx_ctrl, lprx_ctrl);

	/* Pin Swap */
	write32(phy_reg->phy_ln_regs[lane].dln0_pin_swap, 0x0);

	/*
	 * DSIPHY_HSTX_STR_HSTOP
	 * DSIPHY_HSTX_STR_HSBOT
	 */
	write32(phy_reg->phy_ln_regs[lane].dln0_hstx_str_ctrl, hstx_strength);

	/* PGM Delay */
	write32(phy_reg->phy_ln_regs[lane].dln0_cfg[0], 0x0);

	/* DLN0_CFG1 */
	reg_val = (strength_override << 0x5) & 0x20;
	write32(phy_reg->phy_ln_regs[lane].dln0_cfg[1], reg_val);

	/* DLN0_CFG2 */
	reg_val = ((pemph_bottom << 0x04) & 0xf0) |
		   (pemph_top & 0x0f);

	write32(phy_reg->phy_ln_regs[lane].dln0_cfg[2], reg_val);
	write32(phy_reg->phy_ln_regs[lane].dln0_offset_top_ctrl, 0x0);
	write32(phy_reg->phy_ln_regs[lane].dln0_offset_bot_ctrl, 0x0);

	/*
	 * DSIPHY_LPRX_DLY
	 * IS_CKLANE
	 */
	reg_val = (clk_lane << 0x07) & 0x80;
	write32(phy_reg->phy_ln_regs[lane].dln0_cfg[3], reg_val);

	reg_val = 0;
	if (lane == DSI_LANEID_CLK)
		reg_val = 1;

	write32(phy_reg->phy_ln_regs[lane].dln0_tx_dctrl, reg_val);
}

static void mdss_dsi_calculate_phy_timings(struct msm_dsi_phy_ctrl *timing,
					   struct dsi_phy_configtype *phy_cfg)
{
	const unsigned long bit_rate = phy_cfg->desired_bitclk_freq;
	s32 ui, ui_x8;
	s32 tmax, tmin;
	s32 pcnt0 = 50;
	s32 pcnt1 = 50;
	s32 pcnt2 = 10;
	s32 pcnt3 = 30;
	s32 pcnt4 = 10;
	s32 pcnt5 = 2;
	s32 coeff = 1000; /* Precision, should avoid overflow */
	s32 hb_en, hb_en_ckln;
	s32 temp;

	if (!bit_rate)
		return;

	hb_en = 0;
	timing->half_byte_clk_en = 0;
	hb_en_ckln = 0;

	ui = mult_frac(1000000, coeff, bit_rate / 1000);
	ui_x8 = ui << 3;

	temp = S_DIV_ROUND_UP(38 * coeff, ui_x8);
	tmin = MAX(temp, 0);
	temp = (95 * coeff) / ui_x8;
	tmax = MAX(temp, 0);
	timing->clk_prepare = linear_inter(tmax, tmin, pcnt0, 0, false);

	temp = 300 * coeff - (timing->clk_prepare << 3) * ui;
	tmin = S_DIV_ROUND_UP(temp, ui_x8) - 1;
	tmax = (tmin > 255) ? 511 : 255;
	timing->clk_zero = linear_inter(tmax, tmin, pcnt5, 0, false);

	tmin = DIV_ROUND_UP(60 * coeff + 3 * ui, ui_x8);
	temp = 105 * coeff + 12 * ui - 20 * coeff;
	tmax = (temp + 3 * ui) / ui_x8;
	timing->clk_trail = linear_inter(tmax, tmin, pcnt3, 0, false);

	temp = S_DIV_ROUND_UP(40 * coeff + 4 * ui, ui_x8);
	tmin = MAX(temp, 0);
	temp = (85 * coeff + 6 * ui) / ui_x8;
	tmax = MAX(temp, 0);
	timing->hs_prepare = linear_inter(tmax, tmin, pcnt1, 0, false);

	temp = 145 * coeff + 10 * ui - (timing->hs_prepare << 3) * ui;
	tmin = S_DIV_ROUND_UP(temp, ui_x8) - 1;
	tmax = 255;
	timing->hs_zero = linear_inter(tmax, tmin, pcnt4, 0, false);

	tmin = DIV_ROUND_UP(60 * coeff + 4 * ui, ui_x8) - 1;
	temp = 105 * coeff + 12 * ui - 20 * coeff;
	tmax = (temp / ui_x8) - 1;
	timing->hs_trail = linear_inter(tmax, tmin, pcnt3, 0, false);

	temp = 50 * coeff + ((hb_en << 2) - 8) * ui;
	timing->hs_rqst = S_DIV_ROUND_UP(temp, ui_x8);

	tmin = DIV_ROUND_UP(100 * coeff, ui_x8) - 1;
	tmax = 255;
	timing->hs_exit = linear_inter(tmax, tmin, pcnt2, 0, false);

	temp = 50 * coeff + ((hb_en_ckln << 2) - 8) * ui;
	timing->hs_rqst = S_DIV_ROUND_UP(temp, ui_x8);

	temp = 60 * coeff + 52 * ui - 43 * ui;
	tmin = DIV_ROUND_UP(temp, ui_x8) - 1;
	tmax = 63;
	timing->clk_post = linear_inter(tmax, tmin, pcnt2, 0, false);

	temp = 8 * ui + (timing->clk_prepare << 3) * ui;
	temp += (((timing->clk_zero + 3) << 3) + 11) * ui;
	temp += hb_en_ckln ? (((timing->hs_rqst << 3) + 4) * ui) :
			(((timing->hs_rqst << 3) + 8) * ui);
	tmin = S_DIV_ROUND_UP(temp, ui_x8) - 1;
	tmax = 63;
	if (tmin > tmax) {
		temp = linear_inter(tmax << 1, tmin, pcnt2, 0, false);
		timing->clk_pre = temp >> 1;
		timing->clk_pre_inc_by_2 = 1;
	} else {
		timing->clk_pre = linear_inter(tmax, tmin, pcnt2, 0, false);
		timing->clk_pre_inc_by_2 = 0;
	}

	timing->ta_go = 3;
	timing->ta_sure = 0;
	timing->ta_get = 4;

	printf("PHY timings: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
		timing->clk_pre, timing->clk_post,
		timing->clk_pre_inc_by_2, timing->clk_zero,
		timing->clk_trail, timing->clk_prepare, timing->hs_exit,
		timing->hs_zero, timing->hs_prepare, timing->hs_trail,
		timing->hs_rqst);
}

static int mdss_dsi_phy_timings(struct dsi_phy_regs *phy_reg,
				struct msm_dsi_phy_ctrl *phy_timings)
{
	uint32_t reg_val = 0;

	/*
	 * Step 4 Common block including GlobalTiming Parameters
	 * BYTECLK_SEL
	 */
	reg_val = (0x02 << 3) & 0x18;
	write32(phy_reg->phy_cmn_glbl_ctrl, reg_val);

	/* VREG_CTRL */
	write32(phy_reg->phy_cmn_vreg_ctrl, 0x59);

	/*HALFBYTECLK_EN*/
	write32(phy_reg->phy_cmn_timing_ctrl[0], phy_timings->half_byte_clk_en);

	/* T_CLK_ZERO */
	write32(phy_reg->phy_cmn_timing_ctrl[1], phy_timings->clk_zero);

	/* T_CLK_PREPARE */
	write32(phy_reg->phy_cmn_timing_ctrl[2], phy_timings->clk_prepare);

	/* T_CLK_TRAIL */
	write32(phy_reg->phy_cmn_timing_ctrl[3], phy_timings->clk_trail);

	/* T_HS_EXIT */
	write32(phy_reg->phy_cmn_timing_ctrl[4], phy_timings->hs_exit);

	/* T_HS_ZERO */
	write32(phy_reg->phy_cmn_timing_ctrl[5], phy_timings->hs_zero);

	/* T_HS_PREPARE */
	write32(phy_reg->phy_cmn_timing_ctrl[6], phy_timings->hs_prepare);

	/* T_HS_TRAIL */
	write32(phy_reg->phy_cmn_timing_ctrl[7], phy_timings->hs_trail);

	/* T_HS_RQST */
	write32(phy_reg->phy_cmn_timing_ctrl[8], phy_timings->hs_rqst);

	/* T_TA_GO & T_TA_SURE */
	write32(phy_reg->phy_cmn_timing_ctrl[9],
			phy_timings->ta_sure << 3 | phy_timings->ta_go);

	/* T_TA_GET */
	write32(phy_reg->phy_cmn_timing_ctrl[10], phy_timings->ta_get);

	/*DSIPHY_TRIG3_CMD*/
	write32(phy_reg->phy_cmn_timing_ctrl[11], 0x0);

	/* DSI clock out timing ctrl T_CLK_PRE & T_CLK_POST*/
	reg_val = ((phy_timings->clk_post << 8) | phy_timings->clk_pre);
	write32(&dsi0->clkout_timing_ctrl, reg_val);

	/* DCTRL */
	write32(phy_reg->phy_cmn_ctrl2, 0x40);

	return 0;
}

static int dsi_phy_waitforrefgen(struct dsi_phy_regs *phy_reg)
{
	uint32_t timeout = HAL_DSI_PHY_REFGEN_TIMEOUT_MS;
	uint32_t refgen  = 0;
	int ret = 0;

	while (!refgen) {
		refgen = (read32(phy_reg->phy_cmn_phy_status) & 0x1);
		if (!refgen) {
			udelay(100);
			timeout--;
			if (!timeout) {
				/* timeout while polling the lock status */
				ret = -1;
				break;
			}
		}
	}

	return ret;
}

static int mdss_dsi_phy_commit(struct dsi_phy_regs *phy_reg)
{
	int ret = 0;

	ret = dsi_phy_waitforrefgen(phy_reg);
	if (ret) {
		printf("%s: waitforrefgen error\n", __func__);
		return ret;
	}

	mdss_dsi_power_down(phy_reg);

	/* Remove PLL, DIG and all lanes from pwrdn */
	write32(phy_reg->phy_cmn_ctrl0, 0x7F);

	/* Lane enable */
	write32(phy_reg->phy_cmn_dsi_lane_ctrl0, 0x1F);

	mdss_dsi_phy_setup_lanephy(phy_reg, DSI_LANEID_0);
	mdss_dsi_phy_setup_lanephy(phy_reg, DSI_LANEID_1);
	mdss_dsi_phy_setup_lanephy(phy_reg, DSI_LANEID_2);
	mdss_dsi_phy_setup_lanephy(phy_reg, DSI_LANEID_3);
	mdss_dsi_phy_setup_lanephy(phy_reg, DSI_LANEID_CLK);

	return ret;
}

static void mdss_dsi_phy_setup(struct dsi_phy_regs *phy_reg)
{
	/* First reset phy */
	mdss_dsi_phy_reset(phy_reg);

	/* commit phy settings */
	mdss_dsi_phy_commit(phy_reg);
}

static void dsi_phy_resync_fifo(struct dsi_phy_regs *phy_reg)
{
	/* Resync FIFO*/
	write32(phy_reg->phy_cmn_rbuf_ctrl, 0x1);
}

static void dsi_phy_pll_global_clk_enable(struct dsi_phy_regs *phy_reg, bool enable)
{
	uint32_t clk_cfg = read32(phy_reg->phy_cmn_clk_cfg1);
	uint32_t clk_enable = 0;

	/* Set CLK_EN */
	if (enable)
		clk_enable = 1;

	clk_cfg &= ~0x20;
	clk_cfg |= ((clk_enable << 0x5) & 0x20);

	/* clk cfg1 */
	write32(phy_reg->phy_cmn_clk_cfg1, clk_cfg);
}

static int dsi_phy_pll_lock_detect(struct dsi_phy_regs *phy_reg)
{
	int ret = 0;

	/* Enable PLL */
	write32(phy_reg->phy_cmn_pll_ctrl, 0x1);

	/* Wait for Lock */
	if (!wait_us(15000, read32(&phy_pll_qlink->pll_common_status_one) & 0x1)) {
		/* timeout while polling the lock status */
		ret = -1;
		printf("dsi pll lock detect timedout, error.\n");
	}

	return ret;
}

static void dsi_phy_toggle_dln3_tx_dctrl(struct dsi_phy_regs *phy_reg)
{
	uint32_t reg_val = 0;

	reg_val = read32(phy_reg->phy_ln_regs[DSI_LANEID_3].dln0_tx_dctrl);

	/* clear bit 0 and keep all other bits including bit 2 */
	reg_val &= ~0x01;

	/* toggle bit 0 */
	write32(phy_reg->phy_ln_regs[DSI_LANEID_3].dln0_tx_dctrl, (0x01 | reg_val));
	write32(phy_reg->phy_ln_regs[DSI_LANEID_3].dln0_tx_dctrl, 0x4);
}

static void dsi_phy_pll_set_source(struct dsi_phy_regs *phy_reg)
{
	uint32_t clk_cfg = read32(phy_reg->phy_cmn_clk_cfg1);
	uint32_t dsi_clksel = 1;

	clk_cfg &= ~0x03;
	clk_cfg |= ((dsi_clksel) & 0x3);

	/* clk cfg1 */
	write32(phy_reg->phy_cmn_clk_cfg1, clk_cfg);
}

static void dsi_phy_pll_bias_enable(bool enable)
{
	uint32_t reg_val = 0;

	/* Set BIAS_EN_MUX, BIAS_EN */
	if (enable)
		reg_val = (0x01 << 6) | (0x01 << 7);

	/* pll system muxes */
	write32(&phy_pll_qlink->pll_system_muxes, reg_val);
}

static void dsi_phy_mnd_divider(struct dsi_phy_configtype *phy_cfg)
{
	uint32_t m_val = 1;
	uint32_t n_val = 1;

	if (phy_cfg->bits_per_pixel == 18) {
		switch (phy_cfg->num_data_lanes) {
		case 1:
		case 2:
			m_val = 2;
			n_val = 3;
			break;
		case 4:
			m_val = 4;
			n_val = 9;
			break;
		default:
			break;
		}
	} else if ((phy_cfg->bits_per_pixel == 16) &&
			(phy_cfg->num_data_lanes == 3)) {
		m_val = 3;
		n_val = 8;
	} else if ((phy_cfg->bits_per_pixel == 30) &&
			(phy_cfg->num_data_lanes == 4)) {
		m_val = 2;
		n_val = 3;
	}

	/*Save M/N info */
	phy_cfg->pclk_divnumerator = m_val;
	phy_cfg->pclk_divdenominator = n_val;
}

static uint32_t dsi_phy_dsiclk_divider(struct dsi_phy_configtype *phy_cfg)
{
	uint32_t m_val = phy_cfg->pclk_divnumerator;
	uint32_t n_val = phy_cfg->pclk_divdenominator;
	uint32_t div_ctrl = 0;

	div_ctrl = (m_val * phy_cfg->bits_per_pixel) /
	       (n_val * phy_cfg->num_data_lanes * 2);

	return div_ctrl;
}


static unsigned long dsi_phy_calc_clk_divider(struct dsi_phy_regs *phy_reg,
						struct dsi_phy_configtype *phy_cfg)
{
	bool div_found = false;
	uint32_t div_ctrl = 0;
	uint32_t reg_val = 0;
	uint32_t pll_post_div = 0;
	uint32_t phy_post_div = 0;
	uint64_t vco_freq_hz = 0;
	uint64_t fval = 0;
	uint64_t pll_output_freq_hz;
	uint64_t desired_bitclk_hz;
	uint64_t min_vco_freq_hz = 0;
	uint32_t lut_max;
	int i;
	struct dsi_phy_divider_lut_entry_type *lut;

	/* use 1000Mhz */
	min_vco_freq_hz = (HAL_DSI_PLL_VCO_MIN_MHZ_2_2_0 * 1000000);

	dsi_phy_mnd_divider(phy_cfg);

	/* Desired clock in MHz */
	desired_bitclk_hz = (uint64_t)phy_cfg->desired_bitclk_freq;

	/* D Phy */
	lut = pll_dividerlut_dphy;
	lut_max = ARRAY_SIZE(pll_dividerlut_dphy);
	lut += (lut_max - 1);

	/* PLL Post Div - from LUT
	 * Check the LUT in reverse order
	 */
	for (i = lut_max - 1; i >= 0; i--, lut--) {
		fval = (uint64_t)lut->phy_post_div *
				(uint64_t)lut->pll_post_div;
		if (fval) {
			if ((desired_bitclk_hz * fval) > min_vco_freq_hz) {
				/* Range found */
				pll_post_div = lut->pll_post_div;
				phy_post_div = lut->phy_post_div;
				div_found = true;
				break;
			}
		}
	}

	if (div_found) {
		phy_cfg->pll_post_div = pll_post_div;
		phy_cfg->phy_post_div = phy_post_div;

		/*div_ctrl_7_4 */
		div_ctrl = dsi_phy_dsiclk_divider(phy_cfg);

		/* DIV_CTRL_7_4 DIV_CTRL_3_0
		 * (DIV_CTRL_3_0 = PHY post divider ratio)
		 */
		reg_val = (div_ctrl << 0x04) & 0xf0;
		reg_val |= (phy_post_div & 0x0f);
		write32(phy_reg->phy_cmn_clk_cfg0, reg_val);

		/* PLL output frequency = desired_bitclk_hz * phy_post_div */
		pll_output_freq_hz = desired_bitclk_hz * phy_post_div;

		/* VCO output freq*/
		vco_freq_hz = pll_output_freq_hz * pll_post_div;
	}

	return (unsigned long)vco_freq_hz;
}

static void dsi_phy_pll_outputdiv_rate(struct dsi_phy_configtype *pll_cfg)
{
	/* Output divider */
	uint32_t pll_post_div = 0;
	uint32_t reg_val = 0;

	pll_post_div = log2(pll_cfg->pll_post_div);
	reg_val = pll_post_div & 0x3;
	write32(&phy_pll_qlink->pll_outdiv_rate, reg_val);
}

static int dsi_phy_pll_calcandcommit(struct dsi_phy_regs *phy_reg,
					struct dsi_phy_configtype *phy_cfg)
{
	unsigned long vco_freq_hz;
	int ret = 0;

	/* validate input parameters */
	if (!phy_cfg) {
		return -1;
	} else if ((phy_cfg->bits_per_pixel != 16) &&
		   (phy_cfg->bits_per_pixel != 18) &&
		   (phy_cfg->bits_per_pixel != 24)) {
		/* Unsupported pixel bit depth */
		return -1;
	} else if ((phy_cfg->num_data_lanes == 0) ||
		   (phy_cfg->num_data_lanes > 4)) {
		/* Illegal number of DSI data lanes */
		return -1;
	}

	vco_freq_hz = dsi_phy_calc_clk_divider(phy_reg, phy_cfg);
	if (!vco_freq_hz) {
		/* bitclock too low  - unsupported */
		printf("vco_freq_hz is 0, unsupported\n");
		return -1;
	}

	/* Enable PLL bias */
	dsi_phy_pll_bias_enable(true);

	/* Set byte clk source */
	dsi_phy_pll_set_source(phy_cfg);

	dsi_phy_pll_outputdiv_rate(phy_cfg);
	dsi_phy_pll_vco_10nm_set_rate(vco_freq_hz);
	dsi_phy_toggle_dln3_tx_dctrl(phy_reg);

	/* Steps 6,7 Start PLL & Lock */
	if (ret == 0)
		ret = dsi_phy_pll_lock_detect(phy_reg);

	/* Step 8 - Resync Data Paths */
	if (ret == 0) {
		/* Global clock enable */
		dsi_phy_pll_global_clk_enable(phy_cfg, true);

		/* Resync FIFOs */
		dsi_phy_resync_fifo(phy_reg);
	}

	return ret;
}

static uint32_t dsi_calc_desired_bitclk(struct edid *edid, uint32_t num_lines, uint32_t bpp)
{
	uint64_t desired_bclk = 0;
	uint32_t pixel_clock_in_hz;

	pixel_clock_in_hz = edid->mode.pixel_clock * KHz;
	if (num_lines) {
		desired_bclk = pixel_clock_in_hz * (uint64_t)bpp;
		desired_bclk = desired_bclk/(uint64_t)(num_lines);
	}

	printf("Desired bitclock: %uHz\n", (uint32_t)desired_bclk);
	return (uint32_t)desired_bclk;
}

static int mdss_dsi_phy_pll_setup(struct edid *edid,
					struct dsi_phy_regs *phy_reg,
					uint32_t num_of_lanes, uint32_t bpp)
{
	struct dsi_phy_configtype phy_cfg;
	struct msm_dsi_phy_ctrl phy_timings;
	int ret;

	/* Setup the PhyStructure */
	memset(&phy_cfg, 0, sizeof(struct dsi_phy_configtype));
	memset(&phy_timings, 0, sizeof(struct msm_dsi_phy_ctrl));

	phy_cfg.bits_per_pixel = bpp;
	phy_cfg.num_data_lanes = num_of_lanes;

	/* desired DSI PLL bit clk freq in Hz */
	phy_cfg.desired_bitclk_freq = dsi_calc_desired_bitclk(edid, num_of_lanes, bpp);

	ret = dsi_phy_pll_calcandcommit(phy_reg, &phy_cfg);
	if (ret)
		return ret;
	mdss_dsi_calculate_phy_timings(&phy_timings, &phy_cfg);
	ret = mdss_dsi_phy_timings(&phy_timings);

	return ret;
}

int mdss_dsi_phy_10nm_init(struct edid *edid, struct dsi_phy_regs *phy_reg,
			   uint32_t num_of_lanes, uint32_t bpp)
{
	int ret;

	/* Phy set up */
	mdss_dsi_phy_setup();
	ret = mdss_dsi_phy_pll_setup(edid, phy_reg, num_of_lanes, bpp);
	enable_dsi_clk(); // TODO: call clk framework api

	return ret;
}
#endif

static void qcom_dsidphy_mipi_mode_enable(struct qcom_dsidphy *qcom)
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
	timings = qcom->pdata->qcom_mipi_dphy_timing_table;

#if 0
	//qcom_dsidphy_pll_calc_rate(qcom, cfg->hs_clk_rate);

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

static int qcom_dsidphy_power_on(struct phy *phy)
{
	struct qcom_dsidphy *qcom = dev_get_priv(phy->dev);

return 0;
	clk_prepare_enable(qcom->ahb_clk);
	//clk_prepare_enable(qcom->ref_clk);

	qcom_dsidphy_mipi_mode_enable(qcom);

	return 0;
}

static int qcom_dsidphy_power_off(struct phy *phy)
{
	struct qcom_dsidphy *qcom = dev_get_priv(phy->dev);

return 0;
	//clk_disable_unprepare(qcom->ref_clk);
	clk_disable_unprepare(qcom->ahb_clk);

	return 0;
}

static int qcom_dsidphy_configure(struct phy *phy, void *params)
{
	struct qcom_dsidphy *qcom = dev_get_priv(phy->dev);
	struct phy_configure_opts_mipi_dphy *config = params;
	int ret;

return 0;
	ret = phy_mipi_dphy_config_validate(config);
	if (ret)
		return ret;

	memcpy(&qcom->dphy_cfg, config, sizeof(qcom->dphy_cfg));

	return 0;
}

static const struct phy_ops qcom_dsidphy_ops = {
	.configure = qcom_dsidphy_configure,
	.power_on = qcom_dsidphy_power_on,
	.power_off = qcom_dsidphy_power_off,
};

static int qcom_dsidphy_probe(struct udevice *dev)
{
	struct qcom_dsidphy *qcom = dev_get_priv(dev);
	int ret;

	qcom->dev = dev;
	qcom->pdata = (const struct qcom_video_phy_plat_data *)dev_get_driver_data(dev);

	qcom->base = dev_remap_addr_name(dev, "dsi_phy");
	if (IS_ERR(qcom->base))
		return PTR_ERR(qcom->base);

	qcom->pll_base = dev_remap_addr_name(dev, "dsi_pll");
	if (IS_ERR(qcom->pll_base))
		return PTR_ERR(qcom->pll_base);

	qcom->lane_base = dev_remap_addr_name(dev, "dsi_phy_lane");
	if (IS_ERR(qcom->lane_base))
		return PTR_ERR(qcom->lane_base);

printf("========%s udev %p base %p pll_base %p lane_base %p\n", __func__, dev, qcom->base, qcom->pll_base, qcom->lane_base);

/*
	qcom->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(qcom->ref_clk)) {
		ret = PTR_ERR(qcom->ref_clk);
		dev_err(dev, "failed to get ref clock: %d\n", ret);
		return ret;
	}
*/

	qcom->ahb_clk = devm_clk_get(dev, "iface");
	if (IS_ERR(qcom->ahb_clk)) {
		ret = PTR_ERR(qcom->ahb_clk);
		dev_err(dev, "failed to get phy iface clk: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct udevice_id qcom_dsidphy_of_match[] = {
	{ .compatible = "qcom,sm8650-dsi-phy-4nm" },
	{}
};

U_BOOT_DRIVER(qcom_mdss_dsidphy) = {
	.name = "qcom-dsidphy",
	.id = UCLASS_PHY,
	.of_match = qcom_dsidphy_of_match,
	.probe = qcom_dsidphy_probe,
	.ops = &qcom_dsidphy_ops,
	.priv_auto = sizeof(struct qcom_dsidphy),
};
