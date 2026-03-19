// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019
 * Lukasz Majewski, DENX Software Engineering, lukma@denx.de
 */

#include <clk.h>
#include <dm.h>
#include <asm/clk.h>
#include <dm/test.h>
#include <dm/uclass.h>
#include <linux/err.h>
#include <test/test.h>
#include <test/ut.h>
#include <sandbox-clk.h>

#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
/* clk_get_by_id() is nonsensical for CCF_FULL */
static int clk_get_by_dev_id(struct udevice *dev, ulong id, struct clk **clk)
{
	struct clk_hw *hw;
	struct ofnode_phandle_args clkspec = {
		.node = dev_ofnode(dev),
		.args_count = 1,
		.args = {
			id,
		}
	};
	hw = of_clk_get_hw_from_clkspec(&clkspec);
	if (IS_ERR_OR_NULL(hw))
		return PTR_ERR(hw);
	*clk = hw->clk;
	return 0;
}

void __clk_mark_critical(struct clk *clk, bool critical);
#else

const char *__clk_get_name(struct clk *clk)
{
	return clk->dev->name;
}

static int clk_get_by_dev_id(struct udevice *dev, ulong id, struct clk **clk)
{
	return clk_get_by_id(CLK_ID(dev, id), clk);
}

void __clk_mark_critical(struct clk *clk, bool critical)
{
	if (critical)
		clk->flags |= CLK_IS_CRITICAL;
	else
	 	clk->flags &= ~CLK_IS_CRITICAL;
}
#endif


/* Tests for Common Clock Framework driver */
static int dm_test_clk_ccf(struct unit_test_state *uts)
{
	struct clk *clk, *pclk;
	struct udevice *dev, *test_dev;
	long long rate;
	int ret;
	enum uclass_id clk_uc = UCLASS_CLK;
#if CONFIG_IS_ENABLED(CLK_CCF) || CONFIG_IS_ENABLED(CLK_CCF_FULL)
	struct clk clk_ccf;
	const char *clkname;
	int clkid, i;
#endif

	/* CCF clocks use UCLASS_NOP to differentiate */
	if (CONFIG_IS_ENABLED(CLK_CCF_FULL))
		clk_uc = UCLASS_NOP;

	/* Get the device using the clk device */
	ut_assertok(uclass_get_device_by_name(clk_uc, "clk-ccf", &dev));
	ut_assertok(uclass_get_device_by_name(UCLASS_MISC, "clk-test", &test_dev));

	/* Test for clk_get_by_id() */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_ECSPI_ROOT, &clk);
	ut_assertok(ret);
	ut_asserteq_str("ecspi_root", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_PARENT, clk->flags);

	/* Test for clk_get_parent_rate() */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_ECSPI1, &clk);
	ut_assertok(ret);
	ut_asserteq_str("ecspi1", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_PARENT, clk->flags);

	rate = clk_get_parent_rate(clk);
	ut_asserteq_64(20000000, rate);

	/* test the gate of CCF */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_ECSPI0, &clk);
	ut_assertok(ret);
	ut_asserteq_str("ecspi0", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_PARENT, clk->flags);

	rate = clk_get_parent_rate(clk);
	ut_asserteq_64(20000000, rate);

	/* Test the mux of CCF */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_USDHC1_SEL, &clk);
	ut_assertok(ret);
	ut_asserteq_str("usdhc1_sel", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_NO_REPARENT, clk->flags);

	rate = clk_get_parent_rate(clk);
	ut_asserteq_64(60000000, rate);

	rate = clk_set_rate(clk, 60000000);
#if !CONFIG_IS_ENABLED(CLK_CCF_FULL)
	ut_asserteq_64((u64)-ENOSYS, rate);
#endif

	rate = clk_get_rate(clk);
	ut_asserteq_64(60000000, rate);

	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_PLL3_80M, &pclk);
	ut_assertok(ret);

	ret = clk_set_parent(clk, pclk);
	ut_assertok(ret);

	rate = clk_get_rate(clk);
	ut_asserteq_64(80000000, rate);

	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_USDHC2_SEL, &clk);
	ut_assertok(ret);
	ut_asserteq_str("usdhc2_sel", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_NO_REPARENT, clk->flags);

	rate = clk_get_parent_rate(clk);
	ut_asserteq_64(80000000, rate);

	pclk = clk_get_parent(clk);
	ut_asserteq_str("pll3_80m", __clk_get_name(pclk));
	ut_asserteq(CLK_SET_RATE_PARENT, pclk->flags);

	rate = clk_set_rate(clk, 80000000);
