
#include <dm/ofnode.h>
#include <clk.h>

#ifndef __CLK_COMMON_H__
#define __CLK_COMMON_H__

/* These functions are unused with CCF_FULL */

#if !CONFIG_IS_ENABLED(CLK_CCF_FULL)
int clk_get_by_index_tail(int ret, ofnode node,
				 struct ofnode_phandle_args *args,
				 const char *list_name, int index,
				 struct clk *clk);

int clk_set_default_parents(struct udevice *dev,
				   enum clk_defaults_stage stage);


int clk_set_default_rates(struct udevice *dev,
				 enum clk_defaults_stage stage);
#else
int clk_get_by_index_tail(int ret, ofnode node,
				 struct ofnode_phandle_args *args,
				 const char *list_name, int index,
				 struct clk *clk)
{
	return -ENOSYS;
}

int clk_set_default_parents(struct udevice *dev,
				   enum clk_defaults_stage stage)
{
	return -ENOSYS;
}

int clk_set_default_rates(struct udevice *dev,
				 enum clk_defaults_stage stage)
{
	return -ENOSYS;
}

#endif /* CONFIG_CLK_CCF_FULL */

#endif /* __CLK_COMMON_H__ */
