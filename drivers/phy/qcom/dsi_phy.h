/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 */

#ifndef __DSI_PHY_H__
#define __DSI_PHY_H__

#include <dt-bindings/clock/qcom,dsi-phy-28nm.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
//#include <linux/regulator/consumer.h>

//#include "dsi.h"

#define DSI_0   0
#define DSI_1   1
#define DSI_MAX 2

enum msm_dsi_phy_usecase {
        MSM_DSI_PHY_STANDALONE,
        MSM_DSI_PHY_MASTER,
        MSM_DSI_PHY_SLAVE,
};

struct msm_dsi_phy;
struct msm_dsi_phy_shared_timings {
	u32 clk_post;
	u32 clk_pre;
	bool clk_pre_inc_by_2;
	bool byte_intf_clk_div_2;
};

struct msm_dsi_phy_clk_request {
	unsigned long bitclk_rate;
	unsigned long escclk_rate;
};

struct msm_dsi_phy_ops {
	int (*pll_init)(struct msm_dsi_phy *phy);
	int (*enable)(struct msm_dsi_phy *phy,
			struct msm_dsi_phy_clk_request *clk_req);
	void (*disable)(struct msm_dsi_phy *phy);
	bool (*set_continuous_clock)(struct msm_dsi_phy *phy, bool enable);
	int (*parse_dt_properties)(struct msm_dsi_phy *phy);
};

struct msm_dsi_phy_cfg {
	const struct regulator_bulk_data *regulator_data;
	int num_regulators;
	struct msm_dsi_phy_ops ops;

	unsigned long	min_pll_rate;
	unsigned long	max_pll_rate;

	const resource_size_t io_start[DSI_MAX];
	const int num_dsi_phy;
	const int quirks;
	bool has_phy_regulator;
	bool has_phy_lane;
};

extern const struct msm_dsi_phy_cfg dsi_phy_7nm_cfgs;
extern const struct msm_dsi_phy_cfg dsi_phy_4nm_8650_cfgs;

struct msm_dsi_dphy_timing {
	u32 clk_zero;
	u32 clk_trail;
	u32 clk_prepare;
	u32 hs_exit;
	u32 hs_zero;
	u32 hs_prepare;
	u32 hs_trail;
	u32 hs_rqst;
	u32 ta_go;
	u32 ta_sure;
	u32 ta_get;

	struct msm_dsi_phy_shared_timings shared_timings;

	/* For PHY v2 only */
	u32 hs_rqst_ckln;
	u32 hs_prep_dly;
	u32 hs_prep_dly_ckln;
	u8 hs_halfbyte_en;
	u8 hs_halfbyte_en_ckln;
};

#define NUM_PROVIDED_CLKS		(DSI_PIXEL_PLL_CLK + 1)

#define DSI_LANE_MAX			5

struct qcom_video_phy_plat_data {
	const struct qcom_mipi_dphy_timing *qcom_mipi_dphy_timing_table;
	const unsigned int num_timings;
	//enum phy_max_rate max_rate;
};

struct dsi_pll_config {
	bool enable_ssc;
	bool ssc_center;
	u32 ssc_freq;
	u32 ssc_offset;
	u32 ssc_adj_per;

	/* out */
	u32 decimal_div_start;
	u32 frac_div_start;
	u32 pll_clock_inverters;
	u32 ssc_stepsize;
	u32 ssc_div_per;
};

struct dsi_pll_7nm {
	struct udevice *dev;
	void __iomem *base;
	void __iomem *pll_base;
	struct clk clk;
	struct msm_dsi_phy *phy;
	struct msm_dsi_phy_cfg *cfg;
	u64 vco_current_rate;
	struct dsi_pll_7nm *slave;
	u32 id;
};

struct msm_dsi_phy {
	struct udevice *dev;
	const struct qcom_video_phy_plat_data *pdata;
	struct phy_configure_opts_mipi_dphy dphy_cfg;
	void __iomem *base;
	void __iomem *pll_base;
	void __iomem *reg_base;
	void __iomem *lane_base;
	phys_addr_t base_size;
	phys_addr_t pll_size;
	phys_addr_t reg_size;
	phys_addr_t lane_size;
	int id;

	struct clk *ahb_clk;
	//struct regulator_bulk_data *supplies;
	struct udevice *vdds_reg;

	struct msm_dsi_dphy_timing timing;
	const struct msm_dsi_phy_cfg *cfg;
	void *tuning_cfg;

	enum msm_dsi_phy_usecase usecase;
	bool regulator_ldo_mode;
	bool cphy_mode;

	struct clk_hw *vco_hw;
	bool pll_on;

	struct clk_hw_onecell_data *provided_clocks;

	bool state_saved;
};

/*
 * PHY internal functions
 */
int msm_dsi_dphy_timing_calc(struct msm_dsi_dphy_timing *timing,
			     struct msm_dsi_phy_clk_request *clk_req);
int msm_dsi_dphy_timing_calc_v2(struct msm_dsi_dphy_timing *timing,
				struct msm_dsi_phy_clk_request *clk_req);
int msm_dsi_dphy_timing_calc_v3(struct msm_dsi_dphy_timing *timing,
				struct msm_dsi_phy_clk_request *clk_req);
int msm_dsi_dphy_timing_calc_v4(struct msm_dsi_dphy_timing *timing,
				struct msm_dsi_phy_clk_request *clk_req);
int msm_dsi_cphy_timing_calc_v4(struct msm_dsi_dphy_timing *timing,
				struct msm_dsi_phy_clk_request *clk_req);

#endif /* __DSI_PHY_H__ */
