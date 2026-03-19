/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 * Sylwester Nawrocki <s.nawrocki@samsung.com>
 */

#ifndef __CLK_CCF_CLK_H__
#define __CLK_CCF_CLK_H__

#include <dm/ofnode.h>

struct clk_hw;
struct udevice;
struct of_phandle_args;

#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
struct clk_hw *of_clk_get_hw(ofnode np,
				    int index, const char *con_id);

struct clk *clk_hw_create_clk(struct udevice *dev, struct clk_hw *hw,
			      const char *dev_id, const char *con_id);
int clk_hw_create_clk_uboot(struct clk *clk, struct clk_hw *hw);
void __clk_put(struct clk *clk);
#else
struct clk_hw *of_clk_get_hw(ofnode np,
				    int index, const char *con_id)
{
	return NULL;
}
struct clk *clk_hw_create_clk(struct udevice *dev, struct clk_hw *hw,
			      const char *dev_id, const char *con_id)
{
	return NULL;
}
int clk_hw_create_clk_uboot(struct clk *clk, struct clk_hw *hw)
{
	return -ENOSYS;
}
void __clk_put(struct clk *clk) { }
#endif

// Linux compat

#define clk_prepare_lock()
#define clk_prepare_unlock()

static inline unsigned long clk_enable_lock(void) {
	return 0;
}

static inline void clk_enable_unlock(unsigned long flags) { }

#endif /* __CLK_CCF_CLK_H__ */
