// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Linaro Ltd <mturquette@linaro.org>
 *
 * Standard functionality for the common clock API.  See Documentation/driver-api/clk.rst
 */

#define pr_fmt(fmt) "[CLK] "fmt

#include <clk.h>
#include <clk-uclass.h>
#include <dm/devres.h>
#include <linux/bug.h>
#include <linux/clk-provider.h>
#include <linux/compat.h>
#include <linux/clk/clk-conf.h>
#include <linux/device.h>
#include <linux/kref.h>
#include <log.h>
#include <dm/device.h>
#include <dm/uclass.h>
#include <dm/lists.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>

#include "clk.h"

/*
 * List of orphan clk_core's who's parents haven't been registered yet
 */
static struct hlist_head clk_orphan_list = HLIST_HEAD_INIT;

/*
 * List of root clocks (which have no parent)
 */
static struct hlist_head clk_root_list = HLIST_HEAD_INIT;

/*
 * List of all initialise clk_core's. Linux uses a hashtable, we use a hlist
 */
static struct hlist_head clk_hashtable = HLIST_HEAD_INIT;

static const struct hlist_head *all_lists[] = {
	&clk_root_list,
	&clk_orphan_list,
	NULL,
};

/***    private data structures    ***/

struct clk_parent_map {
	const struct clk_hw	*hw;
	struct clk_core		*core;
	const char		*fw_name;
	const char		*name;
	int			index : 20;
	/*
	 * U-Boot: Avoid repeatedly trying to probe
	 * missing clock parents. Since we lazily
	 * probe clock providers, if we get an error
	 * then trying again later will never work since
	 * we don't support loadable modules.
	 */
	int			error : 12;
};

struct clk_core {
	const char		*name;
	const struct clk_ops	*ops;
	struct clk_hw		*hw;
	struct udevice		*dev;
	ofnode			ofnode;
	struct clk_core		*parent;
	struct clk_parent_map	*parents;
	u8			num_parents;
	u8			new_parent_index;
	unsigned long		rate;
	unsigned long		req_rate;
	unsigned long		new_rate;
	struct clk_core		*new_parent;
	struct clk_core		*new_child;
	unsigned long		flags;
	bool			orphan;
	unsigned short		prepare_count;
	unsigned short		enable_count;
	unsigned long		min_rate;
	unsigned long		max_rate;
	struct hlist_head	children;
	struct hlist_node	child_node;
	struct hlist_node	hashtable_node;
	struct hlist_head	clks;
	struct kref		ref;
};

static bool clk_core_is_prepared(struct clk_core *core)
{
	bool ret = false;

	/*
	 * .is_prepared is optional for clocks that can prepare
	 * fall back to software usage counter if it is missing
	 */
	if (!core->ops->is_prepared)
		return core->prepare_count;

	ret = core->ops->is_prepared(core->hw);

	return ret;
}

static bool clk_core_is_enabled(struct clk_core *core)
{
	bool ret = false;

	/*
	 * .is_enabled is only mandatory for clocks that gate
	 * fall back to software usage counter if .is_enabled is missing
	 */
	if (!core->ops->is_enabled)
		return core->enable_count;

	/*
	 * This could be called with the enable lock held, or from atomic
	 * context. If the parent isn't enabled already, we can't do
	 * anything here. We can also assume this clock isn't enabled.
	 */
	if ((core->flags & CLK_OPS_PARENT_ENABLE) && core->parent)
		if (!clk_core_is_enabled(core->parent)) {
			ret = false;
			goto done;
		}

	ret = core->ops->is_enabled(core->hw);

done:
		pm_runtime_put(core->dev);

	return ret;
}

/***    helper functions   ***/

const char *__clk_get_name(const struct clk *clk)
{
	return !clk ? NULL : clk->core->name;
}
EXPORT_SYMBOL_GPL(__clk_get_name);

const char *clk_hw_get_name(const struct clk_hw *hw)
{
	return hw->core->name;
}
EXPORT_SYMBOL_GPL(clk_hw_get_name);

struct udevice *clk_hw_get_dev(const struct clk_hw *hw)
{
	return hw->core->dev;
}
EXPORT_SYMBOL_GPL(clk_hw_get_dev);

ofnode clk_hw_get_of_node(const struct clk_hw *hw)
{
	return hw->core->ofnode;
}
EXPORT_SYMBOL_GPL(clk_hw_get_of_node);

struct clk_hw *__clk_get_hw(struct clk *clk)
{
	return !clk ? NULL : clk->core->hw;
}
EXPORT_SYMBOL_GPL(__clk_get_hw);

unsigned int clk_hw_get_num_parents(const struct clk_hw *hw)
{
	return hw->core->num_parents;
}
EXPORT_SYMBOL_GPL(clk_hw_get_num_parents);

struct clk_hw *clk_hw_get_parent(const struct clk_hw *hw)
{
	return hw->core->parent ? hw->core->parent->hw : NULL;
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent);

static struct clk_core *clk_core_lookup(const char *name)
{
	struct clk_core *core;

	if (!name)
		return NULL;

	/* search the hashtable */
	hlist_for_each_entry(core, &clk_hashtable, hashtable_node)
		if (!strcmp(core->name, name) && strlen(name) == strlen(core->name))
			return core;

	return NULL;
}

static int of_parse_clkspec(const ofnode np, int index,
			    const char *name, struct ofnode_phandle_args *out_args);
struct clk_hw *
of_clk_get_hw_from_clkspec(struct ofnode_phandle_args *clkspec);

/**
 * clk_core_get - Find the clk_core parent of a clk
 * @core: clk to find parent of
 * @p_index: parent index to search for
 *
 * This is the preferred method for clk providers to find the parent of a
 * clk when that parent is external to the clk controller. The parent_names
 * array is indexed and treated as a local name matching a string in the device
 * node's 'clock-names' property or as the 'con_id' matching the device's
 * dev_name() in a clk_lookup. This allows clk providers to use their own
 * namespace instead of looking for a globally unique parent string.
 *
 * For example the following DT snippet would allow a clock registered by the
 * clock-controller@c001 that has a clk_init_data::parent_data array
 * with 'xtal' in the 'name' member to find the clock provided by the
 * clock-controller@f00abcd without needing to get the globally unique name of
 * the xtal clk.
 *
 *      parent: clock-controller@f00abcd {
 *              reg = <0xf00abcd 0xabcd>;
 *              #clock-cells = <0>;
 *      };
 *
 *      clock-controller@c001 {
 *              reg = <0xc001 0xf00d>;
 *              clocks = <&parent>;
 *              clock-names = "xtal";
 *              #clock-cells = <1>;
 *      };
 *
 * Returns: -ENOENT when the provider can't be found or the clk doesn't
 * exist in the provider or the name can't be found in the DT node or
 * in a clkdev lookup. NULL when the provider knows about the clk but it
 * isn't provided on this system.
 * A valid clk_core pointer when the clk can be found in the provider.
 */
static struct clk_core *clk_core_get(struct clk_core *core, u8 p_index)
{
	const char *name = core->parents[p_index].fw_name;
	int index = core->parents[p_index].index;
	struct clk_hw *hw = ERR_PTR(-ENOENT);
	ofnode np = core->ofnode;
	struct ofnode_phandle_args clkspec;

	if (ofnode_valid(np) && (name || index >= 0) &&
	    !of_parse_clkspec(np, index, name, &clkspec)) {
		hw = of_clk_get_hw_from_clkspec(&clkspec);
		ofnode_put(clkspec.node);
	} else if (name) {
		/*
		 * If the DT search above couldn't find the provider fallback to
		 * looking up via clkdev based clk_lookups.
		 */
		// hw = clk_find_hw(dev_id, name);
		WARN(1, "[CCF]: clkdev lookups aren't yet supported!\n");
		return ERR_PTR(-ENOENT);
	}

	if (IS_ERR(hw))
		return ERR_CAST(hw);

	if (!hw)
		return NULL;

	return hw->core;
}

static void clk_core_fill_parent_index(struct clk_core *core, u8 index)
{
	struct clk_parent_map *entry = &core->parents[index];
	struct clk_core *parent;

	if (entry->hw) {
		parent = entry->hw->core;
	} else {
		parent = clk_core_get(core, index);
		if (PTR_ERR(parent) == -ENOENT && entry->name)
			parent = clk_core_lookup(entry->name);
	}

	/*
	 * We have a direct reference but it isn't registered yet?
	 * Orphan it and let clk_reparent() update the orphan status
	 * when the parent is registered.
	 */
	if (!parent)
		parent = ERR_PTR(-EPROBE_DEFER);

	/* Only cache it if it's not an error */
	if (!IS_ERR(parent))
		entry->core = parent;
	/*
	 * U-Boot: save the error so we avoid trying to get this parent again.
	 * -EPROBE_DEFER probably means the parent is in the same clock device
	 * and just isn't available yet.
	 */
	else if (PTR_ERR(parent) != -EPROBE_DEFER) {
		entry->error = PTR_ERR(parent);
	}
}

static struct clk_core *clk_core_get_parent_by_index(struct clk_core *core,
							 u8 index)
{
	if (!core || index >= core->num_parents || !core->parents)
		return NULL;

	/*
	 * U-Boot: if we failed to get a parent before we don't need to
	 * try again, unlike Linux we don't have modules.
	 * The only reason this might happen is due to a driver issue.
	 */
	if (core->parents[index].error)
		return NULL;

	if (!core->parents[index].core)
		clk_core_fill_parent_index(core, index);

	return core->parents[index].core;
}

struct clk_hw *
clk_hw_get_parent_by_index(const struct clk_hw *hw, unsigned int index)
{
	struct clk_core *parent;

	parent = clk_core_get_parent_by_index(hw->core, index);

	return !parent ? NULL : parent->hw;
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent_by_index);

unsigned int __clk_get_enable_count(struct clk *clk)
{
	return !clk ? 0 : clk->core->enable_count;
}

static unsigned long clk_core_get_rate_nolock(struct clk_core *core)
{
	if (!core)
		return 0;

	if (!core->num_parents || core->parent)
		return core->rate;

	/*
	 * Clk must have a parent because num_parents > 0 but the parent isn't
	 * known yet. Best to return 0 as the rate of this clk until we can
	 * properly recalc the rate based on the parent's rate.
	 */
	return 0;
}

