// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Google, Inc
 * Written by Simon Glass <sjg@chromium.org>
 * Copyright (c) 2016, NVIDIA CORPORATION.
 * Copyright (c) 2018, Theobroma Systems Design und Consulting GmbH
 *
 * Common clock support code shared between CCF_FULL and UCLASS_CLK
 */

#include <clk.h>
#include <clk-uclass.h>
#include <dm.h>
#include <dt-structs.h>
#include <log.h>
#include <malloc.h>
#include <asm/global_data.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/devres.h>
#include <dm/read.h>
#include <linux/bug.h>
#include <linux/clk-provider.h>
#include <linux/err.h>

#include <linux/clk/clk-conf.h>
#include "ccf/clk.h"
#include "clk-common.h"

#if CONFIG_IS_ENABLED(OF_REAL)
int clk_get_by_index(struct udevice *dev, int index, struct clk *clk)
{
	return clk_get_by_index_nodev(dev_ofnode(dev), index, clk);
}

int clk_get_by_index_nodev(ofnode node, int index, struct clk *clk)
{
	if (CONFIG_IS_ENABLED(CLK_CCF_FULL)) {
		struct clk_hw *hw;

		hw = of_clk_get_hw(node, index, NULL);
		return clk_hw_create_clk_uboot(clk, hw);
	} else {
		struct ofnode_phandle_args args;
		int ret;

		ret = ofnode_parse_phandle_with_args(node, "clocks", "#clock-cells", 0,
						index, &args);

		return clk_get_by_index_tail(ret, node, &args, "clocks",
					index, clk);
	}
}

int clk_get_bulk(struct udevice *dev, struct clk_bulk *bulk)
{
	int i, ret, err, count;

	bulk->count = 0;

	count = dev_count_phandle_with_args(dev, "clocks", "#clock-cells", 0);
	if (count < 1)
		return count;

	bulk->clks = devm_kcalloc(dev, count, sizeof(struct clk), GFP_KERNEL);
	if (!bulk->clks)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		ret = clk_get_by_index(dev, i, &bulk->clks[i]);
		if (ret < 0)
			goto bulk_get_err;

		++bulk->count;
	}

	return 0;

bulk_get_err:
	err = clk_release_all(bulk->clks, bulk->count);
	if (err)
		debug("%s: could not release all clocks for %s\n",
		      __func__, dev_read_name(dev));

	return ret;
}

int clk_get_by_name(struct udevice *dev, const char *name, struct clk *clk)
{
	if (CONFIG_IS_ENABLED(CLK_CCF_FULL)) {
		struct clk_hw *hw;

		hw = of_clk_get_hw(dev_ofnode(dev), 0, name);
		if (IS_ERR(hw))
			return -ENOENT;
		return clk_hw_create_clk_uboot(clk, hw);
	} else {
		return clk_get_by_name_nodev(dev_ofnode(dev), name, clk);
	}
}
#endif /* OF_REAL */

int clk_get_by_name_nodev(ofnode node, const char *name, struct clk *clk)
{
	int index = 0;

	debug("%s(node=%s, name=%s, clk=%p)\n", __func__,
		ofnode_get_name(node), name, clk);
	clk->dev = NULL;

	if (name) {
		index = ofnode_stringlist_search(node, "clock-names", name);
		if (index < 0) {
			debug("fdt_stringlist_search() failed: %d\n", index);
			return index;
		}
	}

	return clk_get_by_index_nodev(node, index, clk);
}

int clk_release_all(struct clk *clk, unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		debug("%s(clk[%u]=%p)\n", __func__, i, &clk[i]);

		/* check if clock has been previously requested */
		if (!clk[i].dev)
			continue;

		ret = clk_disable(&clk[i]);
		if (ret && ret != -ENOSYS)
			return ret;
	}

	return 0;
}


int clk_set_defaults(struct udevice *dev, enum clk_defaults_stage stage)
{
	int ret;

	if (!dev_has_ofnode(dev))
		return 0;

	/*
	 * To avoid setting defaults twice, don't set them before relocation.
	 * However, still set them for SPL. And still set them if explicitly
	 * asked.
	 */
	if (!(IS_ENABLED(CONFIG_XPL_BUILD) || (gd->flags & GD_FLG_RELOC)))
		if (stage != CLK_DEFAULTS_POST_FORCE)
			return 0;

	debug("%s(%s)\n", __func__, dev_read_name(dev));

	if (CONFIG_IS_ENABLED(CLK_CCF_FULL)) {
		return of_clk_set_defaults(dev_ofnode(dev), false);
	} else {
		ret = clk_set_default_parents(dev, stage);
		if (ret)
			return ret;

		ret = clk_set_default_rates(dev, stage);
		if (ret < 0)
			return ret;
	}

	return 0;
}

struct clk *devm_clk_get(struct udevice *dev, const char *id)
{
	int rc;
	struct clk *clk;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (unlikely(!clk))
		return ERR_PTR(-ENOMEM);

	rc = clk_get_by_name(dev, id, clk);
	if (rc)
		return ERR_PTR(rc);

	return clk;
}

int clk_prepare_bulk(struct clk_bulk *bulk)
{
	int i, ret;

	for (i = 0; i < bulk->count; i++) {
		ret = clk_prepare(&bulk->clks[i]);
		if (ret < 0 && ret != -ENOSYS)
			return ret;
	}

	return 0;
}

int clk_unprepare_bulk(struct clk_bulk *bulk)
{
	int i, ret;

	for (i = 0; i < bulk->count; i++) {
		ret = clk_prepare(&bulk->clks[i]);
		if (ret < 0 && ret != -ENOSYS)
			return ret;
	}

	return 0;
}

int clk_enable_bulk(struct clk_bulk *bulk)
{
	int i, ret;

	for (i = 0; i < bulk->count; i++) {
		ret = clk_enable(&bulk->clks[i]);
		if (ret < 0 && ret != -ENOSYS)
			return ret;
	}

	return 0;
}

int clk_disable_bulk(struct clk_bulk *bulk)
{
	int i, ret;

	for (i = 0; i < bulk->count; i++) {
		ret = clk_disable(&bulk->clks[i]);
		if (ret < 0 && ret != -ENOSYS)
			return ret;
	}

	return 0;
}

#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
long clk_get_parent_rate(struct clk *clk)
{
	struct clk *pclk;

	if (!clk)
		return -EINVAL;

	pclk = clk_get_parent(clk);
	return clk_get_rate(pclk);
}
#endif