#if !CONFIG_IS_ENABLED(CLK_CCF_FULL)
	ut_asserteq_64((u64)-ENOSYS, rate);
#endif

	rate = clk_get_rate(clk);
	ut_asserteq_64(80000000, rate);

	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_PLL3_60M, &pclk);
	ut_assertok(ret);

	ret = clk_set_parent(clk, pclk);
	ut_assertok(ret);

	rate = clk_get_rate(clk);
	ut_asserteq_64(60000000, rate);

	/* Test the composite of CCF */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_I2C, &clk);
	ut_assertok(ret);
	ut_asserteq_str("i2c2", __clk_get_name(clk));
	ut_asserteq(CLK_SET_RATE_UNGATE, clk->flags);

	rate = clk_get_rate(clk);
	// ut_asserteq_64(60000000, rate);

	rate = clk_set_rate(clk, 60000000);
	ut_asserteq_64(60000000, rate);

#if CONFIG_IS_ENABLED(CLK_CCF) || CONFIG_IS_ENABLED(CLK_CCF_FULL)
	/* Test clk tree enable/disable */

	ret = clk_get_by_index(test_dev, SANDBOX_CLK_TEST_ID_I2C_ROOT, &clk_ccf);
	ut_assertok(ret);
	ut_asserteq_str("clk-ccf", clk_ccf.dev->name);
#if !CONFIG_IS_ENABLED(CLK_CCF_FULL)
	ut_asserteq(CLK_ID(clk_ccf.dev, SANDBOX_CLK_I2C_ROOT), clk_ccf.id);
#endif

	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_I2C_ROOT, &clk);
	ut_assertok(ret);
	ut_asserteq_str("i2c_root", __clk_get_name(clk));
#if !CONFIG_IS_ENABLED(CLK_CCF_FULL)
	ut_asserteq(SANDBOX_CLK_I2C_ROOT, clk_get_id(clk));
#endif

	ret = clk_enable(&clk_ccf);
	ut_assertok(ret);

	ret = sandbox_clk_enable_count(clk);
	ut_asserteq(1, ret);

	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_I2C, &pclk);
	ut_assertok(ret);

	ret = sandbox_clk_enable_count(pclk);
	ut_asserteq(1, ret);

	ret = clk_disable(clk);
	ut_assertok(ret);

	ret = sandbox_clk_enable_count(clk);
	ut_asserteq(0, ret);

	ret = sandbox_clk_enable_count(pclk);
	ut_asserteq(0, ret);

	/* Test clock re-parenting. */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_USDHC1_SEL, &clk);
	ut_assertok(ret);
	ut_asserteq_str("usdhc1_sel", __clk_get_name(clk));

	pclk = clk_get_parent(clk);
	ut_assertok_ptr(pclk);
	if (!strcmp(__clk_get_name(pclk), "pll3_60m")) {
		clkname = "pll3_80m";
		clkid = SANDBOX_CLK_PLL3_80M;
	} else {
		clkname = "pll3_60m";
		clkid = SANDBOX_CLK_PLL3_60M;
	}

	ret = clk_get_by_dev_id(dev, clkid, &pclk);
	ut_assertok(ret);
	ret = clk_set_parent(clk, pclk);
	ut_assertok(ret);
	pclk = clk_get_parent(clk);
	ut_assertok_ptr(pclk);
	ut_asserteq_str(clkname, __clk_get_name(pclk));

	/* Test disabling critical clock. */
	ret = clk_get_by_dev_id(dev, SANDBOX_CLK_I2C_ROOT, &clk);
	ut_assertok(ret);
	ut_asserteq_str("i2c_root", __clk_get_name(clk));

	/* Disable it, if any. */
	ret = sandbox_clk_enable_count(clk);
	for (i = 0; i < ret; i++) {
		ret = clk_disable(clk);
		ut_assertok(ret);
	}

	ret = sandbox_clk_enable_count(clk);
	ut_asserteq(0, ret);

	__clk_mark_critical(clk, true);
	ret = clk_enable(clk);
	ut_assertok(ret);

	ret = clk_disable(clk);
	ut_assertok(ret);
	ret = sandbox_clk_enable_count(clk);
	ut_asserteq(1, ret);
	__clk_mark_critical(clk, false);

	ret = clk_disable(clk);
	ut_assertok(ret);
	ret = sandbox_clk_enable_count(clk);
	ut_asserteq(0, ret);
#endif

	return 1;
}
DM_TEST(dm_test_clk_ccf, UTF_SCAN_FDT);