unsigned long clk_hw_get_rate(const struct clk_hw *hw)
{
	return clk_core_get_rate_nolock(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_get_rate);

unsigned long clk_hw_get_flags(const struct clk_hw *hw)
{
	return hw->core->flags;
}
EXPORT_SYMBOL_GPL(clk_hw_get_flags);

bool clk_hw_is_prepared(const struct clk_hw *hw)
{
	return clk_core_is_prepared(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_is_prepared);

bool clk_hw_is_enabled(const struct clk_hw *hw)
{
	return clk_core_is_enabled(hw->core);
}
EXPORT_SYMBOL_GPL(clk_hw_is_enabled);

bool __clk_is_enabled(struct clk *clk)
{
	if (!clk)
		return false;

	return clk_core_is_enabled(clk->core);
}
EXPORT_SYMBOL_GPL(__clk_is_enabled);

static bool mux_is_better_rate(unsigned long rate, unsigned long now,
			   unsigned long best, unsigned long flags)
{
	if (flags & CLK_MUX_ROUND_CLOSEST)
		return abs(now - rate) < abs(best - rate);

	return now <= rate && now > best;
}

static void clk_core_init_rate_req(struct clk_core * const core,
				   struct clk_rate_request *req,
				   unsigned long rate);

static int clk_core_round_rate_nolock(struct clk_core *core,
				      struct clk_rate_request *req);

static bool clk_core_has_parent(struct clk_core *core, const struct clk_core *parent)
{
	struct clk_core *tmp;
	unsigned int i;

	/* Optimize for the case where the parent is already the parent. */
	if (core->parent == parent)
		return true;

	for (i = 0; i < core->num_parents; i++) {
		tmp = clk_core_get_parent_by_index(core, i);
		if (!tmp)
			continue;

		if (tmp == parent)
			return true;
	}

	return false;
}

static void
clk_core_forward_rate_req(struct clk_core *core,
			  const struct clk_rate_request *old_req,
			  struct clk_core *parent,
			  struct clk_rate_request *req,
			  unsigned long parent_rate)
{
	if (WARN_ON(!clk_core_has_parent(core, parent)))
		return;

	clk_core_init_rate_req(parent, req, parent_rate);

	if (req->min_rate < old_req->min_rate)
		req->min_rate = old_req->min_rate;

	if (req->max_rate > old_req->max_rate)
		req->max_rate = old_req->max_rate;
}

static int
clk_core_determine_rate_no_reparent(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	struct clk_core *core = hw->core;
	struct clk_core *parent = core->parent;
	unsigned long best;
	int ret;

	if (core->flags & CLK_SET_RATE_PARENT) {
		struct clk_rate_request parent_req;

		if (!parent) {
			req->rate = 0;
			return 0;
		}

		clk_core_forward_rate_req(core, req, parent, &parent_req,
					  req->rate);

		ret = clk_core_round_rate_nolock(parent, &parent_req);
		if (ret)
			return ret;

		best = parent_req.rate;
	} else if (parent) {
		best = clk_core_get_rate_nolock(parent);
	} else {
		best = clk_core_get_rate_nolock(core);
	}

	req->best_parent_rate = best;
	req->rate = best;

	return 0;
}

int clk_mux_determine_rate_flags(struct clk_hw *hw,
				 struct clk_rate_request *req,
				 unsigned long flags)
{
	struct clk_core *core = hw->core, *parent, *best_parent = NULL;
	int i, num_parents, ret;
	unsigned long best = 0;

	/* if NO_REPARENT flag set, pass through to current parent */
	if (core->flags & CLK_SET_RATE_NO_REPARENT)
		return clk_core_determine_rate_no_reparent(hw, req);

	/* find the parent that can provide the fastest rate <= rate */
	num_parents = core->num_parents;
	for (i = 0; i < num_parents; i++) {
		unsigned long parent_rate;

		parent = clk_core_get_parent_by_index(core, i);
		if (!parent)
			continue;

		if (core->flags & CLK_SET_RATE_PARENT) {
			struct clk_rate_request parent_req;

			clk_core_forward_rate_req(core, req, parent, &parent_req, req->rate);

			ret = clk_core_round_rate_nolock(parent, &parent_req);
			if (ret)
				continue;

			parent_rate = parent_req.rate;
		} else {
			parent_rate = clk_core_get_rate_nolock(parent);
		}

		if (mux_is_better_rate(req->rate, parent_rate,
				       best, flags)) {
			best_parent = parent;
			best = parent_rate;
		}
	}

	if (!best_parent)
		return -EINVAL;

	req->best_parent_hw = best_parent->hw;
	req->best_parent_rate = best;
	req->rate = best;

	return 0;
}
EXPORT_SYMBOL_GPL(clk_mux_determine_rate_flags);

struct clk *__clk_lookup(const char *name)
{
	struct clk_core *core = clk_core_lookup(name);

	return !core ? NULL : core->hw->clk;
}

/* U-Boot: we don't track requests per-user */
static void clk_core_get_boundaries(struct clk_core *core,
				    unsigned long *min_rate,
				    unsigned long *max_rate)
{
	*min_rate = core->min_rate;
	*max_rate = core->max_rate;
}

/*
 * clk_hw_get_rate_range() - returns the clock rate range for a hw clk
 * @hw: the hw clk we want to get the range from
 * @min_rate: pointer to the variable that will hold the minimum
 * @max_rate: pointer to the variable that will hold the maximum
 *
 * Fills the @min_rate and @max_rate variables with the minimum and
 * maximum that clock can reach.
 */
void clk_hw_get_rate_range(struct clk_hw *hw, unsigned long *min_rate,
			   unsigned long *max_rate)
{
	clk_core_get_boundaries(hw->core, min_rate, max_rate);
}
EXPORT_SYMBOL_GPL(clk_hw_get_rate_range);

void clk_hw_set_rate_range(struct clk_hw *hw, unsigned long min_rate,
			   unsigned long max_rate)
{
	hw->core->min_rate = min_rate;
	hw->core->max_rate = max_rate;
}
EXPORT_SYMBOL_GPL(clk_hw_set_rate_range);

/*
 * __clk_mux_determine_rate - clk_ops::determine_rate implementation for a mux type clk
 * @hw: mux type clk to determine rate on
 * @req: rate request, also used to return preferred parent and frequencies
 *
 * Helper for finding best parent to provide a given frequency. This can be used
 * directly as a determine_rate callback (e.g. for a mux), or from a more
 * complex clock that may combine a mux with other operations.
 *
 * Returns: 0 on success, -EERROR value on error
 */
int __clk_mux_determine_rate(struct clk_hw *hw,
			     struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, 0);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate);

int __clk_mux_determine_rate_closest(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	return clk_mux_determine_rate_flags(hw, req, CLK_MUX_ROUND_CLOSEST);
}
EXPORT_SYMBOL_GPL(__clk_mux_determine_rate_closest);

/*
 * clk_hw_determine_rate_no_reparent - clk_ops::determine_rate implementation for a clk that doesn't reparent
 * @hw: mux type clk to determine rate on
 * @req: rate request, also used to return preferred frequency
 *
 * Helper for finding best parent rate to provide a given frequency.
 * This can be used directly as a determine_rate callback (e.g. for a
 * mux), or from a more complex clock that may combine a mux with other
 * operations.
 *
 * Returns: 0 on success, -EERROR value on error
 */
int clk_hw_determine_rate_no_reparent(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	return clk_core_determine_rate_no_reparent(hw, req);
}
EXPORT_SYMBOL_GPL(clk_hw_determine_rate_no_reparent);

/***        clk api        ***/
static void clk_core_unprepare(struct clk_core *core)
{
	if (!core)
		return;

	if (WARN(core->prepare_count == 0,
	    "%s already unprepared\n", core->name))
		return;

	if (WARN(core->prepare_count == 1 && core->flags & CLK_IS_CRITICAL,
	    "Unpreparing critical %s\n", core->name))
		return;

	if (--core->prepare_count > 0)
		return;

	WARN(core->enable_count > 0, "Unpreparing enabled %s\n", core->name);

	if (core->ops->unprepare)
		core->ops->unprepare(core->hw);

	clk_core_unprepare(core->parent);
}

#define clk_core_unprepare_lock(core) clk_core_unprepare(core)

/**
 * clk_unprepare - undo preparation of a clock source
 * @clk: the clk being unprepared
 *
 * clk_unprepare may sleep, which differentiates it from clk_disable.  In a
 * simple case, clk_unprepare can be used instead of clk_disable to gate a clk
 * if the operation may sleep.  One example is a clk which is accessed over
 * I2c.  In the complex case a clk gate operation may require a fast and a slow
 * part.  It is this reason that clk_unprepare and clk_disable are not mutually
 * exclusive.  In fact clk_disable must be called before clk_unprepare.
 */
void clk_unprepare(struct clk *clk)
{
	if (IS_ERR_OR_NULL(clk))
		return;

	clk_core_unprepare(clk->core);
}
EXPORT_SYMBOL_GPL(clk_unprepare);

static int clk_core_prepare(struct clk_core *core)
{
	int ret = 0;

	if (!core)
		return 0;

	if (core->prepare_count == 0) {
		ret = clk_core_prepare(core->parent);
		if (ret)
			return ret;

		if (core->ops->prepare)
			ret = core->ops->prepare(core->hw);

		if (ret)
			goto unprepare;
	}

	core->prepare_count++;

	return 0;
unprepare:
	clk_core_unprepare(core->parent);
	return ret;
}

#define clk_core_prepare_lock(core) clk_core_prepare(core)

/**
 * clk_prepare - prepare a clock source
 * @clk: the clk being prepared
 *
 * clk_prepare may sleep, which differentiates it from clk_enable.  In a simple
 * case, clk_prepare can be used instead of clk_enable to ungate a clk if the
 * operation may sleep.  One example is a clk which is accessed over I2c.  In
 * the complex case a clk ungate operation may require a fast and a slow part.
 * It is this reason that clk_prepare and clk_enable are not mutually
 * exclusive.  In fact clk_prepare must be called before clk_enable.
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_prepare(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_prepare_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_prepare);

static void clk_core_disable(struct clk_core *core)
{
	if (!core)
		return;

	pr_debug("%s disable\n", core->name);

	if (WARN(core->enable_count == 0, "%s already disabled\n", core->name))
		return;

	if (WARN(core->enable_count == 1 && core->flags & CLK_IS_CRITICAL,
	    "Disabling critical %s\n", core->name))
		return;

	if (--core->enable_count > 0)
		return;

	if (core->ops->disable)
		core->ops->disable(core->hw);

	clk_core_disable(core->parent);
}

#define clk_core_disable_lock(core) clk_core_disable(core)

/**
 * clk_disable - gate a clock
 * @clk: the clk being gated
 *
 * clk_disable must not sleep, which differentiates it from clk_unprepare.  In
 * a simple case, clk_disable can be used instead of clk_unprepare to gate a
 * clk if the operation is fast and will never sleep.  One example is a
 * SoC-internal clk which is controlled via simple register writes.  In the
 * complex case a clk gate operation may require a fast and a slow part.  It is
 * this reason that clk_unprepare and clk_disable are not mutually exclusive.
 * In fact clk_disable must be called before clk_unprepare.
 */
int clk_disable(struct clk *clk)
{
	/* U-Boot: Don't return an error here to match U-Boot API. */
	if (IS_ERR_OR_NULL(clk))
		return 0;

	clk_core_disable(clk->core);

	return 0;
}
EXPORT_SYMBOL_GPL(clk_disable);

static int clk_core_enable(struct clk_core *core)
{
	int ret = 0;

	if (!core)
		return 0;

	if (!clk_core_is_prepared(core) && core->ops->prepare) {
		pr_debug("%s: enable without prepare!", core->name);
		ret = clk_core_prepare(core);
		if (ret)
			return ret;
	}

	if (core->enable_count == 0) {
		ret = clk_core_enable(core->parent);

		if (ret)
			return ret;

		if (core->ops->enable)
			ret = core->ops->enable(core->hw);

		if (ret) {
			clk_core_disable(core->parent);
			return ret;
		}
	}

	core->enable_count++;
	return 0;
}

#define clk_core_enable_lock(core) clk_core_enable(core)

/**
 * clk_enable - ungate a clock
 * @clk: the clk being ungated
 *
 * clk_enable must not sleep, which differentiates it from clk_prepare.  In a
 * simple case, clk_enable can be used instead of clk_prepare to ungate a clk
 * if the operation will never sleep.  One example is a SoC-internal clk which
 * is controlled via simple register writes.  In the complex case a clk ungate
 * operation may require a fast and a slow part.  It is this reason that
 * clk_enable and clk_prepare are not mutually exclusive.  In fact clk_prepare
 * must be called before clk_enable.  Returns 0 on success, -EERROR
 * otherwise.
 */
int clk_enable(struct clk *clk)
{
	if (!clk)
		return 0;

	return clk_core_enable_lock(clk->core);
}
EXPORT_SYMBOL_GPL(clk_enable);


static int clk_core_prepare_enable(struct clk_core *core)
{
	int ret;

	ret = clk_core_prepare_lock(core);
	if (ret)
		return ret;

	ret = clk_core_enable_lock(core);
	if (ret)
		clk_core_unprepare_lock(core);

	return ret;
}

static void clk_core_disable_unprepare(struct clk_core *core)
{
	clk_core_disable_lock(core);
	clk_core_unprepare_lock(core);
}

static int clk_core_determine_round_nolock(struct clk_core *core,
					   struct clk_rate_request *req)
{
	long rate;

	if (!core)
		return 0;

	/*
	 * Some clock providers hand-craft their clk_rate_requests and
	 * might not fill min_rate and max_rate.
	 *
	 * If it's the case, clamping the rate is equivalent to setting
	 * the rate to 0 which is bad. Skip the clamping but complain so
	 * that it gets fixed, hopefully.
	 */
	if (!req->min_rate && !req->max_rate)
		pr_warn("%s: %s: clk_rate_request has initialized min or max rate.\n",
			__func__, core->name);
	else
		req->rate = clamp(req->rate, req->min_rate, req->max_rate);

	/*
	 * At this point, core protection will be disabled
	 * - if the provider is not protected at all
	 * - if the calling consumer is the only one which has exclusivity
	 *   over the provider
	 */
	/*if (clk_core_rate_is_protected(core)) {
		req->rate = core->rate;
	} else*/ if (core->ops->determine_rate) {
		return core->ops->determine_rate(core->hw, req);
	} else if (core->ops->round_rate) {
		rate = core->ops->round_rate(core->hw, req->rate,
					     &req->best_parent_rate);
		if (rate < 0)
			return rate;

		req->rate = rate;
	} else {
		return -EINVAL;
	}

	return 0;
}

static void clk_core_init_rate_req(struct clk_core * const core,
				   struct clk_rate_request *req,
				   unsigned long rate)
{
	struct clk_core *parent;

	if (WARN_ON(!req))
		return;

	memset(req, 0, sizeof(*req));
	req->max_rate = ULONG_MAX;

	if (!core)
		return;

	req->core = core;
	req->rate = rate;
	clk_core_get_boundaries(core, &req->min_rate, &req->max_rate);

	parent = core->parent;
	if (parent) {
		req->best_parent_hw = parent->hw;
		req->best_parent_rate = parent->rate;
	} else {
		req->best_parent_hw = NULL;
		req->best_parent_rate = 0;
	}
}

/**
 * clk_hw_init_rate_request - Initializes a clk_rate_request
 * @hw: the clk for which we want to submit a rate request
 * @req: the clk_rate_request structure we want to initialise
 * @rate: the rate which is to be requested
 *
 * Initializes a clk_rate_request structure to submit to
 * __clk_determine_rate() or similar functions.
 */
void clk_hw_init_rate_request(const struct clk_hw *hw,
			      struct clk_rate_request *req,
			      unsigned long rate)
{
	if (WARN_ON(!hw || !req))
		return;

	clk_core_init_rate_req(hw->core, req, rate);
}
EXPORT_SYMBOL_GPL(clk_hw_init_rate_request);

/**
 * clk_hw_forward_rate_request - Forwards a clk_rate_request to a clock's parent
 * @hw: the original clock that got the rate request
 * @old_req: the original clk_rate_request structure we want to forward
 * @parent: the clk we want to forward @old_req to
 * @req: the clk_rate_request structure we want to initialise
 * @parent_rate: The rate which is to be requested to @parent
 *
 * Initializes a clk_rate_request structure to submit to a clock parent
 * in __clk_determine_rate() or similar functions.
 */
void clk_hw_forward_rate_request(const struct clk_hw *hw,
				 const struct clk_rate_request *old_req,
				 const struct clk_hw *parent,
				 struct clk_rate_request *req,
				 unsigned long parent_rate)
{
	if (WARN_ON(!hw || !old_req || !parent || !req))
		return;

	clk_core_forward_rate_req(hw->core, old_req,
				  parent->core, req,
				  parent_rate);
}
EXPORT_SYMBOL_GPL(clk_hw_forward_rate_request);

static bool clk_core_can_round(struct clk_core * const core)
{
	return core->ops->determine_rate || core->ops->round_rate;
}

static int clk_core_round_rate_nolock(struct clk_core *core,
				      struct clk_rate_request *req)
{
	int ret;

	if (!core) {
		req->rate = 0;
		return 0;
	}

	if (clk_core_can_round(core))
		return clk_core_determine_round_nolock(core, req);

	if (core->flags & CLK_SET_RATE_PARENT) {
		struct clk_rate_request parent_req;

		clk_core_forward_rate_req(core, req, core->parent, &parent_req, req->rate);

		ret = clk_core_round_rate_nolock(core->parent, &parent_req);
		if (ret)
			return ret;

		req->best_parent_rate = parent_req.rate;
		req->rate = parent_req.rate;

		return 0;
	}

	req->rate = core->rate;
	return 0;
}

/**
 * __clk_determine_rate - get the closest rate actually supported by a clock
 * @hw: determine the rate of this clock
 * @req: target rate request
 *
 * Useful for clk_ops such as .set_rate and .determine_rate.
 */
int __clk_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	if (!hw) {
		req->rate = 0;
		return 0;
	}

	return clk_core_round_rate_nolock(hw->core, req);
}
EXPORT_SYMBOL_GPL(__clk_determine_rate);

