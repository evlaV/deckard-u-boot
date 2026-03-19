// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011 Sascha Hauer, Pengutronix <s.hauer@pengutronix.de>
 */

#include <dm/ofnode.h>
#include <dm/device.h>
#include <dm/device-internal.h>
#include <dm/devres.h>
#include <div64.h>
#include <linux/compat.h>
#include <linux/clk-provider.h>
#include <linux/err.h>

#include "clk.h"

/*
 * DOC: basic fixed multiplier and divider clock that cannot gate
 *
 * Traits of this clock:
 * prepare - clk_prepare only ensures that parents are prepared
 * enable - clk_enable only ensures that parents are enabled
 * rate - rate is fixed.  clk->rate = parent->rate / div * mult
 * parent - fixed parent.  No clk_set_parent support
 */

static unsigned long clk_factor_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct clk_fixed_factor *fix = to_clk_fixed_factor(hw);
	unsigned long long int rate;

	rate = (unsigned long long int)parent_rate * fix->mult;
	do_div(rate, fix->div);
	return (unsigned long)rate;
}

static int clk_factor_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	struct clk_fixed_factor *fix = to_clk_fixed_factor(hw);

	if (clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT) {
		unsigned long best_parent;

		best_parent = (req->rate / fix->mult) * fix->div;
		req->best_parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw), best_parent);
	}

	req->rate = (req->best_parent_rate / fix->div) * fix->mult;

	return 0;
}

static int clk_factor_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	/*
	 * We must report success but we can do so unconditionally because
	 * clk_factor_determine_rate returns values that ensure this call is a
	 * nop.
	 */

	return 0;
}

static unsigned long clk_factor_recalc_accuracy(struct clk_hw *hw,
						unsigned long parent_accuracy)
{
	struct clk_fixed_factor *fix = to_clk_fixed_factor(hw);

	if (fix->flags & CLK_FIXED_FACTOR_FIXED_ACCURACY)
		return fix->acc;

	return parent_accuracy;
}

const struct clk_ops clk_fixed_factor_ops = {
	.determine_rate = clk_factor_determine_rate,
	.set_rate = clk_factor_set_rate,
	.recalc_rate = clk_factor_recalc_rate,
	.recalc_accuracy = clk_factor_recalc_accuracy,
};
EXPORT_SYMBOL_GPL(clk_fixed_factor_ops);

static void devm_clk_hw_register_fixed_factor_release(struct udevice *dev, void *res)
{
	struct clk_fixed_factor *fix = res;

	/*
	 * We can not use clk_hw_unregister_fixed_factor, since it will kfree()
	 * the hw, resulting in double free. Just unregister the hw and let
	 * devres code kfree() it.
	 */
	clk_hw_unregister(&fix->hw);
}

static struct clk_hw *
__clk_hw_register_fixed_factor(struct udevice *dev, ofnode np,
		const char *name, const char *parent_name,
		const struct clk_hw *parent_hw, const struct clk_parent_data *pdata,
		unsigned long flags, unsigned int mult, unsigned int div,
		unsigned long acc, unsigned int fixflags, bool devm)
{
	struct clk_fixed_factor *fix;
	struct clk_init_data init = { };
	struct clk_hw *hw;
	int ret;

	/* You can't use devm without a dev */
	if (devm && !dev)
		return ERR_PTR(-EINVAL);

	if (devm)
		fix = devres_alloc(devm_clk_hw_register_fixed_factor_release,
				sizeof(*fix), GFP_KERNEL);
	else
		fix = kmalloc(sizeof(*fix), GFP_KERNEL);
	if (!fix)
		return ERR_PTR(-ENOMEM);

	/* struct clk_fixed_factor assignments */
	fix->mult = mult;
	fix->div = div;
	fix->hw.init = &init;
	fix->acc = acc;
	fix->flags = fixflags;

	init.name = name;
	init.ops = &clk_fixed_factor_ops;
	init.flags = flags;
	if (parent_name)
		init.parent_names = &parent_name;
	else if (parent_hw)
		init.parent_hws = &parent_hw;
	else
		init.parent_data = pdata;
	init.num_parents = 1;

	hw = &fix->hw;
	if (dev)
		ret = clk_hw_register(dev, hw);
	else
		ret = of_clk_hw_register(np, hw);
	if (ret) {
		if (devm)
			devres_free(fix);
		else
			kfree(fix);
		hw = ERR_PTR(ret);
	} else if (devm)
		devres_add(dev, fix);

	return hw;
}

