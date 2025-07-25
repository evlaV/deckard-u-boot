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

#if 0
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
	.offset = 0x0,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_pll0",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_reset_lucid_ole_ops,  /* is clk_alpha_pll_reset_lucid_evo_ops actually */
		},
	},
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
	.offset = 0x1000,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_pll1",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_reset_lucid_ole_ops,
		},
	},
};

static const struct parent_map disp_cc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data disp_cc_parent_data_0[] = {
	{ .index = DT_BI_TCXO },
};

static const struct clk_parent_data disp_cc_parent_data_0_ao[] = {
	{ .index = DT_BI_TCXO_AO },
};

static const struct parent_map disp_cc_parent_map_1[] = {
	{ P_BI_TCXO, 0 },
	{ P_DP3_PHY_PLL_VCO_DIV_CLK, 3 },
	{ P_DP1_PHY_PLL_VCO_DIV_CLK, 4 },
	{ P_DP2_PHY_PLL_VCO_DIV_CLK, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_1[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DP3_PHY_PLL_VCO_DIV_CLK },
	{ .index = DT_DP1_PHY_PLL_VCO_DIV_CLK },
	{ .index = DT_DP2_PHY_PLL_VCO_DIV_CLK },
};

static const struct parent_map disp_cc_parent_map_2[] = {
	{ P_BI_TCXO, 0 },
	{ P_DSI0_PHY_PLL_OUT_DSICLK, 1 },
	{ P_DSI0_PHY_PLL_OUT_BYTECLK, 2 },
	{ P_DSI1_PHY_PLL_OUT_DSICLK, 3 },
	{ P_DSI1_PHY_PLL_OUT_BYTECLK, 4 },
};

static const struct clk_parent_data disp_cc_parent_data_2[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DSI0_PHY_PLL_OUT_DSICLK },
	{ .index = DT_DSI0_PHY_PLL_OUT_BYTECLK },
	{ .index = DT_DSI1_PHY_PLL_OUT_DSICLK },
	{ .index = DT_DSI1_PHY_PLL_OUT_BYTECLK },
};

static const struct parent_map disp_cc_parent_map_3[] = {
	{ P_BI_TCXO, 0 },
	{ P_DP1_PHY_PLL_LINK_CLK, 2 },
	{ P_DP2_PHY_PLL_LINK_CLK, 3 },
	{ P_DP3_PHY_PLL_LINK_CLK, 4 },
};

static const struct clk_parent_data disp_cc_parent_data_3[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DP1_PHY_PLL_LINK_CLK },
	{ .index = DT_DP2_PHY_PLL_LINK_CLK },
	{ .index = DT_DP3_PHY_PLL_LINK_CLK },
};

static const struct parent_map disp_cc_parent_map_4[] = {
	{ P_BI_TCXO, 0 },
	{ P_DP0_PHY_PLL_LINK_CLK, 1 },
	{ P_DP0_PHY_PLL_VCO_DIV_CLK, 2 },
	{ P_DP3_PHY_PLL_VCO_DIV_CLK, 3 },
	{ P_DP1_PHY_PLL_VCO_DIV_CLK, 4 },
	{ P_DP2_PHY_PLL_VCO_DIV_CLK, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_4[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DP0_PHY_PLL_LINK_CLK },
	{ .index = DT_DP0_PHY_PLL_VCO_DIV_CLK },
	{ .index = DT_DP3_PHY_PLL_VCO_DIV_CLK },
	{ .index = DT_DP1_PHY_PLL_VCO_DIV_CLK },
	{ .index = DT_DP2_PHY_PLL_VCO_DIV_CLK },
};

static const struct parent_map disp_cc_parent_map_5[] = {
	{ P_BI_TCXO, 0 },
	{ P_DSI0_PHY_PLL_OUT_BYTECLK, 2 },
	{ P_DSI1_PHY_PLL_OUT_BYTECLK, 4 },
};

static const struct clk_parent_data disp_cc_parent_data_5[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DSI0_PHY_PLL_OUT_BYTECLK },
	{ .index = DT_DSI1_PHY_PLL_OUT_BYTECLK },
};