/**
 * clk_hw_round_rate() - round the given rate for a hw clk
 * @hw: the hw clk for which we are rounding a rate
 * @rate: the rate which is to be rounded
 *
 * Takes in a rate as input and rounds it to a rate that the clk can actually
 * use.
 *
 * Context: prepare_lock must be held.
 *          For clk providers to call from within clk_ops such as .round_rate,
 *          .determine_rate.
 *
 * Return: returns rounded rate of hw clk if clk supports round_rate operation
 *         else returns the parent rate.
 */
unsigned long clk_hw_round_rate(struct clk_hw *hw, unsigned long rate)
{
	int ret;
	struct clk_rate_request req;

	clk_core_init_rate_req(hw->core, &req, rate);

	ret = clk_core_round_rate_nolock(hw->core, &req);
	if (ret)
		return 0;

	return req.rate;
}
EXPORT_SYMBOL_GPL(clk_hw_round_rate);

/**
 * clk_round_rate - round the given rate for a clk
 * @clk: the clk for which we are rounding a rate
 * @rate: the rate which is to be rounded
 *
 * Takes in a rate as input and rounds it to a rate that the clk can actually
 * use which is then returned.  If clk doesn't support round_rate operation
 * then the parent rate is returned.
 */
ulong clk_round_rate(struct clk *clk, unsigned long rate)
{
	struct clk_rate_request req;
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();

	clk_core_init_rate_req(clk->core, &req, rate);

	ret = clk_core_round_rate_nolock(clk->core, &req);

	clk_prepare_unlock();

	if (ret)
		return ret;

	return req.rate;
}
EXPORT_SYMBOL_GPL(clk_round_rate);

static unsigned long clk_recalc(struct clk_core *core,
				unsigned long parent_rate)
{
	unsigned long rate = parent_rate;

	if (core->ops->recalc_rate) {
		rate = core->ops->recalc_rate(core->hw, parent_rate);
	}
	return rate;
}

/**
 * __clk_recalc_rates
 * @core: first clk in the subtree
 * @update_req: Whether req_rate should be updated with the new rate
 * @msg: notification type (see include/linux/clk.h)
 *
 * Walks the subtree of clks starting with clk and recalculates rates as it
 * goes.  Note that if a clk does not implement the .recalc_rate callback then
 * it is assumed that the clock will take on the rate of its parent.
 *
 * clk_recalc_rates also propagates the POST_RATE_CHANGE notification,
 * if necessary.
 */
static void __clk_recalc_rates(struct clk_core *core, bool update_req)
{
	unsigned long old_rate;
	unsigned long parent_rate = 0;
	struct clk_core *child;

	old_rate = core->rate;

	if (core->parent)
		parent_rate = core->parent->rate;

	core->rate = clk_recalc(core, parent_rate);
	if (update_req)
		core->req_rate = core->rate;

	hlist_for_each_entry(child, &core->children, child_node)
		__clk_recalc_rates(child, update_req);
}

static unsigned long clk_core_get_rate_recalc(struct clk_core *core)
{
	if (core && (core->flags & CLK_GET_RATE_NOCACHE))
		__clk_recalc_rates(core, false);

	return clk_core_get_rate_nolock(core);
}

/**
 * clk_get_rate - return the rate of clk
 * @clk: the clk whose rate is being returned
 *
 * Simply returns the cached rate of the clk, unless CLK_GET_RATE_NOCACHE flag
 * is set, which means a recalc_rate will be issued. Can be called regardless of
 * the clock enabledness. If clk is NULL, or if an error occurred, then returns
 * 0.
 */
unsigned long clk_get_rate(struct clk *clk)
{
	unsigned long rate;

	if (!clk)
		return 0;

	clk_prepare_lock();
	rate = clk_core_get_rate_recalc(clk->core);
	clk_prepare_unlock();

	return rate;
}
EXPORT_SYMBOL_GPL(clk_get_rate);

static int clk_fetch_parent_index(struct clk_core *core,
				  struct clk_core *parent)
{
	int i;

	if (!parent)
		return -EINVAL;

	for (i = 0; i < core->num_parents; i++) {
		/* Found it first try! */
		if (core->parents[i].core == parent)
			return i;

		/* Something else is here, so keep looking */
		if (core->parents[i].core)
			continue;

		/* Maybe core hasn't been cached but the hw is all we know? */
		if (core->parents[i].hw) {
			if (core->parents[i].hw == parent->hw)
				break;

			/* Didn't match, but we're expecting a clk_hw */
			continue;
		}

		/* Maybe it hasn't been cached (clk_set_parent() path) */
		if (parent == clk_core_get(core, i))
			break;

		/* Fallback to comparing globally unique names */
		if (core->parents[i].name &&
		    !strcmp(parent->name, core->parents[i].name))
			break;
	}

	if (i == core->num_parents)
		return -EINVAL;

	core->parents[i].core = parent;
	return i;
}

/**
 * clk_hw_get_parent_index - return the index of the parent clock
 * @hw: clk_hw associated with the clk being consumed
 *
 * Fetches and returns the index of parent clock. Returns -EINVAL if the given
 * clock does not have a current parent.
 */
int clk_hw_get_parent_index(struct clk_hw *hw)
{
	struct clk_hw *parent = clk_hw_get_parent(hw);

	if (WARN_ON(parent == NULL))
		return -EINVAL;

	return clk_fetch_parent_index(hw->core, parent->core);
}
EXPORT_SYMBOL_GPL(clk_hw_get_parent_index);

/*
 * Update the orphan status of @core and all its children.
 */
static void clk_core_update_orphan_status(struct clk_core *core, bool is_orphan)
{
	struct clk_core *child;

	core->orphan = is_orphan;

	hlist_for_each_entry(child, &core->children, child_node)
		clk_core_update_orphan_status(child, is_orphan);
}