/**
 * devm_clk_hw_register_fixed_factor_index - Register a fixed factor clock with
 * parent from DT index
 * @dev: device that is registering this clock
 * @name: name of this clock
 * @index: index of phandle in @dev 'clocks' property
 * @flags: fixed factor flags
 * @mult: multiplier
 * @div: divider
 *
 * Return: Pointer to fixed factor clk_hw structure that was registered or
 * an error pointer.
 */
struct clk_hw *devm_clk_hw_register_fixed_factor_index(struct udevice *dev,
		const char *name, unsigned int index, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = index };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, NULL, NULL, &pdata,
					      flags, mult, div, 0, 0, true);
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register_fixed_factor_index);

/**
 * devm_clk_hw_register_fixed_factor_parent_hw - Register a fixed factor clock with
 * pointer to parent clock
 * @dev: device that is registering this clock
 * @name: name of this clock
 * @parent_hw: pointer to parent clk
 * @flags: fixed factor flags
 * @mult: multiplier
 * @div: divider
 *
 * Return: Pointer to fixed factor clk_hw structure that was registered or
 * an error pointer.
 */
struct clk_hw *devm_clk_hw_register_fixed_factor_parent_hw(struct udevice *dev,
		const char *name, const struct clk_hw *parent_hw,
		unsigned long flags, unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1 };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, NULL, parent_hw,
					      &pdata, flags, mult, div, 0, 0, true);
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register_fixed_factor_parent_hw);

struct clk_hw *clk_hw_register_fixed_factor_parent_hw(struct udevice *dev,
		const char *name, const struct clk_hw *parent_hw,
		unsigned long flags, unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1 };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, NULL, parent_hw,
					      &pdata, flags, mult, div, 0, 0, false);
}
EXPORT_SYMBOL_GPL(clk_hw_register_fixed_factor_parent_hw);

struct clk_hw *clk_hw_register_fixed_factor(struct udevice *dev,
		const char *name, const char *parent_name, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1 };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, parent_name, NULL,
					      &pdata, flags, mult, div, 0, 0, false);
}
EXPORT_SYMBOL_GPL(clk_hw_register_fixed_factor);

struct clk_hw *clk_hw_register_fixed_factor_fwname(struct udevice *dev,
		ofnode np, const char *name, const char *fw_name,
		unsigned long flags, unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1, .fw_name = fw_name };

	return __clk_hw_register_fixed_factor(dev, np, name, NULL, NULL,
			&pdata, flags, mult, div, 0, 0, false);
}
EXPORT_SYMBOL_GPL(clk_hw_register_fixed_factor_fwname);

struct clk_hw *clk_hw_register_fixed_factor_with_accuracy_fwname(struct udevice *dev,
		ofnode np, const char *name, const char *fw_name,
		unsigned long flags, unsigned int mult, unsigned int div,
		unsigned long acc)
{
	const struct clk_parent_data pdata = { .index = -1, .fw_name = fw_name };

	return __clk_hw_register_fixed_factor(dev, np, name, NULL, NULL,
			&pdata, flags, mult, div, acc,
			CLK_FIXED_FACTOR_FIXED_ACCURACY, false);
}
EXPORT_SYMBOL_GPL(clk_hw_register_fixed_factor_with_accuracy_fwname);

struct clk_hw *clk_hw_register_fixed_factor_index(struct udevice *dev,
		const char *name, unsigned int index, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = index };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, NULL, NULL, &pdata,
					      flags, mult, div, 0, 0, false);
}
EXPORT_SYMBOL_GPL(clk_hw_register_fixed_factor_index);

struct clk *clk_register_fixed_factor(struct udevice *dev, const char *name,
		const char *parent_name, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fixed_factor(dev, name, parent_name, flags, mult,
					  div);
	if (IS_ERR(hw))
		return ERR_CAST(hw);
	return hw->clk;
}
EXPORT_SYMBOL_GPL(clk_register_fixed_factor);

