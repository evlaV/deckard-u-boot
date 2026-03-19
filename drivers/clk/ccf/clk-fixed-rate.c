// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Mike Turquette, Linaro Ltd <mturquette@linaro.org>
 *
 * Fixed rate clock implementation
 */

#include <linux/clk-provider.h>
#include <dm.h>
#include <dm/devres.h>
#include <log.h>
#include <dm/device-internal.h>
#include <linux/io.h>
#include <linux/err.h>

#include "clk.h"

/*
 * DOC: basic fixed-rate clock that cannot gate
 *
 * Traits of this clock:
 * prepare - clk_(un)prepare only ensures parents are prepared
 * enable - clk_enable only ensures parents are enabled
 * rate - rate is always a fixed value.  No clk_set_rate support
 * parent - fixed parent.  No clk_set_parent support
 */

#define to_clk_fixed_rate(_hw) container_of(_hw, struct clk_fixed_rate, hw)

static unsigned long clk_fixed_rate_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	return to_clk_fixed_rate(hw)->fixed_rate;
}

static unsigned long clk_fixed_rate_recalc_accuracy(struct clk_hw *hw,
		unsigned long parent_accuracy)
{
	struct clk_fixed_rate *fixed = to_clk_fixed_rate(hw);

	if (fixed->flags & CLK_FIXED_RATE_PARENT_ACCURACY)
		return parent_accuracy;

	return fixed->fixed_accuracy;
}

const struct clk_ops clk_fixed_rate_ops = {
	.recalc_rate = clk_fixed_rate_recalc_rate,
	.recalc_accuracy = clk_fixed_rate_recalc_accuracy,
};
EXPORT_SYMBOL_GPL(clk_fixed_rate_ops);

static void devm_clk_hw_register_fixed_rate_release(struct udevice *dev, void *res)
{
	struct clk_fixed_rate *fix = res;

	/*
	 * We can not use clk_hw_unregister_fixed_rate, since it will kfree()
	 * the hw, resulting in double free. Just unregister the hw and let
	 * devres code kfree() it.
	 */
	clk_hw_unregister(&fix->hw);
}

struct clk_hw *__clk_hw_register_fixed_rate(struct udevice *dev,
		ofnode np, const char *name,
		const char *parent_name, const struct clk_hw *parent_hw,
		const struct clk_parent_data *parent_data, unsigned long flags,
		unsigned long fixed_rate, unsigned long fixed_accuracy,
		unsigned long clk_fixed_flags, bool devm)
{
	struct clk_fixed_rate *fixed;
	struct clk_hw *hw;
	struct clk_init_data init = {};
	int ret = -EINVAL;

	/* allocate fixed-rate clock */
	if (devm)
		fixed = devres_alloc(devm_clk_hw_register_fixed_rate_release,
				     sizeof(*fixed), GFP_KERNEL);
	else
		fixed = kzalloc(sizeof(*fixed), GFP_KERNEL);
	if (!fixed)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_fixed_rate_ops;
	init.flags = flags;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.parent_hws = parent_hw ? &parent_hw : NULL;
	init.parent_data = parent_data;
	if (parent_name || parent_hw || parent_data)
		init.num_parents = 1;
	else
		init.num_parents = 0;

	/* struct clk_fixed_rate assignments */
	fixed->flags = clk_fixed_flags;
	fixed->fixed_rate = fixed_rate;
	fixed->fixed_accuracy = fixed_accuracy;
	fixed->hw.init = &init;

	/* register the clock */
	hw = &fixed->hw;
	if (dev || !ofnode_valid(np))
		ret = clk_hw_register(dev, hw);
	else
		ret = of_clk_hw_register(np, hw);
	if (ret) {
		if (devm)
			devres_free(fixed);
		else
			kfree(fixed);
		hw = ERR_PTR(ret);
	} else if (devm)
		devres_add(dev, fixed);

	return hw;
}
EXPORT_SYMBOL_GPL(__clk_hw_register_fixed_rate);

struct clk *clk_register_fixed_rate(struct udevice *dev, const char *name,
		const char *parent_name, unsigned long flags,
		unsigned long fixed_rate)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fixed_rate_with_accuracy(dev, name, parent_name,
						      flags, fixed_rate, 0);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}
EXPORT_SYMBOL_GPL(clk_register_fixed_rate);

void clk_unregister_fixed_rate(struct clk *clk)
{
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	clk_unregister(clk);
	kfree(to_clk_fixed_rate(hw));
}
EXPORT_SYMBOL_GPL(clk_unregister_fixed_rate);

void clk_hw_unregister_fixed_rate(struct clk_hw *hw)
{
	struct clk_fixed_rate *fixed;

	fixed = to_clk_fixed_rate(hw);

	clk_hw_unregister(hw);
	kfree(fixed);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister_fixed_rate);

static struct clk_hw *_of_fixed_clk_setup(struct udevice *dev)
{
	struct clk_hw *hw;
	ofnode node = dev_ofnode(dev);
	const char *clk_name;
	u32 rate;
	u32 accuracy = 0;
	int ret;

	if (ofnode_read_u32(node, "clock-frequency", &rate))
		return ERR_PTR(-EIO);

	ofnode_read_u32(node, "clock-accuracy", &accuracy);

	clk_name = ofnode_read_string(node, "clock-output-names");
	if (!clk_name)
		clk_name = ofnode_get_name(node);

	hw = clk_hw_register_fixed_rate_with_accuracy(dev, clk_name, NULL,
						    0, rate, accuracy);
	if (IS_ERR(hw))
		return hw;

	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, hw);
	if (ret) {
		clk_hw_unregister_fixed_rate(hw);
		return ERR_PTR(ret);
	}

	return hw;
}

static int clk_fixed_rate_probe(struct udevice *dev)
{
	struct clk_hw *hw = _of_fixed_clk_setup(dev);

	if (!IS_ERR_OR_NULL(hw))
		dev_set_priv(dev, hw);

	return 0;
}

static const struct udevice_id clk_fixed_rate_match[] = {
	{
		.compatible = "fixed-clock",
	},
	{ /* sentinel */ }
};

U_BOOT_DRIVER(fixed_clock) = {
	.name = "fixed_clock",
	.id = UCLASS_NOP,
	.of_match = clk_fixed_rate_match,
	.probe = clk_fixed_rate_probe,
	.plat_auto	= sizeof(struct clk_fixed_rate),
	.flags = DM_FLAG_PRE_RELOC,
};
