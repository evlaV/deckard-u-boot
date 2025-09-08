// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <clk-uclass.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <errno.h>
#include <asm/io.h>
#include <linux/bug.h>
#include <linux/bitops.h>
#include <dt-bindings/clock/qcom,sm8650-dispcc.h>

#include "clock-qcom.h"
#include "clk-alpha-pll.h"

/* Need to match the order of clocks in DT binding */
enum {
	DT_BI_TCXO,
	DT_BI_TCXO_AO,
	DT_AHB_CLK,
	DT_SLEEP_CLK,

	DT_DSI0_PHY_PLL_OUT_BYTECLK,
	DT_DSI0_PHY_PLL_OUT_DSICLK,
	DT_DSI1_PHY_PLL_OUT_BYTECLK,
	DT_DSI1_PHY_PLL_OUT_DSICLK,
};

#define DISP_CC_MISC_CMD	0xF000

enum {
	P_BI_TCXO,
	P_DISP_CC_PLL0_OUT_MAIN,
	P_DISP_CC_PLL1_OUT_EVEN,
	P_DISP_CC_PLL1_OUT_MAIN,
	P_DSI0_PHY_PLL_OUT_BYTECLK,
	P_DSI0_PHY_PLL_OUT_DSICLK,
	P_DSI1_PHY_PLL_OUT_BYTECLK,
	P_DSI1_PHY_PLL_OUT_DSICLK,
	P_SLEEP_CLK,
};

#define XO_BOARD 76800000

static struct pll_vco lucid_ole_vco[] = {
	{ 249600000, 2000000000, 0 },
};

static struct alpha_pll_config disp_cc_pll0_config = {
	.l = 0xd,
	.alpha = 0x6492,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x82aa299c,
	.test_ctl_val = 0x00000000,
	.test_ctl_hi_val = 0x00000003,
	.test_ctl_hi1_val = 0x00009000,
	.test_ctl_hi2_val = 0x00000034,
	.user_ctl_val = 0x00000000,
	.user_ctl_hi_val = 0x00000005,
};

static struct clk_alpha_pll disp_cc_pll0 = {
	.name = "disp_cc_pll0",
	.offset = 0x0,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
};

static struct alpha_pll_config disp_cc_pll1_config = {
	.l = 0x1f,
	.alpha = 0x4000,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x82aa299c,
	.test_ctl_val = 0x00000000,
	.test_ctl_hi_val = 0x00000003,
	.test_ctl_hi1_val = 0x00009000,
	.test_ctl_hi2_val = 0x00000034,
	.user_ctl_val = 0x00000000,
	.user_ctl_hi_val = 0x00000005,
};

static struct clk_alpha_pll disp_cc_pll1 = {
	.name = "disp_cc_pll1",
	.offset = 0x1000,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
};

/* We need to look up the cfg val from parent_map with the src index from
 * freq table in Linux to merge the 2 tables into CFG_xx in 1 table in u-boot */
#define CFG_BI_TCXO			(0 << 8)
#define CFG_DISP_CC_PLL1_OUT_MAIN	(4 << 8)
static const struct freq_tbl ftbl_disp_cc_mdss_ahb_clk_src[] = {
	F(19200000, CFG_BI_TCXO, 1, 0, 0),
	F(37500000, CFG_DISP_CC_PLL1_OUT_MAIN, 16, 0, 0),
	F(75000000, CFG_DISP_CC_PLL1_OUT_MAIN, 8, 0, 0),
	{ }
};

/* hard code per dual-dsi case */
#define CFG_DSI0_PHY_PLL_OUT_DSICLK (1 << 8)
#define CFG_DSI0_PHY_PLL_OUT_BYTECLK (2 << 8)
#define CFG_DSI1_PHY_PLL_OUT_DSICLK (3 << 8)
#define CFG_DSI1_PHY_PLL_OUT_BYTECLK (4 << 8)