static void clk_reparent(struct clk_core *core, struct clk_core *new_parent)
{
	bool was_orphan = core->orphan;

	hlist_del(&core->child_node);

	if (new_parent) {
		bool becomes_orphan = new_parent->orphan;

		/* avoid duplicate POST_RATE_CHANGE notifications */
		if (new_parent->new_child == core)
			new_parent->new_child = NULL;

		hlist_add_head(&core->child_node, &new_parent->children);

		if (was_orphan != becomes_orphan)
			clk_core_update_orphan_status(core, becomes_orphan);
	} else {
		hlist_add_head(&core->child_node, &clk_orphan_list);
		if (!was_orphan)
			clk_core_update_orphan_status(core, true);
	}

	core->parent = new_parent;
}

static struct clk_core *__clk_set_parent_before(struct clk_core *core,
					   struct clk_core *parent)
{
	unsigned long flags;
	struct clk_core *old_parent = core->parent;

	/*
	 * 1. enable parents for CLK_OPS_PARENT_ENABLE clock
	 *
	 * 2. Migrate prepare state between parents and prevent race with
	 * clk_enable().
	 *
	 * If the clock is not prepared, then a race with
	 * clk_enable/disable() is impossible since we already have the
	 * prepare lock (future calls to clk_enable() need to be preceded by
	 * a clk_prepare()).
	 *
	 * If the clock is prepared, migrate the prepared state to the new
	 * parent and also protect against a race with clk_enable() by
	 * forcing the clock and the new parent on.  This ensures that all
	 * future calls to clk_enable() are practically NOPs with respect to
	 * hardware and software states.
	 *
	 * See also: Comment for clk_set_parent() below.
	 */

	/* enable old_parent & parent if CLK_OPS_PARENT_ENABLE is set */
	if (core->flags & CLK_OPS_PARENT_ENABLE) {
		clk_core_prepare_enable(old_parent);
		clk_core_prepare_enable(parent);
	}

	/* migrate prepare count if > 0 */
	if (core->prepare_count) {
		clk_core_prepare_enable(parent);
		clk_core_enable_lock(core);
	}

	/* update the clk tree topology */
	flags = clk_enable_lock();
	clk_reparent(core, parent);
	clk_enable_unlock(flags);

	return old_parent;
}

static void __clk_set_parent_after(struct clk_core *core,
				   struct clk_core *parent,
				   struct clk_core *old_parent)
{
	/*
	 * Finish the migration of prepare state and undo the changes done
	 * for preventing a race with clk_enable().
	 */
	if (core->prepare_count) {
		clk_core_disable_lock(core);
		clk_core_disable_unprepare(old_parent);
	}

	/* re-balance ref counting if CLK_OPS_PARENT_ENABLE is set */
	if (core->flags & CLK_OPS_PARENT_ENABLE) {
		clk_core_disable_unprepare(parent);
		clk_core_disable_unprepare(old_parent);
	}
}

static int __clk_set_parent(struct clk_core *core, struct clk_core *parent,
			    u8 p_index)
{
	unsigned long flags;
	int ret = 0;
	struct clk_core *old_parent;

	old_parent = __clk_set_parent_before(core, parent);

	/* change clock input source */
	if (parent && core->ops->set_parent)
		ret = core->ops->set_parent(core->hw, p_index);

	if (ret) {
		flags = clk_enable_lock();
		clk_reparent(core, old_parent);
		clk_enable_unlock(flags);

		__clk_set_parent_after(core, old_parent, parent);

		return ret;
	}

	__clk_set_parent_after(core, parent, old_parent);

	return 0;
}

static void clk_calc_subtree(struct clk_core *core, unsigned long new_rate,
			     struct clk_core *new_parent, u8 p_index)
{
	struct clk_core *child;

	core->new_rate = new_rate;
	core->new_parent = new_parent;
	core->new_parent_index = p_index;
	/* include clk in new parent's PRE_RATE_CHANGE notifications */
	core->new_child = NULL;
	if (new_parent && new_parent != core->parent)
		new_parent->new_child = core;

	hlist_for_each_entry(child, &core->children, child_node) {
		child->new_rate = clk_recalc(child, new_rate);
		clk_calc_subtree(child, child->new_rate, NULL, 0);
	}
}

/*
 * calculate the new rates returning the topmost clock that has to be
 * changed.
 */
static struct clk_core *clk_calc_new_rates(struct clk_core *core,
					   unsigned long rate)
{
	struct clk_core *top = core;
	struct clk_core *old_parent, *parent;
	unsigned long best_parent_rate = 0;
	unsigned long new_rate;
	unsigned long min_rate;
	unsigned long max_rate;
	int p_index = 0;
	int ret;

	/* sanity */
	if (IS_ERR_OR_NULL(core))
		return NULL;

	/* save parent rate, if it exists */
	parent = old_parent = core->parent;
	if (parent)
		best_parent_rate = parent->rate;

	clk_core_get_boundaries(core, &min_rate, &max_rate);

	/* find the closest rate and parent clk/rate */
	if (clk_core_can_round(core)) {
		struct clk_rate_request req;

		clk_core_init_rate_req(core, &req, rate);

		ret = clk_core_determine_round_nolock(core, &req);
		if (ret < 0)
			return NULL;

		best_parent_rate = req.best_parent_rate;
		new_rate = req.rate;
		parent = req.best_parent_hw ? req.best_parent_hw->core : NULL;

		if (new_rate < min_rate || new_rate > max_rate)
			return NULL;
	} else if (!parent || !(core->flags & CLK_SET_RATE_PARENT)) {
		/* pass-through clock without adjustable parent */
		core->new_rate = core->rate;
		return NULL;
	} else {
		/* pass-through clock with adjustable parent */
		top = clk_calc_new_rates(parent, rate);
		new_rate = parent->new_rate;
		goto out;
	}

	/* some clocks must be gated to change parent */
	if (parent != old_parent &&
	    (core->flags & CLK_SET_PARENT_GATE) && core->prepare_count) {
		pr_debug("%s: %s not gated but wants to reparent\n",
			 __func__, core->name);
		return NULL;
	}

	/* try finding the new parent index */
	if (parent && core->num_parents > 1) {
		p_index = clk_fetch_parent_index(core, parent);
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
				 __func__, parent->name, core->name);
			return NULL;
		}
	}

	if ((core->flags & CLK_SET_RATE_PARENT) && parent &&
	    best_parent_rate != parent->rate)
		top = clk_calc_new_rates(parent, best_parent_rate);

out:
	clk_calc_subtree(core, new_rate, parent, p_index);

	return top;
}

/*
 * walk down a subtree and set the new rates notifying the rate
 * change on the way
 */
static void clk_change_rate(struct clk_core *core)
{
	struct clk_core *child;
	struct hlist_node *tmp;
	unsigned long old_rate;
	unsigned long best_parent_rate = 0;
	bool skip_set_rate = false;
	struct clk_core *old_parent;
	struct clk_core *parent = NULL;

	old_rate = core->rate;

	if (core->new_parent) {
		parent = core->new_parent;
		best_parent_rate = core->new_parent->rate;
	} else if (core->parent) {
		parent = core->parent;
		best_parent_rate = core->parent->rate;
	}

	if (core->flags & CLK_SET_RATE_UNGATE) {
		clk_core_prepare(core);
		clk_core_enable_lock(core);
	}

	if (core->new_parent && core->new_parent != core->parent) {
		old_parent = __clk_set_parent_before(core, core->new_parent);

		if (core->ops->set_rate_and_parent) {
			skip_set_rate = true;
			core->ops->set_rate_and_parent(core->hw, core->new_rate,
					best_parent_rate,
					core->new_parent_index);
		} else if (core->ops->set_parent) {
			core->ops->set_parent(core->hw, core->new_parent_index);
		}
		__clk_set_parent_after(core, core->new_parent, old_parent);
	}

	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_prepare_enable(parent);

	if (!skip_set_rate && core->ops->set_rate)
		core->ops->set_rate(core->hw, core->new_rate, best_parent_rate);

	core->rate = clk_recalc(core, best_parent_rate);

	if (core->flags & CLK_SET_RATE_UNGATE) {
		clk_core_disable_lock(core);
		clk_core_unprepare(core);
	}

	if (core->flags & CLK_OPS_PARENT_ENABLE)
		clk_core_disable_unprepare(parent);

	if (core->flags & CLK_RECALC_NEW_RATES)
		(void)clk_calc_new_rates(core, core->new_rate);

	/*
	 * Use safe iteration, as change_rate can actually swap parents
	 * for certain clock types.
	 */
	hlist_for_each_entry_safe(child, tmp, &core->children, child_node) {
		/* Skip children who will be reparented to another clock */
		if (child->new_parent && child->new_parent != core)
			continue;
		clk_change_rate(child);
	}

	/* handle the new child who might not be in core->children yet */
	if (core->new_child)
		clk_change_rate(core->new_child);
}

static unsigned long clk_core_req_round_rate_nolock(struct clk_core *core,
						     unsigned long req_rate)
{
	int ret;
	struct clk_rate_request req;

	if (!core)
		return 0;

	clk_core_init_rate_req(core, &req, req_rate);

	ret = clk_core_round_rate_nolock(core, &req);

	return ret ? 0 : req.rate;
}

static int clk_core_set_rate_nolock(struct clk_core *core,
				    unsigned long req_rate)
{
	struct clk_core *top;
	unsigned long rate;

	if (!core)
		return 0;

	rate = clk_core_req_round_rate_nolock(core, req_rate);

	/* bail early if nothing to do */
	if (rate == clk_core_get_rate_nolock(core))
		return 0;

	/* calculate new rates and get the topmost changed clock */
	top = clk_calc_new_rates(core, req_rate);
	if (!top)
		return -EINVAL;

	/* change the rates */
	clk_change_rate(top);

	core->req_rate = req_rate;

	return 0;
}

/**
 * clk_set_rate - specify a new rate for clk
 * @clk: the clk whose rate is being changed
 * @rate: the new rate for clk
 *
 * In the simplest case clk_set_rate will only adjust the rate of clk.
 *
 * Setting the CLK_SET_RATE_PARENT flag allows the rate change operation to
 * propagate up to clk's parent; whether or not this happens depends on the
 * outcome of clk's .round_rate implementation.  If *parent_rate is unchanged
 * after calling .round_rate then upstream parent propagation is ignored.  If
 * *parent_rate comes back with a new rate for clk's parent then we propagate
 * up to clk's parent and set its rate.  Upward propagation will continue
 * until either a clk does not support the CLK_SET_RATE_PARENT flag or
 * .round_rate stops requesting changes to clk's parent_rate.
 *
 * Rate changes are accomplished via tree traversal that also recalculates the
 * rates for the clocks and fires off POST_RATE_CHANGE notifiers.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
long clk_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;

	if (!clk)
		return 0;

	/* prevent racing with updates to the clock topology */
	clk_prepare_lock();

	ret = clk_core_set_rate_nolock(clk->core, rate);

	clk_prepare_unlock();
	if (ret < 0)
		return ret;

	/* U-Boot: clk_set_rate() returns the new rate, many drivers rely on this */
	return clk->core->new_rate;
}
EXPORT_SYMBOL_GPL(clk_set_rate);

/**
 * clk_get_parent - return the parent of a clk
 * @clk: the clk whose parent gets returned
 *
 * Simply returns clk->parent.  Returns NULL if clk is NULL.
 */
struct clk *clk_get_parent(struct clk *clk)
{
	struct clk *parent;

	if (!clk)
		return NULL;

	clk_prepare_lock();
	/* TODO: Create a per-user clk and change callers to call clk_put */
	parent = !clk->core->parent ? NULL : clk->core->parent->hw->clk;
	clk_prepare_unlock();

	return parent;
}
EXPORT_SYMBOL_GPL(clk_get_parent);

static struct clk_core *__clk_init_parent(struct clk_core *core)
{
	u8 index = 0;