static const struct parent_map disp_cc_parent_map_6[] = {
	{ P_BI_TCXO, 0 },
	{ P_DISP_CC_PLL1_OUT_MAIN, 4 },
	{ P_DISP_CC_PLL1_OUT_EVEN, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_6[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &disp_cc_pll1.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
};

static const struct parent_map disp_cc_parent_map_7[] = {
	{ P_BI_TCXO, 0 },
	{ P_DP0_PHY_PLL_LINK_CLK, 1 },
	{ P_DP1_PHY_PLL_LINK_CLK, 2 },
	{ P_DP2_PHY_PLL_LINK_CLK, 3 },
	{ P_DP3_PHY_PLL_LINK_CLK, 4 },
};

static const struct clk_parent_data disp_cc_parent_data_7[] = {
	{ .index = DT_BI_TCXO },
	{ .index = DT_DP0_PHY_PLL_LINK_CLK },
	{ .index = DT_DP1_PHY_PLL_LINK_CLK },
	{ .index = DT_DP2_PHY_PLL_LINK_CLK },
	{ .index = DT_DP3_PHY_PLL_LINK_CLK },
};

static const struct parent_map disp_cc_parent_map_8[] = {
	{ P_BI_TCXO, 0 },
	{ P_DISP_CC_PLL0_OUT_MAIN, 1 },
	{ P_DISP_CC_PLL1_OUT_MAIN, 4 },
	{ P_DISP_CC_PLL1_OUT_EVEN, 6 },
};

static const struct clk_parent_data disp_cc_parent_data_8[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &disp_cc_pll0.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
	{ .hw = &disp_cc_pll1.clkr.hw },
};

static const struct parent_map disp_cc_parent_map_9[] = {
	{ P_SLEEP_CLK, 0 },
};

static const struct clk_parent_data disp_cc_parent_data_9[] = {
	{ .index = DT_SLEEP_CLK },
};

static const struct freq_tbl ftbl_disp_cc_mdss_ahb_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	F(37500000, P_DISP_CC_PLL1_OUT_MAIN, 16, 0, 0),
	F(75000000, P_DISP_CC_PLL1_OUT_MAIN, 8, 0, 0),
	{ },
};

/* there is only 1 entry in ftbl_disp_cc_mdss_byte0_clk_src, why need 4 entry in disp_cc_parent_map_2 */
static struct clk_rcg2 disp_cc_mdss_byte0_clk_src = {
	.cmd_rcgr = 0x8108,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte0_clk_src",
		.parent_data = disp_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_byte2_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_byte1_clk_src = {
	.cmd_rcgr = 0x8124,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte1_clk_src",
		.parent_data = disp_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_byte2_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_esc0_clk_src = {
	.cmd_rcgr = 0x8140,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_5,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_esc0_clk_src",
		.parent_data = disp_cc_parent_data_5,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_5),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_esc1_clk_src = {
	.cmd_rcgr = 0x8158,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_5,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_esc1_clk_src",
		.parent_data = disp_cc_parent_data_5,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_5),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_mdss_mdp_clk_src_sar2130p[] = {
	F(200000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(325000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(514000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_disp_cc_mdss_mdp_clk_src_sm8650[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	F(85714286, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(100000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(150000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(200000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(325000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(402000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	F(514000000, P_DISP_CC_PLL0_OUT_MAIN, 3, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_mdss_mdp_clk_src = {
	.cmd_rcgr = 0x80d8,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_8,
	.freq_tbl = ftbl_disp_cc_mdss_mdp_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_mdp_clk_src",
		.parent_data = disp_cc_parent_data_8,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_8),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_pclk0_clk_src = {
	.cmd_rcgr = 0x80a8,
	.mnd_width = 8,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_pclk0_clk_src",
		.parent_data = disp_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_pixel_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_pclk1_clk_src = {
	.cmd_rcgr = 0x80c0,
	.mnd_width = 8,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_2,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_pclk1_clk_src",
		.parent_data = disp_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_pixel_ops,
	},
};

static struct clk_rcg2 disp_cc_mdss_vsync_clk_src = {
	.cmd_rcgr = 0x80f0,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_0,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_vsync_clk_src",
		.parent_data = disp_cc_parent_data_0,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static const struct freq_tbl ftbl_disp_cc_sleep_clk_src[] = {
	F(32000, P_SLEEP_CLK, 1, 0, 0),
	{ }
};

static struct clk_rcg2 disp_cc_sleep_clk_src = {
	.cmd_rcgr = 0xe05c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_9,
	.freq_tbl = ftbl_disp_cc_sleep_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_sleep_clk_src",
		.parent_data = disp_cc_parent_data_9,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_9),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_rcg2 disp_cc_xo_clk_src = {
	.cmd_rcgr = 0xe03c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = disp_cc_parent_map_0,
	.freq_tbl = ftbl_disp_cc_mdss_byte0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_xo_clk_src",
		.parent_data = disp_cc_parent_data_0_ao,
		.num_parents = ARRAY_SIZE(disp_cc_parent_data_0_ao),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_ops,
	},
};

static struct clk_regmap_div disp_cc_mdss_byte0_div_clk_src = {
	.reg = 0x8120,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte0_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&disp_cc_mdss_byte0_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_regmap_div disp_cc_mdss_byte1_div_clk_src = {
	.reg = 0x813c,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "disp_cc_mdss_byte1_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&disp_cc_mdss_byte1_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.ops = &clk_regmap_div_ops,
	},
};

static struct clk_branch disp_cc_mdss_byte0_clk = {
	.halt_reg = 0x8028,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8028,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_byte0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_byte0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_byte1_clk = {
	.halt_reg = 0x8030,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8030,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_byte1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_byte1_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_byte1_intf_clk = {
	.halt_reg = 0x8034,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8034,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_byte1_intf_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_byte1_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_esc0_clk = {
	.halt_reg = 0x8038,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8038,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_esc0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_esc0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_esc1_clk = {
	.halt_reg = 0x803c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x803c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_esc1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_esc1_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp_clk = {
	.halt_reg = 0x800c,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x800c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_mdp_lut_clk = {
	.halt_reg = 0x8018,
	.halt_check = BRANCH_HALT_VOTED,
	.clkr = {
		.enable_reg = 0x8018,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_mdp_lut_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_mdp_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_pclk0_clk = {
	.halt_reg = 0x8004,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8004,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_pclk0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_pclk0_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_pclk1_clk = {
	.halt_reg = 0x8008,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8008,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_pclk1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_pclk1_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_mdss_vsync_clk = {
	.halt_reg = 0x8024,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8024,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_mdss_vsync_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_mdss_vsync_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch disp_cc_sleep_clk = {
	.halt_reg = 0xe074,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0xe074,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "disp_cc_sleep_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&disp_cc_sleep_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_regmap *disp_cc_sm8550_clocks[] = {
y	[DISP_CC_MDSS_BYTE0_CLK _SRC] = &disp_cc_mdss_byte0_clk_src.clkr,

y	[DISP_CC_MDSS_BYTE0_INTF_CLK] = &disp_cc_mdss_byte0_intf_clk.clkr, <-disp_cc_mdss_byte0_div_clk_src:DISP_CC_MDSS_BYTE0_DIV_CLK_SRC
y	[DISP_CC_MDSS_BYTE1_CLK] = &disp_cc_mdss_byte1_clk.clkr, <- disp_cc_mdss_byte1_clk_src
y	[DISP_CC_MDSS_BYTE1_CLK _SRC] = &disp_cc_mdss_byte1_clk_src.clkr,
	[DISP_CC_MDSS_BYTE1_DIV_CLK_SRC] = &disp_cc_mdss_byte1_div_clk_src.clkr,
y	[DISP_CC_MDSS_BYTE1_INTF_CLK] = &disp_cc_mdss_byte1_intf_clk.clkr, <-disp_cc_mdss_byte1_div_clk_src:DISP_CC_MDSS_BYTE1_DIV_CLK_SRC
y	[DISP_CC_MDSS_ESC0_CLK] = &disp_cc_mdss_esc0_clk.clkr, <=disp_cc_mdss_esc0_clk_src
	[DISP_CC_MDSS_ESC0_CLK_SRC] = &disp_cc_mdss_esc0_clk_src.clkr,
y	[DISP_CC_MDSS_ESC1_CLK] = &disp_cc_mdss_esc1_clk.clkr, <-disp_cc_mdss_esc1_clk_src
	[DISP_CC_MDSS_ESC1_CLK_SRC] = &disp_cc_mdss_esc1_clk_src.clkr,
y	[DISP_CC_MDSS_MDP_CLK] = &disp_cc_mdss_mdp_clk.clkr, <= disp_cc_mdss_mdp_clk_src
	[DISP_CC_MDSS_MDP_CLK_SRC] = &disp_cc_mdss_mdp_clk_src.clkr,
y	[DISP_CC_MDSS_MDP_LUT_CLK] = &disp_cc_mdss_mdp_lut_clk.clkr, <= disp_cc_mdss_mdp_clk_src
y	[DISP_CC_MDSS_PCLK0_CLK] = &disp_cc_mdss_pclk0_clk.clkr, <= disp_cc_mdss_pclk0_clk_src
y	[DISP_CC_MDSS_PCLK0_CLK_SRC] = &disp_cc_mdss_pclk0_clk_src.clkr,
y	[DISP_CC_MDSS_PCLK1_CLK] = &disp_cc_mdss_pclk1_clk.clkr, <= disp_cc_mdss_pclk1_clk_src
y	[DISP_CC_MDSS_PCLK1_CLK_SRC] = &disp_cc_mdss_pclk1_clk_src.clkr,
y	[DISP_CC_MDSS_VSYNC_CLK] = &disp_cc_mdss_vsync_clk.clkr, <= disp_cc_mdss_vsync_clk_src
	[DISP_CC_MDSS_VSYNC_CLK_SRC] = &disp_cc_mdss_vsync_clk_src.clkr,
	[DISP_CC_PLL0] = &disp_cc_pll0.clkr, <= DT_BI_TCXO /* it is needed by disp_cc_mdss_mdp_clk@0x800c */
	[DISP_CC_PLL1] = &disp_cc_pll1.clkr, <= DT_BI_TCXO
	[DISP_CC_SLEEP_CLK] = &disp_cc_sleep_clk.clkr, /* not used */
	[DISP_CC_SLEEP_CLK_SRC] = &disp_cc_sleep_clk_src.clkr, /* not used */
	[DISP_CC_XO_CLK_SRC] = &disp_cc_xo_clk_src.clkr, /* not used */
};

static const struct regmap_config disp_cc_sm8550_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x11008,
	.fast_io = true,
};
#endif

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

static const struct freq_tbl ftbl_disp_cc_mdss_vsync_clk_src[] = {
	F(19200000, CFG_BI_TCXO, 1, 0, 0),
};

/* hard code per dual-dsi case */
#define CFG_DSI0_PHY_PLL_OUT_DSICLK (1 << 8)
#define CFG_DSI0_PHY_PLL_OUT_BYTECLK (2 << 8)
#define CFG_DSI1_PHY_PLL_OUT_BYTECLK (4 << 8)
static const struct freq_tbl ftbl_disp_cc_mdss_byte0_clk_src[] = {
	F(121574874,  CFG_DSI0_PHY_PLL_OUT_BYTECLK, 1, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_disp_cc_mdss_pclk0_clk_src[] = {
	F(243149748,  CFG_DSI0_PHY_PLL_OUT_DSICLK, 1, 0, 0),
};

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

static ulong sm8650_disp_cc_set_rate(struct clk *clk, ulong rate)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);
	const struct freq_tbl *freq;

	dev_err(clk->dev, "set clk %d rate: %d\n", clk->id, rate);

	switch (clk->id) {
	case DISP_CC_MDSS_AHB_CLK:
		freq = qcom_find_freq(ftbl_disp_cc_mdss_ahb_clk_src, rate);
		clk_rcg_set_rate(priv->base, 0x82e8, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_BYTE0_INTF_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_byte0_div_clk_src:
		 * .reg = 0x8120, .shift = 0, .width = 4 with test. Due to DSC ?
		 * Or we just fall thru into the grand parent disp_cc_mdss_byte0_clk_src
		 * at 0x8108? */
		clk_rcg_set_rate_div(priv->base, 0x8120, 4, 0);
	case DISP_CC_MDSS_BYTE0_CLK:
		/* There is only 1 entry in original ftbl_disp_cc_mdss_byte0_clk_src,
		 *while parent is dsi0_phy_pll_out_byteclk with rate 121574874 in
		 * kernel test, because re-parent can link it to dsi phy. We just
		 * force the re-parent with the table.
		 */
		freq = qcom_find_freq(ftbl_disp_cc_mdss_byte0_clk_src, rate);
		clk_rcg_set_rate(priv->base, 0x8108, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_BYTE1_INTF_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_byte1_div_clk_src:
		 * .reg = 0x813c, .shift = 0, .width = 4 with test. Due to DSC ?
		 * Or we just fall thru into the grand parent disp_cc_mdss_byte1_clk_src
		 * at 0x8124 */
		clk_rcg_set_rate_div(priv->base, 0x813c, 4, 0);
	case DISP_CC_MDSS_BYTE1_CLK:
		/* Its parent is disp_cc_mdss_byte1_clk_src:
		 * .cmd_rcgr = 0x8124, .mnd_width = 0, .hid_width = 5,
		 * The rate of it and grand parent's(dsi0_phy_pll_out_byteclk) rate
		 * are both 121574874 in kernel test.
		 * Force with the table to re-parent to dsi phy.
		 */
		freq = qcom_find_freq(ftbl_disp_cc_mdss_byte0_clk_src, rate);
		clk_rcg_set_rate(priv->base, 0x8124, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_MDP_LUT_CLK:
		/* Its parent is disp_cc_mdss_mdp_clk_src at 0x80d8:
		 * .cmd_rcgr = 0x80d8, .mnd_width = 0, .hid_width = 5,
		 * So fall thru to DISP_CC_MDSS_MDP_CLK.
		 */
	case DISP_CC_MDSS_MDP_CLK:
		freq = qcom_find_freq(ftbl_disp_cc_mdss_mdp_clk_src, rate);
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
		freq = qcom_find_freq(ftbl_disp_cc_mdss_vsync_clk_src, rate);
		clk_rcg_set_rate(priv->base, 0x80f0, freq->pre_div, freq->src);
		return freq->freq;

	case DISP_CC_MDSS_PCLK0_CLK:
		/* The rate of its parent disp_cc_mdss_pclk0_clk_src and grand parent
		 * dsi0_phy_pll_out_dsiclk and GGrand-parent dsi0_pll_post_out_div_clk
		 * rate are 243149748 per test. The GGGrand-parent dsi0_pll_out_div_clk
		 * is 851024121 */
		/* TODO: this clk use clk_pixel_ops in Linux, not mnd */
		freq = qcom_find_freq(ftbl_disp_cc_mdss_pclk0_clk_src, rate);
		clk_rcg_set_rate_mnd(priv->base, 0x80a8,
				     freq->pre_div, freq->m, freq->n, freq->src, 8);
		printf("!!!!!!!!!!! fix clock DISP_CC_MDSS_PCLK0_CLK!!!!!!!!!!\n");
		return 0;

	case DISP_CC_MDSS_ESC0_CLK:
		/* Assume the divider is 1 in parent disp_cc_mdss_esc0_clk_src:
		 * .cmd_rcgr = 0x8140, .mnd_width = 0, .hid_width = 5,
		 * rate is 19200000, same with its parent bi-tcxo-div2-clk per test.
		 * so ignore this handling. Or to use the table to force the parent.
		 */
		freq = qcom_find_freq(ftbl_disp_cc_mdss_vsync_clk_src, rate);
		clk_rcg_set_rate(priv->base, 0x8140, freq->pre_div, freq->src);
		return freq->freq;
	default:
		return 0;
	}
}

static int sm8650_disp_cc_enable(struct clk *clk)
{
	struct msm_clk_priv *priv = dev_get_priv(clk->dev);

/*
	switch (clk->id) {
	case GCC_AGGRE_USB3_PRIM_AXI_CLK:
		qcom_gate_clk_en(priv, GCC_USB30_PRIM_MASTER_CLK);
		fallthrough;
	case GCC_USB30_PRIM_MASTER_CLK:
		qcom_gate_clk_en(priv, GCC_USB3_PRIM_PHY_AUX_CLK);
		qcom_gate_clk_en(priv, GCC_USB3_PRIM_PHY_COM_AUX_CLK);
		break;
	}
*/

	dev_err(clk->dev, "enable clk %d\n", clk->id);
	qcom_gate_clk_en(priv, clk->id);

	return 0;
}

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
	GATE_CLK(DISP_CC_MDSS_ESC0_CLK,		0x8038, BIT(0)),
};


static const struct qcom_reset_map disp_cc_sm8550_resets[] = {
	[DISP_CC_MDSS_CORE_BCR] = { 0x8000 },
	[DISP_CC_MDSS_CORE_INT2_BCR] = { 0xa000 },
	[DISP_CC_MDSS_RSCC_BCR] = { 0xc000 },
};

static const struct qcom_power_map sm8650_disp_gdscs[] = {
	[MDSS_GDSC] = { 0x9000 },
	[MDSS_INT2_GDSC] = { 0xb000 },
};

static struct msm_clk_data sm8650_disp_cc_data = {
	.resets = disp_cc_sm8550_resets,
	.num_resets = ARRAY_SIZE(disp_cc_sm8550_resets),
	.clks = sm8650_disp_clks,
	.num_clks = ARRAY_SIZE(sm8650_disp_clks),
	.power_domains = sm8650_disp_gdscs,
	.num_power_domains = ARRAY_SIZE(sm8650_disp_gdscs),

	.enable = sm8650_disp_cc_enable,
	.set_rate = sm8650_disp_cc_set_rate,
};

static const struct udevice_id disp_cc_sm8550_of_match[] = {
	{	.compatible = "qcom,sm8650-dispcc",
		.data = (ulong)&sm8650_disp_cc_data,
	},
	{ }
};

U_BOOT_DRIVER(disp_cc_sm8550) = {
	.name		= "disp_cc-sm8550",
	.id		= UCLASS_NOP,
	.of_match	= disp_cc_sm8550_of_match,
	.bind		= qcom_cc_bind,
	.flags		= DM_FLAG_PRE_RELOC,
};