void clk_unregister_fixed_factor(struct clk *clk)
{
	struct clk_hw *hw;

	hw = __clk_get_hw(clk);
	if (!hw)
		return;

	clk_unregister(clk);
	kfree(to_clk_fixed_factor(hw));
}
EXPORT_SYMBOL_GPL(clk_unregister_fixed_factor);

void clk_hw_unregister_fixed_factor(struct clk_hw *hw)
{
	struct clk_fixed_factor *fix;

	fix = to_clk_fixed_factor(hw);

	clk_hw_unregister(hw);
	kfree(fix);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister_fixed_factor);

struct clk_hw *devm_clk_hw_register_fixed_factor(struct udevice *dev,
		const char *name, const char *parent_name, unsigned long flags,
		unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1 };

	return __clk_hw_register_fixed_factor(dev, dev_ofnode(dev), name, parent_name, NULL,
			&pdata, flags, mult, div, 0, 0, true);
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register_fixed_factor);

struct clk_hw *devm_clk_hw_register_fixed_factor_fwname(struct udevice *dev,
		ofnode np, const char *name, const char *fw_name,
		unsigned long flags, unsigned int mult, unsigned int div)
{
	const struct clk_parent_data pdata = { .index = -1, .fw_name = fw_name };

	return __clk_hw_register_fixed_factor(dev, np, name, NULL, NULL,
			&pdata, flags, mult, div, 0, 0, true);
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register_fixed_factor_fwname);

struct clk_hw *devm_clk_hw_register_fixed_factor_with_accuracy_fwname(struct udevice *dev,
		ofnode np, const char *name, const char *fw_name,
		unsigned long flags, unsigned int mult, unsigned int div,
		unsigned long acc)
{
	const struct clk_parent_data pdata = { .index = -1, .fw_name = fw_name };

	return __clk_hw_register_fixed_factor(dev, np, name, NULL, NULL,
			&pdata, flags, mult, div, acc,
			CLK_FIXED_FACTOR_FIXED_ACCURACY, true);
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register_fixed_factor_with_accuracy_fwname);
static struct clk_hw *_of_fixed_factor_clk_setup(struct udevice *dev)
{
	struct clk_hw *hw;
	const char *clk_name;
	ofnode node = dev_ofnode(dev);
	const struct clk_parent_data pdata = { .index = 0 };
	u32 div, mult;
	int ret;

	if (ofnode_read_u32(node, "clock-div", &div)) {
		pr_err("%s Fixed factor clock <%pOFn> must have a clock-div property\n",
			__func__, &node);
		return ERR_PTR(-EIO);
	}

	if (ofnode_read_u32(node, "clock-mult", &mult)) {
		pr_err("%s Fixed factor clock <%pOFn> must have a clock-mult property\n",
			__func__, &node);
		return ERR_PTR(-EIO);
	}

	clk_name = ofnode_read_string(node, "clock-output-names");
	if (!clk_name)
		clk_name = ofnode_get_name(node);

	hw = __clk_hw_register_fixed_factor(dev, node, clk_name, NULL, NULL,
					    &pdata, 0, mult, div, 0, 0, false);
	if (IS_ERR(hw)) {
		/*
		 * Clear OF_POPULATED flag so that clock registration can be
		 * attempted again from probe function.
		 */
		return ERR_CAST(hw);
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, hw);
	if (ret) {
		clk_hw_unregister_fixed_factor(hw);
		return ERR_PTR(ret);
	}

	return hw;
}

static int of_fixed_factor_clk_probe(struct udevice *dev)
{
	struct clk_hw *clk;

	/*
	 * This function is not executed when of_fixed_factor_clk_setup
	 * succeeded.
	 */
	clk = _of_fixed_factor_clk_setup(dev);
	if (IS_ERR(clk)) {
		printf("%s: %s: err %ld\n", __func__, dev->name, PTR_ERR(clk));
		return PTR_ERR(clk);
	}

	dev_set_priv(dev, clk);

	return 0;
}

static const struct udevice_id clk_fixed_factor_match_table[] = {
	{
		.compatible = "fixed-factor-clock",
	},
	{ }
};

U_BOOT_DRIVER(clk_fixed_factor) = {
	.name	= "clk-fixed-factor",
	.id	= UCLASS_NOP,
	.probe = of_fixed_factor_clk_probe,
	.of_match = clk_fixed_factor_match_table,
};