	if (core->num_parents > 1 && core->ops->get_parent)
		index = core->ops->get_parent(core->hw);

	return clk_core_get_parent_by_index(core, index);
}

#define clk_core_reparent(core, new_parent) clk_reparent(core, new_parent)

void clk_hw_reparent(struct clk_hw *hw, struct clk_hw *new_parent)
{
	if (!hw)
		return;

	clk_core_reparent(hw->core, !new_parent ? NULL : new_parent->core);
}

/**
 * clk_has_parent - check if a clock is a possible parent for another
 * @clk: clock source
 * @parent: parent clock source
 *
 * This function can be used in drivers that need to check that a clock can be
 * the parent of another without actually changing the parent.
 *
 * Returns true if @parent is a possible parent for @clk, false otherwise.
 */
bool clk_has_parent(const struct clk *clk, const struct clk *parent)
{
	/* NULL clocks should be nops, so return success if either is NULL. */
	if (!clk || !parent)
		return true;

	return clk_core_has_parent(clk->core, parent->core);
}
EXPORT_SYMBOL_GPL(clk_has_parent);

static int clk_core_set_parent_nolock(struct clk_core *core,
				      struct clk_core *parent)
{
	int ret = 0;
	int p_index = 0;
	unsigned long p_rate = 0;

	if (!core)
		return 0;

	if (core->parent == parent)
		return 0;

	/* verify ops for multi-parent clks */
	if (core->num_parents > 1 && !core->ops->set_parent)
		return -EPERM;

	/* check that we are allowed to re-parent if the clock is in use */
	if ((core->flags & CLK_SET_PARENT_GATE) && core->prepare_count)
		return -EBUSY;

	/* try finding the new parent index */
	if (parent) {
		p_index = clk_fetch_parent_index(core, parent);
		if (p_index < 0) {
			pr_debug("%s: clk %s can not be parent of clk %s\n",
					__func__, parent->name, core->name);
			return p_index;
		}
		p_rate = parent->rate;
	}

	/* do the re-parent */
	ret = __clk_set_parent(core, parent, p_index);

	/* propagate rate an accuracy recalculation accordingly */
	if (ret) {
		__clk_recalc_rates(core, true);
	} else {
		__clk_recalc_rates(core, true);
	}

	return ret;
}

int clk_hw_set_parent(struct clk_hw *hw, struct clk_hw *parent)
{
	return clk_core_set_parent_nolock(hw->core, parent->core);
}
EXPORT_SYMBOL_GPL(clk_hw_set_parent);

/**
 * clk_set_parent - switch the parent of a mux clk
 * @clk: the mux clk whose input we are switching
 * @parent: the new input to clk
 *
 * Re-parent clk to use parent as its new input source.  If clk is in
 * prepared state, the clk will get enabled for the duration of this call. If
 * that's not acceptable for a specific clk (Eg: the consumer can't handle
 * that, the reparenting is glitchy in hardware, etc), use the
 * CLK_SET_PARENT_GATE flag to allow reparenting only when clk is unprepared.
 *
 * After successfully changing clk's parent clk_set_parent will update the
 * clk topology, sysfs topology and propagate rate recalculation via
 * __clk_recalc_rates.
 *
 * Returns 0 on success, -EERROR otherwise.
 */
