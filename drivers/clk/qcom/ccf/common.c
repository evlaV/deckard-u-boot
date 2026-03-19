// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 */

#include <dm/read.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <dm/ofnode.h>
#include <linux/compat.h>
#include <linux/regmap.h>
#include <linux/clk-provider.h>
#include <reset.h>
#include <event.h>

#include "common.h"
#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "reset.h"
#include "gdsc.h"

#include "common-uboot.h"

DECLARE_GLOBAL_DATA_PTR;

struct qcom_cc {
	struct clk_regmap **rclks;
	size_t num_rclks;
};

const
struct freq_tbl *qcom_find_freq(const struct freq_tbl *f, unsigned long rate)
{
	if (!f)
		return NULL;

	if (!f->freq)
		return f;

	for (; f->freq; f++)
		if (rate <= f->freq)
			return f;

	/* Default to our fastest rate */
	return f - 1;
}
EXPORT_SYMBOL_GPL(qcom_find_freq);

const struct freq_multi_tbl *qcom_find_freq_multi(const struct freq_multi_tbl *f,
						  unsigned long rate)
{
	if (!f)
		return NULL;

	if (!f->freq)
		return f;

	for (; f->freq; f++)
		if (rate <= f->freq)
			return f;

	/* Default to our fastest rate */
	return f - 1;
}
EXPORT_SYMBOL_GPL(qcom_find_freq_multi);

const struct freq_tbl *qcom_find_freq_floor(const struct freq_tbl *f,
					    unsigned long rate)
{
	const struct freq_tbl *best = NULL;

	for ( ; f->freq; f++) {
		if (rate >= f->freq)
			best = f;
		else
			break;
	}

	return best;
}
EXPORT_SYMBOL_GPL(qcom_find_freq_floor);

int qcom_find_src_index(struct clk_hw *hw, const struct parent_map *map, u8 src)
{
	int i, num_parents = clk_hw_get_num_parents(hw);

	for (i = 0; i < num_parents; i++)
		if (src == map[i].src)
			return i;

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(qcom_find_src_index);

int qcom_find_cfg_index(struct clk_hw *hw, const struct parent_map *map, u8 cfg)
{
	int i, num_parents = clk_hw_get_num_parents(hw);

	for (i = 0; i < num_parents; i++)
		if (cfg == map[i].cfg)
			return i;

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(qcom_find_cfg_index);

struct regmap *
qcom_cc_map(struct udevice *dev, const struct qcom_cc_desc *desc)
{
	void __iomem *base;
	struct regmap_config config = {
		.reg_offset_shift = 0,
	};

	switch (desc->config->val_bits) {
	case 64:
		config.width = REGMAP_SIZE_64;
		break;
	case 32:
		config.width = REGMAP_SIZE_32;
		break;
	case 16:
		config.width = REGMAP_SIZE_16;
		break;
	}

	if (desc->config->max_register) {
		base = dev_read_addr_ptr(dev);
		if (!IS_ERR(base)) {
			config.r_start = (ulong)base;
			config.r_size = desc->config->max_register - (ulong)base;
		}
	}

	return devm_regmap_init(dev, NULL, NULL, &config);
}
EXPORT_SYMBOL_GPL(qcom_cc_map);

void
qcom_pll_set_fsm_mode(struct regmap *map, u32 reg, u8 bias_count, u8 lock_count)
{
	u32 val;
	u32 mask;

	/* De-assert reset to FSM */
	regmap_update_bits(map, reg, PLL_VOTE_FSM_RESET, 0);

	/* Program bias count and lock count */
	val = bias_count << PLL_BIAS_COUNT_SHIFT |
		lock_count << PLL_LOCK_COUNT_SHIFT;
	mask = PLL_BIAS_COUNT_MASK << PLL_BIAS_COUNT_SHIFT;
	mask |= PLL_LOCK_COUNT_MASK << PLL_LOCK_COUNT_SHIFT;
	regmap_update_bits(map, reg, mask, val);

	/* Enable PLL FSM voting */
	regmap_update_bits(map, reg, PLL_VOTE_FSM_ENA, PLL_VOTE_FSM_ENA);
}
EXPORT_SYMBOL_GPL(qcom_pll_set_fsm_mode);

static struct clk_hw *qcom_cc_clk_hw_get(struct ofnode_phandle_args *clkspec,
					 void *data)
{
	struct qcom_cc *cc = data;
	unsigned int idx = clkspec->args[0];

	// printf("%s: %s: id %u\n", __func__, ofnode_get_name(clkspec->node), idx);

	if (idx >= cc->num_rclks) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	if (!cc->rclks[idx])
		return NULL;

	/* U-Boot: some clocks may not be registered pre-reloc */
	return cc->rclks[idx]->hw.core ? &cc->rclks[idx]->hw : NULL;
}

int qcom_cc_really_probe(struct udevice *dev,
			 const struct qcom_cc_desc *desc, struct regmap *regmap)
{
	int ret;
	struct qcom_cc *cc;
	size_t num_clks = desc->num_clks;
	struct clk_regmap **rclks = desc->clks;
	size_t num_clk_hws = desc->num_clk_hws;
	struct clk_hw **clk_hws = desc->clk_hws;
	int i;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	if (desc->driver_data &&
	    desc->driver_data->dfs_rcgs &&
	    desc->driver_data->num_dfs_rcgs) {
		ret = qcom_cc_register_rcg_dfs(regmap,
					       desc->driver_data->dfs_rcgs,
					       desc->driver_data->num_dfs_rcgs);
		if (ret)
			goto put_rpm;
	}

	cc->rclks = rclks;
	cc->num_rclks = num_clks;

	for (i = 0; i < num_clk_hws; i++) {
		ret = devm_clk_hw_register(dev, clk_hws[i]);
		if (ret)
			return ret;
	}

	for (i = 0; i < num_clks; i++) {
		if (!rclks[i])
			continue;

		ret = devm_clk_register_regmap(dev, rclks[i], regmap);
		if (ret)
			return ret;
	}

	ret = devm_of_clk_add_hw_provider(dev, qcom_cc_clk_hw_get, cc);
	if (ret)
		goto put_rpm;

put_rpm:
	if (desc->use_rpm)
		pm_runtime_put(dev);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_cc_really_probe);

int qcom_cc_probe(struct udevice *dev, const struct qcom_cc_desc *desc)
{
	struct regmap *regmap;

	regmap = qcom_cc_map(dev, desc);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return qcom_cc_really_probe(dev, desc, regmap);
}
EXPORT_SYMBOL_GPL(qcom_cc_probe);