#define CFG_DISP_CC_PLL0_OUT_MAIN (1 << 8)
static const struct freq_tbl ftbl_disp_cc_mdss_mdp_clk_src[] = {
	F(19200000, CFG_BI_TCXO, 1, 0, 0),
	F(85714286, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(100000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(150000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(172000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(200000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(325000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(375000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(514000000, CFG_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(690000000, CFG_DISP_CC_PLL0_OUT_MAIN, 2, 0, 0),
	{ }
};

static const struct gate_clk sm8650_disp_clks[] = {
	GATE_CLK(DISP_CC_MDSS_AHB_CLK,		0x80a4, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_BYTE0_CLK,	0x8028, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_BYTE0_INTF_CLK,	0x802c, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_BYTE1_CLK,	0x8030, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_BYTE1_INTF_CLK,	0x8034, BIT(0)),

	GATE_CLK(DISP_CC_MDSS_MDP_CLK,		0x800c, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_MDP_LUT_CLK,	0x8018, BIT(0)),

	GATE_CLK(DISP_CC_MDSS_VSYNC_CLK,	0x8024, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_PCLK0_CLK,	0x8004, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_PCLK1_CLK,	0x8008, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_ESC0_CLK,		0x8038, BIT(0)),
	GATE_CLK(DISP_CC_MDSS_ESC1_CLK,		0x803c, BIT(0)),
};

static int dispcc_enable_src(const struct freq_tbl *freq)
{
	switch (freq->src) {
		// RPMh CXO already enabled :3
		case CFG_BI_TCXO:
			return 0;
		case CFG_DISP_CC_PLL0_OUT_MAIN:
			// bi_tcxo_div2 clock is routed to PLL0
			// 1542000000 from measurements?
			if (alpha_trion_pll_set_rate(&disp_cc_pll0, 1542000000, XO_BOARD / 4))
				printf("Failed to set PLL0 rate!\n");
			if (alpha_pll_lucid_evo_enable(&disp_cc_pll0))
				printf("Failed to enable PLL0\n");
			return 0;
		case CFG_DISP_CC_PLL1_OUT_MAIN:
			if (alpha_trion_pll_set_rate(&disp_cc_pll1, 1542000000, XO_BOARD / 4))
				printf("Failed to set PLL0 rate!\n");
			if (alpha_pll_lucid_evo_enable(&disp_cc_pll1))
				printf("Failed to enable PLL0\n");
			return 0;
		default:
			printf("%s: Unsupported PLL source %d\n", __func__, freq->src);
			return -EINVAL;
	}
}

static ulong sm8650_disp_cc_set_rate(struct clk *clk, ulong rate)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);
	const struct freq_tbl *freq;
	const char *name;

	if (clk->id < ARRAY_SIZE(sm8650_disp_clks))
		name = sm8650_disp_clks[clk->id].name;
	else
		name = "unknown";

	dev_dbg(clk->dev, "set clk %s (%lu) rate: %lu\n", name, clk->id, rate);

	switch (clk->id) {
	case DISP_CC_MDSS_AHB_CLK:
		freq = qcom_find_freq(ftbl_disp_cc_mdss_ahb_clk_src, rate);
		dispcc_enable_src(freq);
		dev_dbg(clk->dev, "Picked rate %llu for DISP_CC_MDSS_AHB_CLK\n", freq->freq);
		clk_rcg_set_rate(priv->base, 0x82e8, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_BYTE0_INTF_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_byte0_div_clk_src:
		 * .reg = 0x8120, .shift = 0, .width = 4 with test. Due to DSC ?
		 * Or we just fall thru into the grand parent disp_cc_mdss_byte0_clk_src
		 * at 0x8108? */
		writel(0, priv->base + 0x8120);
	case DISP_CC_MDSS_BYTE0_CLK:
		/* There is only 1 entry in original ftbl_disp_cc_mdss_byte0_clk_src,
		 *while parent is dsi0_phy_pll_out_byteclk with rate 121574874 in
		 * kernel test, because re-parent can link it to dsi phy. We just
		 * force the re-parent with the table.
		 */
		clk_rcg_set_rate(priv->base, 0x8108, 1, CFG_DSI0_PHY_PLL_OUT_BYTECLK);
		return rate;

	case DISP_CC_MDSS_BYTE1_INTF_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_byte1_div_clk_src:
		 * .reg = 0x813c, .shift = 0, .width = 4 with test. Due to DSC ?
		 * Or we just fall thru into the grand parent disp_cc_mdss_byte1_clk_src
		 * at 0x8124 */
		writel(0, priv->base + 0x813c);
	case DISP_CC_MDSS_BYTE1_CLK:
		/* Its parent is disp_cc_mdss_byte1_clk_src:
		 * .cmd_rcgr = 0x8124, .mnd_width = 0, .hid_width = 5,
		 * The rate of it and grand parent's(dsi0_phy_pll_out_byteclk) rate
		 * are both 121574874 in kernel test.
		 * Force with the table to re-parent to dsi phy.
		 */
		clk_rcg_set_rate(priv->base, 0x8124, 1, CFG_DSI1_PHY_PLL_OUT_BYTECLK);
		return rate;

	case DISP_CC_MDSS_MDP_LUT_CLK:
		/* Its parent is disp_cc_mdss_mdp_clk_src at 0x80d8:
		 * .cmd_rcgr = 0x80d8, .mnd_width = 0, .hid_width = 5,
		 * So fall thru to DISP_CC_MDSS_MDP_CLK.
		 */
	case DISP_CC_MDSS_MDP_CLK:
		freq = qcom_find_freq(ftbl_disp_cc_mdss_mdp_clk_src, rate);
		dispcc_enable_src(freq);
		/* Per test, it is 514000000 and grand parent disp_cc_pll0 rate should
		 * be 1542000000 */
		clk_rcg_set_rate(priv->base, 0x80d8, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_VSYNC_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_vsync_clk_src :
		 * .cmd_rcgr = 0x80f0, .mnd_width = 0, .hid_width = 5,
		 * rate is 19200000, same with its parent bi-tcxo-div2-clk per test.
		 * so ignore this handling.
		 */
		clk_rcg_set_rate(priv->base, 0x80f0, 1, CFG_BI_TCXO);
		return rate;

	case DISP_CC_MDSS_PCLK0_CLK:
		/* The rate of its parent disp_cc_mdss_pclk0_clk_src and grand parent
		 * dsi0_phy_pll_out_dsiclk and GGrand-parent dsi0_pll_post_out_div_clk
		 * rate are 243149748 per test. The GGGrand-parent dsi0_pll_out_div_clk
		 * is 851024121 in 90Hz case, that's m/n = 7/2. Also 7/2 for 80/120Hz
		 * The divider shall happen inside DSI PLL, ignore it here. */
		clk_rcg_set_rate_mnd(priv->base, 0x80a8, 1, 1, 1, CFG_DSI0_PHY_PLL_OUT_DSICLK, 8);
		return rate;

	case DISP_CC_MDSS_PCLK1_CLK:
		/* The rate of its parent is disp_cc_mdss_pclk1_clk_src and grand parent
		 * dsi0_phy_pll_out_dsiclk in dual-dsi case. GGrand-parent dsi0_pll_post_out_div_clk
		 * rate are 243149748 per test. The GGGrand-parent dsi0_pll_out_div_clk
		 * is 851024121 in 90Hz case, that's m/n = 7/2. Also 7/2 for 80/120Hz
		 * The divider shall happen inside DSI PLL, ignore it here. */
		/* TODO: this clk use clk_pixel_ops in Linux, not mnd */
		clk_rcg_set_rate_mnd(priv->base, 0x80c0, 1, 1, 1, CFG_DSI1_PHY_PLL_OUT_DSICLK, 8);
		return rate;

	case DISP_CC_MDSS_ESC0_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_esc0_clk_src:
		 * .cmd_rcgr = 0x8140, .mnd_width = 0, .hid_width = 5,
		 * rate is 19200000, same with its parent bi-tcxo-div2-clk per test.
		 * so ignore this handling. Or to use the table to force the parent.
		 */
		clk_rcg_set_rate(priv->base, 0x8140, 1, CFG_BI_TCXO);
		return rate;

	case DISP_CC_MDSS_ESC1_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_esc0_clk_src:
		 * .cmd_rcgr = 0x8158, .mnd_width = 0, .hid_width = 5,
		 * rate is 19200000, same with its parent bi-tcxo-div2-clk per test.
		 * so ignore this handling. Or to use the table to force the parent.
		 */
		clk_rcg_set_rate(priv->base, 0x8158, 1, CFG_BI_TCXO);
		return rate;

	default:
		return 0;
	}
}

static int sm8650_disp_cc_parent_enable(struct clk *clk, ulong id)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);
	struct clk *parent = &priv->parents_map[id];

	if (clk_valid(parent))
		clk_enable(parent);

	return 0;
}

static int sm8650_disp_cc_enable(struct clk *clk)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);
	const char *name;

	if (clk->id < ARRAY_SIZE(sm8650_disp_clks))
		name = sm8650_disp_clks[clk->id].name;
	else
		name = "unknown";

	switch (clk->id) {
	case DISP_CC_MDSS_AHB_CLK:
		sm8650_disp_cc_set_rate(clk, 19200000);
		break;
	case DISP_CC_MDSS_MDP_CLK:
		sm8650_disp_cc_set_rate(clk, 514000000);
		break;
	case DISP_CC_MDSS_BYTE0_CLK:
		sm8650_disp_cc_parent_enable(clk, DISP_CC_MDSS_BYTE0_CLK_SRC);
		sm8650_disp_cc_set_rate(clk, 121574874);
		break;
	case DISP_CC_MDSS_BYTE1_CLK:
		sm8650_disp_cc_parent_enable(clk, DISP_CC_MDSS_BYTE1_CLK_SRC);
		sm8650_disp_cc_set_rate(clk, 121574874);
		break;
	case DISP_CC_MDSS_BYTE0_INTF_CLK:
		sm8650_disp_cc_parent_enable(clk, DISP_CC_MDSS_BYTE0_CLK_SRC);
		sm8650_disp_cc_set_rate(clk, 121574874);
		break;
	case DISP_CC_MDSS_BYTE1_INTF_CLK:
		sm8650_disp_cc_parent_enable(clk, DISP_CC_MDSS_BYTE1_CLK_SRC);
		sm8650_disp_cc_set_rate(clk, 121574874);
		break;
	case DISP_CC_MDSS_VSYNC_CLK:
		sm8650_disp_cc_set_rate(clk, 19200000);
		break;
	case DISP_CC_MDSS_PCLK0_CLK:
	case DISP_CC_MDSS_PCLK1_CLK:
		sm8650_disp_cc_set_rate(clk, 243149748);
		break;
	case DISP_CC_MDSS_ESC0_CLK:
	case DISP_CC_MDSS_ESC1_CLK:
		sm8650_disp_cc_set_rate(clk, 19200000);
		break;
	default:
		break;
	}

	dev_dbg(clk->dev, "enable clk %s (%lu)\n", name, clk->id);
	qcom_gate_clk_en(priv, clk->id);

	return 0;
}


static const struct qcom_reset_map disp_cc_sm8650_resets[] = {
	[DISP_CC_MDSS_CORE_BCR] = { 0x8000 },
	[DISP_CC_MDSS_CORE_INT2_BCR] = { 0xa000 },
	[DISP_CC_MDSS_RSCC_BCR] = { 0xc000 },
};

static const struct qcom_power_map sm8650_disp_gdscs[] = {
	[MDSS_GDSC] = { 0x9000 },
	[MDSS_INT2_GDSC] = { 0xb000 },
};

#define BASE 0xaf00000

static const phys_addr_t sm8650_gpll_addrs[] = {
	BASE + 0, // DISP_CC_PLL0_OUT_MAIN
	BASE + 0x4000, // DISP_CC_PLL1_OUT_MAIN
};

static const phys_addr_t dbg_rcg_addrs[] = {
	BASE + 0x82e8,
	BASE + 0x80d8,
	BASE + 0x8108,
	BASE + 0x8120,
	BASE + 0x8124,
	BASE + 0x813c,
	BASE + 0x80a8,
	BASE + 0xe03c,
};

static const char *dbg_rcg_names[] = {
	"disp_cc_mdss_ahb_clk_src",
	"disp_cc_mdss_mdp_clk_src",
	"disp_cc_mdss_byte0_clk_src",
	"disp_cc_mdss_byte0_div_clk_src",
	"disp_cc_mdss_byte1_clk_src",
	"disp_cc_mdss_byte1_div_clk_src",
	"disp_cc_mdss_pclk0_clk_src",
	"disp_cc_xo_clk_src",
};

static struct msm_clk_data sm8650_disp_cc_data = {
	.resets = disp_cc_sm8650_resets,
	.num_resets = ARRAY_SIZE(disp_cc_sm8650_resets),
	.clks = sm8650_disp_clks,
	.num_clks = ARRAY_SIZE(sm8650_disp_clks),
	.power_domains = sm8650_disp_gdscs,
	.num_power_domains = ARRAY_SIZE(sm8650_disp_gdscs),

	.dbg_rcg_addrs = dbg_rcg_addrs,
	.dbg_rcg_names = dbg_rcg_names,
	.num_rcgs = ARRAY_SIZE(dbg_rcg_addrs),
	.dbg_pll_addrs = sm8650_gpll_addrs,
	.num_plls = ARRAY_SIZE(sm8650_gpll_addrs),

	.enable = sm8650_disp_cc_enable,
	.set_rate = sm8650_disp_cc_set_rate,
};

#define DISP_CC_MISC_CMD	0xF000

static int dispcc_sm8650_probe(struct udevice *dev)
{
	phys_addr_t base = dev_read_addr(dev);

	// FIXME: avoid passing base around by making it a property of the pll struct
	disp_cc_pll0.base = base;
	disp_cc_pll1.base = base;
	clk_lucid_ole_pll_configure(&disp_cc_pll0, &disp_cc_pll0_config);
	clk_lucid_ole_pll_configure(&disp_cc_pll0, &disp_cc_pll1_config);

	/* Enable clock gating for MDP clocks */
	setbits_32(base + DISP_CC_MISC_CMD, 0x10);

	/* Keep some clocks always on */
	qcom_branch_set_clk_en(base, 0xe054); /* DISP_CC_XO_CLK */
	return 0;
}

static const struct udevice_id disp_cc_sm8650_of_match[] = {
	{	.compatible = "qcom,sm8650-dispcc",
		.data = (ulong)&sm8650_disp_cc_data,
	},
	{ }
};

U_BOOT_DRIVER(disp_cc_sm8650) = {
	.name		= "disp_cc-sm8650",
	.id		= UCLASS_NOP,
	.of_match	= disp_cc_sm8650_of_match,
	.bind		= qcom_cc_bind,
	.probe		= dispcc_sm8650_probe,
	.flags		= DM_FLAG_PRE_RELOC,
};
