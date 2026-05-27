/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2014 Samsung Electronics Co., Ltd.
 * Sylwester Nawrocki <s.nawrocki@samsung.com>
 */

#ifndef __CLK_CONF_H
#define __CLK_CONF_H

#include <dm/ofnode.h>
#include <linux/types.h>

#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
int of_clk_set_defaults(ofnode node, bool clk_supplier);
#else
int of_clk_set_defaults(ofnode node, bool clk_supplier)
{
	return -ENOSYS;
}
#endif

#endif /* __CLK_CONF_H */