int clk_set_parent(struct clk *clk, struct clk *parent)
{
	int ret;

	if (!clk)
		return 0;

	clk_prepare_lock();

	ret = clk_core_set_parent_nolock(clk->core,
					 parent ? parent->core : NULL);

	clk_prepare_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(clk_set_parent);

/**
 * clk_is_match - check if two clk's point to the same hardware clock
 * @p: clk compared against q
 * @q: clk compared against p
 *
 * Returns true if the two struct clk pointers both point to the same hardware
 * clock node. Put differently, returns true if struct clk *p and struct clk *q
 * share the same struct clk_core object.
 *
 * Returns false otherwise. Note that two NULL clks are treated as matching.
 */
bool clk_is_match(const struct clk *p, const struct clk *q)
{
	/* trivial case: identical struct clk's or both NULL */
	if (p == q)
		return true;

	/* true if clk->core pointers match. Avoid dereferencing garbage */
	if (!IS_ERR_OR_NULL(p) && !IS_ERR_OR_NULL(q))
		if (p->core == q->core)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(clk_is_match);

static void clk_core_reparent_orphans_nolock(void)
{
	struct clk_core *orphan;
	struct hlist_node *tmp2;

	/*
	 * walk the list of orphan clocks and reparent any that newly finds a
	 * parent.
	 */
	hlist_for_each_entry_safe(orphan, tmp2, &clk_orphan_list, child_node) {
		struct clk_core *parent = __clk_init_parent(orphan);

		/*
		 * We need to use __clk_set_parent_before() and _after() to
		 * properly migrate any prepare/enable count of the orphan
		 * clock. This is important for CLK_IS_CRITICAL clocks, which
		 * are enabled during init but might not have a parent yet.
		 */
		if (parent) {
			/* update the clk tree topology */
			__clk_set_parent_before(orphan, parent);
			__clk_set_parent_after(orphan, parent, NULL);
			__clk_recalc_rates(orphan, true);

			/*
			 * __clk_init_parent() will set the initial req_rate to
			 * 0 if the clock doesn't have clk_ops::recalc_rate and
			 * is an orphan when it's registered.
			 *
			 * 'req_rate' is used by clk_set_rate_range() and
			 * clk_put() to trigger a clk_set_rate() call whenever
			 * the boundaries are modified. Let's make sure
			 * 'req_rate' is set to something non-zero so that
			 * clk_set_rate_range() doesn't drop the frequency.
			 */
			orphan->req_rate = orphan->rate;
		}
	}
}

/**
 * __clk_core_init - initialize the data structures in a struct clk_core
 * @core:	clk_core being initialized
 *
 * Initializes the lists in struct clk_core, queries the hardware for the
 * parent and rate and sets them both.
 */
static int __clk_core_init(struct clk_core *core)
{
	int ret;
	struct clk_core *parent;
	unsigned long rate;

	clk_prepare_lock();

	/*
	 * Set hw->core after grabbing the prepare_lock to synchronize with
	 * callers of clk_core_fill_parent_index() where we treat hw->core
	 * being NULL as the clk not being registered yet. This is crucial so
	 * that clks aren't parented until their parent is fully registered.
	 */
	core->hw->core = core;

	/* check to see if a clock with this name is already registered */
	if (clk_core_lookup(core->name)) {
		pr_debug("%s: clk %s already initialized\n",
				__func__, core->name);
		ret = -EEXIST;
		goto out;
	}

	/* check that clk_ops are sane.  See Documentation/driver-api/clk.rst */
	if (core->ops->set_rate &&
	    !((core->ops->round_rate || core->ops->determine_rate) &&
	      core->ops->recalc_rate)) {
		pr_err("%s: %s must implement .round_rate or .determine_rate in addition to .recalc_rate\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->ops->set_parent && !core->ops->get_parent) {
		pr_err("%s: %s must implement .get_parent & .set_parent\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->ops->set_parent && !core->ops->determine_rate) {
		pr_err("%s: %s must implement .set_parent & .determine_rate\n",
			__func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->num_parents > 1 && !core->ops->get_parent) {
		pr_err("%s: %s must implement .get_parent as it has multi parents\n",
		       __func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	if (core->ops->set_rate_and_parent &&
			!(core->ops->set_parent && core->ops->set_rate)) {
		pr_err("%s: %s must implement .set_parent & .set_rate\n",
				__func__, core->name);
		ret = -EINVAL;
		goto out;
	}

	/*
	 * optional platform-specific magic
	 *
	 * The .init callback is not used by any of the basic clock types, but
	 * exists for weird hardware that must perform initialization magic for
	 * CCF to get an accurate view of clock for any other callbacks. It may
	 * also be used needs to perform dynamic allocations. Such allocation
	 * must be freed in the terminate() callback.
	 * This callback shall not be used to initialize the parameters state,
	 * such as rate, parent, etc ...
	 *
	 * If it exist, this callback should called before any other callback of
	 * the clock
	 */
	if (core->ops->init) {
		ret = core->ops->init(core->hw);
		if (ret)
			goto out;
	}

	parent = core->parent = __clk_init_parent(core);

	/*
	 * Populate core->parent if parent has already been clk_core_init'd. If
	 * parent has not yet been clk_core_init'd then place clk in the orphan
	 * list.  If clk doesn't have any parents then place it in the root
	 * clk list.
	 *
	 * Every time a new clk is clk_init'd then we walk the list of orphan
	 * clocks and re-parent any that are children of the clock currently
	 * being clk_init'd.
	 */
	if (parent) {
		hlist_add_head(&core->child_node, &parent->children);
		core->orphan = parent->orphan;
	} else if (!core->num_parents) {
		hlist_add_head(&core->child_node, &clk_root_list);
		core->orphan = false;
	} else {
		hlist_add_head(&core->child_node, &clk_orphan_list);
		core->orphan = true;
	}
	hlist_add_head(&core->hashtable_node, &clk_hashtable);

	/*
	 * Set clk's rate.  The preferred method is to use .recalc_rate.  For
	 * simple clocks and lazy developers the default fallback is to use the
	 * parent's rate.  If a clock doesn't have a parent (or is orphaned)
	 * then rate is set to zero.
	 */
	if (core->ops->recalc_rate)
		rate = core->ops->recalc_rate(core->hw,
				clk_core_get_rate_nolock(parent));
	else if (parent)
		rate = parent->rate;
	else
		rate = 0;
	core->rate = core->req_rate = rate;

	/*
	 * Enable CLK_IS_CRITICAL clocks so newly added critical clocks
	 * don't get accidentally disabled when walking the orphan tree and
	 * reparenting clocks
	 */
	if (core->flags & CLK_IS_CRITICAL) {
		ret = clk_core_prepare(core);
		if (ret) {
			pr_warn("%s: critical clk '%s' failed to prepare\n",
			       __func__, core->name);
			goto out_list_del;
		}

		ret = clk_core_enable_lock(core);
		if (ret) {
			pr_warn("%s: critical clk '%s' failed to enable\n",
			       __func__, core->name);
			clk_core_unprepare(core);
			goto out_list_del;
		}
	}

	/*
	 * U-Boot: since we probe clk providers on-demand it's enough to only reparent
	 * orphans when adding providers not for every single clock.
	 */
	//clk_core_reparent_orphans_nolock();
out_list_del:
	if (ret)
		hlist_del(&core->hashtable_node);
out:
	if (ret) {
		hlist_del_init(&core->child_node);
		core->hw->core = NULL;
	}

	clk_prepare_unlock();

	return ret;
}

/**
 * clk_core_link_consumer - Add a clk consumer to the list of consumers in a clk_core
 * @core: clk to add consumer to
 * @clk: consumer to link to a clk
 */
static void clk_core_link_consumer(struct clk_core *core, struct clk *clk)
{
	clk_prepare_lock();
	hlist_add_head(&clk->clks_node, &core->clks);
	clk_prepare_unlock();
}

/**
 * clk_core_unlink_consumer - Remove a clk consumer from the list of consumers in a clk_core
 * @clk: consumer to unlink
 */
static void clk_core_unlink_consumer(struct clk *clk)
{
	hlist_del(&clk->clks_node);
}

/**
 * alloc_clk - Allocate a clk consumer, but leave it unlinked to the clk_core
 * @core: clk to allocate a consumer for
 * @dev_id: string describing device name
 * @con_id: connection ID string on device
 *
 * Returns: clk consumer left unlinked from the consumer list
 */
static struct clk *alloc_clk(struct clk_core *core, const char *dev_id,
			     const char *con_id)
{
	struct clk *clk;

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->core = core;
	/*
	 * U-Boot: we don't yet support dev_id/con_id/max_rate in clock
	 * consumers.
	 */
	// clk->dev_id = dev_id;
	// clk->con_id = kstrdup_const(con_id, GFP_KERNEL);
	// clk->max_rate = ULONG_MAX;

	/* U-Boot: stash the provider device so we can get it via clk_hw */
	clk->dev = core->dev;

	return clk;
}

/**
 * free_clk - Free a clk consumer
 * @clk: clk consumer to free
 *
 * Note, this assumes the clk has been unlinked from the clk_core consumer
 * list.
 */
static void free_clk(struct clk *clk)
{
	// kfree_const(clk->con_id);
	kfree(clk);
}

/**
 * clk_hw_create_clk: Allocate and link a clk consumer to a clk_core given
 * a clk_hw
 * @dev: clk consumer device
 * @hw: clk_hw associated with the clk being consumed
 * @dev_id: string describing device name
 * @con_id: connection ID string on device
 *
 * This is the main function used to create a clk pointer for use by clk
 * consumers. It connects a consumer to the clk_core and clk_hw structures
 * used by the framework and clk provider respectively.
 */
struct clk *clk_hw_create_clk(struct udevice *dev, struct clk_hw *hw,
			      const char *dev_id, const char *con_id)
{
	struct clk *clk;
	struct clk_core *core;

	/* This is to allow this function to be chained to others */
	if (IS_ERR_OR_NULL(hw))
		return ERR_CAST(hw);

	core = hw->core;
	clk = alloc_clk(core, dev_id, con_id);
	if (IS_ERR(clk))
		return clk;
	clk->dev = dev;

	// if (!try_module_get(core->owner)) {
	// 	free_clk(clk);
	// 	return ERR_PTR(-ENOENT);
	// }

	kref_get(&core->ref);
	clk_core_link_consumer(core, clk);

	return clk;
}

/* U-Boot: consumers allocate struct clk, we just populate it! */
int clk_hw_create_clk_uboot(struct clk *clk, struct clk_hw *hw)
{
	struct clk_core *core;

	/* This is to allow this function to be chained to others */
	if (IS_ERR_OR_NULL(hw))
		return PTR_ERR(hw);

	if (IS_ERR_OR_NULL(hw->core)) 
		return -EINVAL;

	core = hw->core;
	clk->core = core;

	if (core->dev)
		clk->dev = core->dev;

	/* U-Boot: necessary for UCLASS_CLK compat shim */
	clk->id = hw->clk->id;
	clk->data = hw->clk->data;

	clk_core_link_consumer(core, clk);

	return 0;
}

/**
 * clk_hw_get_clk - get clk consumer given an clk_hw
 * @hw: clk_hw associated with the clk being consumed
 * @con_id: connection ID string on device
 *
 * Returns: new clk consumer
 * This is the function to be used by providers which need
 * to get a consumer clk and act on the clock element
 * Calls to this function must be balanced with calls clk_put()
 */
struct clk *clk_hw_get_clk(struct clk_hw *hw, const char *con_id)
{
	struct udevice *dev = hw->core->dev;
	const char *name = dev ? dev_name(dev) : NULL;

	return clk_hw_create_clk(dev, hw, name, con_id);
}
EXPORT_SYMBOL(clk_hw_get_clk);

static int clk_cpy_name(const char **dst_p, const char *src, bool must_exist)
{
	const char *dst;

	if (!src) {
		if (must_exist)
			return -EINVAL;
		return 0;
	}

	*dst_p = dst = kstrdup_const(src, GFP_KERNEL);
	if (!dst)
		return -ENOMEM;

	return 0;
}

static int clk_core_populate_parent_map(struct clk_core *core,
					const struct clk_init_data *init)
{
	u8 num_parents = init->num_parents;
	const char * const *parent_names = init->parent_names;
	const struct clk_hw **parent_hws = init->parent_hws;
	const struct clk_parent_data *parent_data = init->parent_data;
	int i, ret = 0;
	struct clk_parent_map *parents, *parent;

	if (!num_parents)
		return 0;

	/*
	 * Avoid unnecessary string look-ups of clk_core's possible parents by
	 * having a cache of names/clk_hw pointers to clk_core pointers.
	 */
	parents = kcalloc(num_parents, sizeof(*parents), GFP_KERNEL);
	core->parents = parents;
	if (!parents)
		return -ENOMEM;

	/* Copy everything over because it might be __initdata */
	for (i = 0, parent = parents; i < num_parents; i++, parent++) {
		parent->index = -1;
		if (parent_names) {
			/* throw a WARN if any entries are NULL */
			WARN(!parent_names[i],
				"%s: invalid NULL in %s's .parent_names\n",
				__func__, core->name);
			ret = clk_cpy_name(&parent->name, parent_names[i],
					   true);
		} else if (parent_data) {
			parent->hw = parent_data[i].hw;
			parent->index = parent_data[i].index;
			ret = clk_cpy_name(&parent->fw_name,
					   parent_data[i].fw_name, false);
			if (!ret)
				ret = clk_cpy_name(&parent->name,
						   parent_data[i].name,
						   false);
		} else if (parent_hws) {
			parent->hw = parent_hws[i];
		} else {
			ret = -EINVAL;
			WARN(1, "Must specify parents if num_parents > 0\n");
		}

		if (ret) {
			do {
				kfree_const(parents[i].name);
				kfree_const(parents[i].fw_name);
			} while (--i >= 0);
			kfree(parents);

			return ret;
		}
	}

	return 0;
}

static void clk_core_free_parent_map(struct clk_core *core)
{
	int i = core->num_parents;

	if (!core->num_parents)
		return;

	while (--i >= 0) {
		kfree_const(core->parents[i].name);
		kfree_const(core->parents[i].fw_name);
	}

	kfree(core->parents);
}

/* Free memory allocated for a struct clk_core */
static void __clk_release(struct kref *ref)
{
	struct clk_core *core = container_of(ref, struct clk_core, ref);

	clk_core_free_parent_map(core);
	kfree_const(core->name);
	kfree(core);
}

static struct clk *
__clk_register(struct udevice *dev, ofnode np, struct clk_hw *hw)
{
	int ret;
	struct clk_core *core;
	const struct clk_init_data *init = hw->init;

	/*
	 * The init data is not supposed to be used outside of registration path.
	 * Set it to NULL so that provider drivers can't use it either and so that
	 * we catch use of hw->init early on in the core.
	 */
	hw->init = NULL;

	core = kzalloc(sizeof(*core), GFP_KERNEL);
	if (!core) {
		ret = -ENOMEM;
		goto fail_out;
	}

	kref_init(&core->ref);

	core->name = kstrdup_const(init->name, GFP_KERNEL);
	if (!core->name) {
		ret = -ENOMEM;
		goto fail_name;
	}

	if (WARN_ON(!init->ops)) {
		ret = -EINVAL;
		goto fail_ops;
	}
	core->ops = init->ops;

	core->dev = dev;
	core->ofnode = np;
	// if (dev && dev->driver)
	// 	core->owner = dev->driver->owner;
	core->hw = hw;
	core->flags = init->flags;
	core->num_parents = init->num_parents;
	core->min_rate = 0;
	core->max_rate = ULONG_MAX;

	ret = clk_core_populate_parent_map(core, init);
	if (ret)
		goto fail_parents;

	INIT_HLIST_HEAD(&core->clks);

	/*
	 * Don't call clk_hw_create_clk() here because that would pin the
	 * provider module to itself and prevent it from ever being removed.
	 */
	hw->clk = alloc_clk(core, NULL, NULL);
	if (IS_ERR(hw->clk)) {
		ret = PTR_ERR(hw->clk);
		goto fail_create_clk;
	}

	/* U-Boot: Copy clk flags to hw->clk for sandbox tests */
	hw->clk->flags = core->flags;

	clk_core_link_consumer(core, hw->clk);

	ret = __clk_core_init(core);
	if (!ret)
		return hw->clk;

	clk_prepare_lock();
	clk_core_unlink_consumer(hw->clk);
	clk_prepare_unlock();

	free_clk(hw->clk);
	hw->clk = NULL;

fail_create_clk:
fail_parents:
fail_ops:
fail_name:
	kref_put(&core->ref, __clk_release);
fail_out:
	if (dev) {
		dev_err(dev, "err %d failed to register clk '%s' (%pS)\n",
			      ret, init->name, hw);
	} else {
		pr_err("%pOF: error %pe: failed to register clk '%s' (%pS)\n",
		       &np, ERR_PTR(ret), init->name, hw);
	}
	return ERR_PTR(ret);
}

/**
 * dev_or_parent_of_node() - Get device node of @dev or @dev's parent
 * @dev: Device to get device node of
 *
 * Return: device node pointer of @dev, or the device node pointer of
 * @dev->parent if dev doesn't have a device node, or NULL if neither
 * @dev or @dev->parent have a device node.
 */
static ofnode dev_or_parent_of_node(struct udevice *dev)
{
	ofnode node;

	if (!dev)
		return ofnode_null();

	node = dev_ofnode(dev);
	// if (!ofnode_valid(node))
	// 	node = dev_ofnode(dev->parent);

	return node;
}

/**
 * clk_register - allocate a new clock, register it and return an opaque cookie
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * clk_register is the *deprecated* interface for populating the clock tree with
 * new clock nodes. Use clk_hw_register() instead.
 *
 * Returns: a pointer to the newly allocated struct clk which
 * cannot be dereferenced by driver code but may be used in conjunction with the
 * rest of the clock API.  In the event of an error clk_register will return an
 * error code; drivers must test for an error code after calling clk_register.
 */
struct clk *clk_register(struct udevice *dev, struct clk_hw *hw)
{
	return __clk_register(dev, dev_or_parent_of_node(dev), hw);
}
EXPORT_SYMBOL_GPL(clk_register);

/**
 * clk_hw_register - register a clk_hw and return an error code
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * clk_hw_register is the primary interface for populating the clock tree with
 * new clock nodes. It returns an integer equal to zero indicating success or
 * less than zero indicating failure. Drivers must test for an error code after
 * calling clk_hw_register().
 */
int clk_hw_register(struct udevice *dev, struct clk_hw *hw)
{
	return PTR_ERR_OR_ZERO(__clk_register(dev, dev_or_parent_of_node(dev),
			       hw));
}
EXPORT_SYMBOL_GPL(clk_hw_register);

/*
 * of_clk_hw_register - register a clk_hw and return an error code
 * @node: device_node of device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * of_clk_hw_register() is the primary interface for populating the clock tree
 * with new clock nodes when a struct udevice is not available, but a struct
 * device_node is. It returns an integer equal to zero indicating success or
 * less than zero indicating failure. Drivers must test for an error code after
 * calling of_clk_hw_register().
 */
int of_clk_hw_register(ofnode node, struct clk_hw *hw)
{
	return PTR_ERR_OR_ZERO(__clk_register(NULL, node, hw));
}
EXPORT_SYMBOL_GPL(of_clk_hw_register);

/*
 * Empty clk_ops for unregistered clocks. These are used temporarily
 * after clk_unregister() was called on a clock and until last clock
 * consumer calls clk_put() and the struct clk object is freed.
 */
static int clk_nodrv_prepare_enable(struct clk_hw *hw)
{
	return -ENXIO;
}

static void clk_nodrv_disable_unprepare(struct clk_hw *hw)
{
	WARN_ON_ONCE(1);
}

static int clk_nodrv_set_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long parent_rate)
{
	return -ENXIO;
}

static int clk_nodrv_set_parent(struct clk_hw *hw, u8 index)
{
	return -ENXIO;
}

static int clk_nodrv_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	return -ENXIO;
}

static const struct clk_ops clk_nodrv_ops = {
	.enable		= clk_nodrv_prepare_enable,
	.disable	= clk_nodrv_disable_unprepare,
	.prepare	= clk_nodrv_prepare_enable,
	.unprepare	= clk_nodrv_disable_unprepare,
	.determine_rate	= clk_nodrv_determine_rate,
	.set_rate	= clk_nodrv_set_rate,
	.set_parent	= clk_nodrv_set_parent,
};

static void clk_core_evict_parent_cache_subtree(struct clk_core *root,
						const struct clk_core *target)
{
	int i;
	struct clk_core *child;

	for (i = 0; i < root->num_parents; i++)
		if (root->parents[i].core == target)
			root->parents[i].core = NULL;

	hlist_for_each_entry(child, &root->children, child_node)
		clk_core_evict_parent_cache_subtree(child, target);
}

/* Remove this clk from all parent caches */
static void clk_core_evict_parent_cache(struct clk_core *core)
{
	const struct hlist_head **lists;
	struct clk_core *root;

	for (lists = all_lists; *lists; lists++)
		hlist_for_each_entry(root, *lists, child_node)
			clk_core_evict_parent_cache_subtree(root, core);

}

/**
 * clk_unregister - unregister a currently registered clock
 * @clk: clock to unregister
 */
void clk_unregister(struct clk *clk)
{
	unsigned long flags;
	const struct clk_ops *ops;

	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_prepare_lock();

	ops = clk->core->ops;
	if (ops == &clk_nodrv_ops) {
		pr_err("%s: unregistered clock: %s\n", __func__,
		       clk->core->name);
		clk_prepare_unlock();
		return;
	}
	/*
	 * Assign empty clock ops for consumers that might still hold
	 * a reference to this clock.
	 */
	flags = clk_enable_lock();
	clk->core->ops = &clk_nodrv_ops;
	clk_enable_unlock(flags);

	if (ops->terminate)
		ops->terminate(clk->core->hw);

	if (!hlist_empty(&clk->core->children)) {
		struct clk_core *child;
		struct hlist_node *t;

		/* Reparent all children to the orphan list. */
		hlist_for_each_entry_safe(child, t, &clk->core->children,
					  child_node)
			clk_core_set_parent_nolock(child, NULL);
	}

	clk_core_evict_parent_cache(clk->core);

	hlist_del(&clk->core->hashtable_node);
	hlist_del_init(&clk->core->child_node);

	if (clk->core->prepare_count)
		pr_warn("%s: unregistering prepared clock: %s\n",
					__func__, clk->core->name);

	clk_prepare_unlock();

	kref_put(&clk->core->ref, __clk_release);
	free_clk(clk);
}
EXPORT_SYMBOL_GPL(clk_unregister);

/**
 * clk_hw_unregister - unregister a currently registered clk_hw
 * @hw: hardware-specific clock data to unregister
 */
void clk_hw_unregister(struct clk_hw *hw)
{
	clk_unregister(hw->clk);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister);

static void devm_clk_unregister_cb(struct udevice *dev, void *res)
{
	clk_unregister(*(struct clk **)res);
}

static void devm_clk_hw_unregister_cb(struct udevice *dev, void *res)
{
	clk_hw_unregister(*(struct clk_hw **)res);
}

/**
 * devm_clk_register - resource managed clk_register()
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * Managed clk_register(). This function is *deprecated*, use devm_clk_hw_register() instead.
 *
 * Clocks returned from this function are automatically clk_unregister()ed on
 * driver detach. See clk_register() for more information.
 */
struct clk *devm_clk_register(struct udevice *dev, struct clk_hw *hw)
{
	struct clk *clk;
	struct clk **clkp;

	clkp = devres_alloc(devm_clk_unregister_cb, sizeof(*clkp), GFP_KERNEL);
	if (!clkp)
		return ERR_PTR(-ENOMEM);

	clk = clk_register(dev, hw);
	if (!IS_ERR(clk)) {
		*clkp = clk;
		devres_add(dev, clkp);
	} else {
		devres_free(clkp);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(devm_clk_register);

/**
 * devm_clk_hw_register - resource managed clk_hw_register()
 * @dev: device that is registering this clock
 * @hw: link to hardware-specific clock data
 *
 * Managed clk_hw_register(). Clocks registered by this function are
 * automatically clk_hw_unregister()ed on driver detach. See clk_hw_register()
 * for more information.
 */
int devm_clk_hw_register(struct udevice *dev, struct clk_hw *hw)
{
	struct clk_hw **hwp;
	int ret;

	hwp = devres_alloc(devm_clk_hw_unregister_cb, sizeof(*hwp), GFP_KERNEL);
	if (!hwp)
		return -ENOMEM;

	ret = clk_hw_register(dev, hw);
	if (!ret) {
		*hwp = hw;
		devres_add(dev, hwp);
	} else {
		devres_free(hwp);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_clk_hw_register);

static void devm_clk_release(struct udevice *dev, void *res)
{
	__clk_put(*(struct clk **)res);
}

/**
 * devm_clk_hw_get_clk - resource managed clk_hw_get_clk()
 * @dev: device that is registering this clock
 * @hw: clk_hw associated with the clk being consumed
 * @con_id: connection ID string on device
 *
 * Managed clk_hw_get_clk(). Clocks got with this function are
 * automatically clk_put() on driver detach. See clk_put()
 * for more information.
 */
struct clk *devm_clk_hw_get_clk(struct udevice *dev, struct clk_hw *hw,
				const char *con_id)
{
	struct clk *clk;
	struct clk **clkp;

	/* This should not happen because it would mean we have drivers
	 * passing around clk_hw pointers instead of having the caller use
	 * proper clk_get() style APIs
	 */
	WARN_ON_ONCE(dev != hw->core->dev);

	clkp = devres_alloc(devm_clk_release, sizeof(*clkp), GFP_KERNEL);
	if (!clkp)
		return ERR_PTR(-ENOMEM);

	clk = clk_hw_get_clk(hw, con_id);
	if (!IS_ERR(clk)) {
		*clkp = clk;
		devres_add(dev, clkp);
	} else {
		devres_free(clkp);
	}

	return clk;
}
EXPORT_SYMBOL_GPL(devm_clk_hw_get_clk);

/*
 * clkdev helpers
 */

void __clk_put(struct clk *clk)
{
	if (!clk || WARN_ON_ONCE(IS_ERR(clk)))
		return;

	clk_prepare_lock();

	clk_core_unlink_consumer(clk);

	clk_prepare_unlock();

	kref_put(&clk->core->ref, __clk_release);
	module_put(owner);
	free_clk(clk);
}

// U-Boot: from clkdev.c
void clk_put(struct clk *clk)
{
	__clk_put(clk);
}

static void clk_core_reparent_orphans(void)
{
	clk_prepare_lock();
	clk_core_reparent_orphans_nolock();
	clk_prepare_unlock();
}

/**
 * struct of_clk_provider - Clock provider registration structure
 * @link: Entry in global list of clock providers
 * @node: Pointer to device tree node of clock provider
 * @get: Get clock callback.  Returns NULL or a struct clk for the
 *       given clock specifier
 * @get_hw: Get clk_hw callback.  Returns NULL, ERR_PTR or a
 *       struct clk_hw for the given clock specifier
 * @data: context pointer to be passed into @get callback
 */
struct of_clk_provider {
	struct list_head link;

	ofnode node;
	struct clk *(*get)(struct ofnode_phandle_args *clkspec, void *data);
	struct clk_hw *(*get_hw)(struct ofnode_phandle_args *clkspec, void *data);
	void *data;
};

/*
 * List of ofnodes which provide clocks and associated data
 * registered by the device to retrieve the clock
 */
static LIST_HEAD(of_clk_providers);

struct clk *of_clk_src_simple_get(struct ofnode_phandle_args *clkspec,
				     void *data)
{
	return data;
}
EXPORT_SYMBOL_GPL(of_clk_src_simple_get);

struct clk_hw *of_clk_hw_simple_get(struct ofnode_phandle_args *clkspec, void *data)
{
	return data;
}
EXPORT_SYMBOL_GPL(of_clk_hw_simple_get);

struct clk *of_clk_src_onecell_get(struct ofnode_phandle_args *clkspec, void *data)
{
	struct clk_onecell_data *clk_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= clk_data->clk_num) {
		pr_err("%s: invalid clock index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return clk_data->clks[idx];
}
EXPORT_SYMBOL_GPL(of_clk_src_onecell_get);

struct clk_hw *
of_clk_hw_onecell_get(struct ofnode_phandle_args *clkspec, void *data)
{
	struct clk_hw_onecell_data *hw_data = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= hw_data->num) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return hw_data->hws[idx];
}
EXPORT_SYMBOL_GPL(of_clk_hw_onecell_get);

/**
 * of_clk_add_provider() - Register a clock provider for a node
 * @np: Device node pointer associated with clock provider
 * @clk_src_get: callback for decoding clock
 * @data: context pointer for @clk_src_get callback.
 *
 * This function is *deprecated*. Use of_clk_add_hw_provider() instead.
 */
int of_clk_add_provider(ofnode np,
			struct clk *(*clk_src_get)(struct ofnode_phandle_args *clkspec,
						   void *data),
			void *data)
{
	struct of_clk_provider *cp;
	int ret;

	if (!ofnode_valid(np))
		return 0;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->node = of_node_get(np);
	cp->data = data;
	cp->get = clk_src_get;

	mutex_lock(&of_clk_mutex);
	list_add(&cp->link, &of_clk_providers);
	mutex_unlock(&of_clk_mutex);
	pr_debug("Added clock from %pOF\n", &np);

	clk_core_reparent_orphans();

	ret = of_clk_set_defaults(np, true);
	if (ret < 0)
		of_clk_del_provider(np);

	return ret;
}
EXPORT_SYMBOL_GPL(of_clk_add_provider);

/**
 * of_clk_add_hw_provider() - Register a clock provider for a node
 * @np: Device node pointer associated with clock provider
 * @get: callback for decoding clk_hw
 * @data: context pointer for @get callback.
 */
int of_clk_add_hw_provider(ofnode np,
			   struct clk_hw *(*get)(struct ofnode_phandle_args *clkspec,
						 void *data),
			   void *data)
{
	struct of_clk_provider *cp;
	int ret;

	if (!ofnode_valid(np))
		return 0;

	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->node = of_node_get(np);
	cp->data = data;
	cp->get_hw = get;

	mutex_lock(&of_clk_mutex);
	list_add(&cp->link, &of_clk_providers);
	mutex_unlock(&of_clk_mutex);
	pr_debug("Added clk_hw provider from %pOF\n", &np);

	clk_core_reparent_orphans();

	ret = of_clk_set_defaults(np, true);
	if (ret < 0)
		of_clk_del_provider(np);

	return ret;
}
EXPORT_SYMBOL_GPL(of_clk_add_hw_provider);

static void devm_of_clk_release_provider(struct udevice *dev, void *res)
{
	of_clk_del_provider(*(ofnode *)res);
}

/*
 * We allow a child device to use its parent device as the clock provider node
 * for cases like MFD sub-devices where the child device driver wants to use
 * devm_*() APIs but not list the device in DT as a sub-node.
 */
static ofnode get_clk_provider_node(struct udevice *dev)
{
	ofnode np, parent_np;

	np = dev_ofnode(dev);
	parent_np = dev->parent ? dev_ofnode(dev->parent) : ofnode_null();

	if (!ofnode_has_property(np, "#clock-cells"))
		if (ofnode_has_property(parent_np, "#clock-cells"))
			np = parent_np;

	return np;
}

/**
 * devm_of_clk_add_hw_provider() - Managed clk provider node registration
 * @dev: Device acting as the clock provider (used for DT node and lifetime)
 * @get: callback for decoding clk_hw
 * @data: context pointer for @get callback
 *
 * Registers clock provider for given device's node. If the device has no DT
 * node or if the device node lacks of clock provider information (#clock-cells)
 * then the parent device's node is scanned for this information. If parent node
 * has the #clock-cells then it is used in registration. Provider is
 * automatically released at device exit.
 *
 * Return: 0 on success or an errno on failure.
 */
int devm_of_clk_add_hw_provider(struct udevice *dev,
			struct clk_hw *(*get)(struct ofnode_phandle_args *clkspec,
					      void *data),
			void *data)
{
	ofnode *ptr, np;
	int ret;

	ptr = devres_alloc(devm_of_clk_release_provider, sizeof(*ptr),
			   GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	np = get_clk_provider_node(dev);
	ret = of_clk_add_hw_provider(np, get, data);
	if (!ret) {
		*ptr = np;
		devres_add(dev, ptr);
	} else {
		devres_free(ptr);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(devm_of_clk_add_hw_provider);

/**
 * of_clk_del_provider() - Remove a previously registered clock provider
 * @np: Device node pointer associated with clock provider
 */
void of_clk_del_provider(ofnode np)
{
	struct of_clk_provider *cp;

	if (!ofnode_valid(np))
		return;

	mutex_lock(&of_clk_mutex);
	list_for_each_entry(cp, &of_clk_providers, link) {
		if (ofnode_equal(cp->node, np)) {
			list_del(&cp->link);
			ofnode_put(cp->node);
			kfree(cp);
			break;
		}
	}
	mutex_unlock(&of_clk_mutex);
}
EXPORT_SYMBOL_GPL(of_clk_del_provider);

/**
 * of_parse_clkspec() - Parse a DT clock specifier for a given device node
 * @np: device node to parse clock specifier from
 * @index: index of phandle to parse clock out of. If index < 0, @name is used
 * @name: clock name to find and parse. If name is NULL, the index is used
 * @out_args: Result of parsing the clock specifier
 *
 * Parses a device node's "clocks" and "clock-names" properties to find the
 * phandle and cells for the index or name that is desired. The resulting clock
 * specifier is placed into @out_args, or an errno is returned when there's a
 * parsing error. The @index argument is ignored if @name is non-NULL.
 *
 * Example:
 *
 * phandle1: clock-controller@1 {
 *	#clock-cells = <2>;
 * }
 *
 * phandle2: clock-controller@2 {
 *	#clock-cells = <1>;
 * }
 *
 * clock-consumer@3 {
 *	clocks = <&phandle1 1 2 &phandle2 3>;
 *	clock-names = "name1", "name2";
 * }
 *
 * To get a device_node for `clock-controller@2' node you may call this
 * function a few different ways:
 *
 *   of_parse_clkspec(clock-consumer@3, -1, "name2", &args);
 *   of_parse_clkspec(clock-consumer@3, 1, NULL, &args);
 *   of_parse_clkspec(clock-consumer@3, 1, "name2", &args);
 *
 * Return: 0 upon successfully parsing the clock specifier. Otherwise, -ENOENT
 * if @name is NULL or -EINVAL if @name is non-NULL and it can't be found in
 * the "clock-names" property of @np.
 */
static int of_parse_clkspec(ofnode np, int index,
			    const char *name, struct ofnode_phandle_args *out_args)
{
	int ret = -ENOENT;

	/* Walk up the tree of devices looking for a clock property that matches */
	while (ofnode_valid(np) && !ofnode_equal(np, ofnode_root())) {
		/*
		 * For named clocks, first look up the name in the
		 * "clock-names" property.  If it cannot be found, then index
		 * will be an error code and of_parse_phandle_with_args() will
		 * return -EINVAL.
		 */
		if (name)
			index = ofnode_stringlist_search(np, "clock-names", name);
		ret = ofnode_parse_phandle_with_args(np, "clocks", "#clock-cells", 0,
						 index, out_args);
		if (!ret)
			break;
		if (name && index >= 0)
			break;

		/*
		 * No matching clock found on this node.  If the parent node
		 * has a "clock-ranges" property, then we can try one of its
		 * clocks.
		 */
		np = ofnode_get_parent(np);
		if (ofnode_valid(np) && !ofnode_has_property(np, "clock-ranges"))
			break;
		index = 0;
	}

	return ret;
}

static struct clk_hw *
__of_clk_get_hw_from_provider(struct of_clk_provider *provider,
			      struct ofnode_phandle_args *clkspec)
{
	struct clk *clk;

	if (provider->get_hw)
		return provider->get_hw(clkspec, provider->data);

	clk = provider->get(clkspec, provider->data);
	if (IS_ERR(clk))
		return ERR_CAST(clk);
	return __clk_get_hw(clk);
}

/* U-Boot: export so we can call this from clk-uclass */
struct clk_hw *of_clk_get_hw_from_clkspec(struct ofnode_phandle_args *clkspec)
{
	struct of_clk_provider *provider;
	struct clk_hw *hw = ERR_PTR(-EPROBE_DEFER);
	struct udevice *dev;
	int ret;

	if (!clkspec)
		return ERR_PTR(-EINVAL);

	/* Check if node in clkspec is in disabled/fail state */
	if (!ofnode_is_enabled(clkspec->node))
		return ERR_PTR(-ENOENT);

	/* U-Boot: we need to probe devices on-demand so they register their clocks */
	ret = device_get_global_by_ofnode(clkspec->node, &dev);
	if (ret) {
		pr_debug("Couldn't probe clock device %pOF: %d\n", &clkspec->node, ret);
		return ERR_PTR(ret);
	}

	mutex_lock(&of_clk_mutex);
	list_for_each_entry(provider, &of_clk_providers, link) {
		if (ofnode_equal(provider->node,clkspec->node)) {
			hw = __of_clk_get_hw_from_provider(provider, clkspec);
			if (!IS_ERR(hw))
				break;
		}
	}
	mutex_unlock(&of_clk_mutex);

	if (!IS_ERR(hw) && !hw->core->dev && !IS_ERR_OR_NULL(dev))
		hw->core->dev = dev;

	return hw;
}

/**
 * of_clk_get_from_provider() - Lookup a clock from a clock provider
 * @clkspec: pointer to a clock specifier data structure
 *
 * This function looks up a struct clk from the registered list of clock
 * providers, an input is a clock specifier data structure as returned
 * from the of_parse_phandle_with_args() function call.
 */
struct clk *of_clk_get_from_provider(struct ofnode_phandle_args *clkspec)
{
	struct clk_hw *hw = of_clk_get_hw_from_clkspec(clkspec);

	return clk_hw_create_clk(NULL, hw, NULL, __func__);
}
EXPORT_SYMBOL_GPL(of_clk_get_from_provider);

struct clk_hw *of_clk_get_hw(ofnode np, int index,
			     const char *con_id)
{
	int ret;
	struct clk_hw *hw;
	struct ofnode_phandle_args clkspec;

	ret = of_parse_clkspec(np, index, con_id, &clkspec);
	if (ret)
		return ERR_PTR(ret);

	hw = of_clk_get_hw_from_clkspec(&clkspec);
	ofnode_put(clkspec.node);

	return hw;
}

static struct clk *__of_clk_get(ofnode np,
				int index, const char *dev_id,
				const char *con_id)
{
	struct clk_hw *hw = of_clk_get_hw(np, index, con_id);

	return clk_hw_create_clk(NULL, hw, dev_id, con_id);
}

struct clk *of_clk_get(ofnode np, int index)
{
	return __of_clk_get(np, index, ofnode_get_name(np), NULL);
}
EXPORT_SYMBOL(of_clk_get);

/**
 * of_clk_get_by_name() - Parse and lookup a clock referenced by a device node
 * @np: pointer to clock consumer node
 * @name: name of consumer's clock input, or NULL for the first clock reference
 *
 * This function parses the clocks and clock-names properties,
 * and uses them to look up the struct clk from the registered list of clock
 * providers.
 */
struct clk *of_clk_get_by_name(ofnode np, const char *name)
{
	if (!ofnode_valid(np))
		return ERR_PTR(-ENOENT);

	return __of_clk_get(np, 0, ofnode_get_name(np), name);
}
EXPORT_SYMBOL(of_clk_get_by_name);

/**
 * of_clk_get_parent_count() - Count the number of clocks a device node has
 * @np: device node to count
 *
 * Returns: The number of clocks that are possible parents of this node
 */
unsigned int of_clk_get_parent_count(const ofnode np)
{
	int count;

	count = ofnode_count_phandle_with_args(np, "clocks", "#clock-cells", 0);
	if (count < 0)
		return 0;

	return count;
}
EXPORT_SYMBOL_GPL(of_clk_get_parent_count);

#if CONFIG_IS_ENABLED(UNIT_TEST)
void clk_ccf_reset(void)
{
	clk_root_list.first = NULL;
	clk_orphan_list.first = NULL;
	clk_hashtable.first = NULL;
	of_clk_providers.next = of_clk_providers.prev = &of_clk_providers;
}
#endif

static void clk_summary_show_one(struct clk_core *c,
				 int level)
{
	// struct clk *clk_user;
	// int multi_node = 0;

	printf("%*s%-*s %-7d %-11lu",
		   level * 3 + 1, "",
		   35 - level * 3, c->name,
		   c->enable_count,
		   clk_core_get_rate_recalc(c));

	if (c->ops->is_enabled)
		printf(" %5c ", clk_core_is_enabled(c) ? 'Y' : 'N');
	else if (!c->ops->enable)
		printf(" %5c ", 'Y');
	else
		printf(" %5c ", '?');

	printf("\n");

	// hlist_for_each_entry(clk_user, &c->clks, clks_node) {
	// 	if (!clk_user->dev || !clk_user->dev->name)
	// 		continue;

	// 	printf("%*s%-*s\n",
	// 		   level * 3 + 2 + 105 * multi_node, "",
	// 		   30,
	// 		   clk_user->dev->name);

	// 	multi_node = 1;
	// }

}

static void clk_summary_show_subtree(struct clk_core *c,
				     int level)
{
	struct clk_core *child;

	clk_summary_show_one(c, level);

	hlist_for_each_entry(child, &c->children, child_node)
		clk_summary_show_subtree(child, level + 1);
}

int clk_summary_show(void)
{
	struct clk_core *c;
	const struct hlist_head **lists = all_lists;

	printf("                                 enable              hardware           \n");
	printf("   clock                          count        rate    enable   consumer\n");
	printf("------------------------------------------------------------------------\n");

	for (; *lists; lists++)
		hlist_for_each_entry(c, *lists, child_node)
			clk_summary_show_subtree(c, 0);

	return 0;
}

/* Exposed for sandbox DM tests only! */
#if CONFIG_UNIT_TEST
int clk_get_enable_count(struct clk *clk)
{
	return clk->core->enable_count;
}

void __clk_mark_critical(struct clk *clk, bool critical)
{
	if (critical)
		clk->core->flags |= CLK_IS_CRITICAL;
	else
	 	clk->core->flags &= ~CLK_IS_CRITICAL;
}
#endif
