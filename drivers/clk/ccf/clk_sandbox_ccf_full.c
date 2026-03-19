// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2019
 * Lukasz Majewski, DENX Software Engineering, lukma@denx.de
 *
 * Common Clock Framework [CCF] driver for Sandbox
 */

#include <dm.h>
#include <clk.h>
#include <malloc.h>
#include <asm/clk.h>
#include <clk-uclass.h>
#include <dm/devres.h>
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <sandbox-clk.h>
#include <linux/err.h>

/*
 * Sandbox implementation of CCF primitives necessary for clk-uclass testing
 *
 * --- Sandbox PLLv3 ---
 */
struct clk_pllv3 {
	struct clk_hw	clk;
	u32		div_mask;
	u32		div_shift;
};

#if CONFIG_IS_ENABLED(UNIT_TEST)
int clk_get_enable_count(struct clk *clk);
#else
#define clk_get_enable_count(clk) 0
#endif

int sandbox_clk_enable_count(struct clk *clk)
{
	return clk_get_enable_count(clk);
}

static ulong clk_pllv3_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return parent_rate * 24;
}

static const struct clk_ops clk_pllv3_generic_ops = {
	.recalc_rate       = clk_pllv3_recalc_rate,
};

static struct clk_hw *sb_clk_pllv3(enum sandbox_pllv3_type type, ofnode node, const char *name,
			      const char *parent_name, void __iomem *base,
			      u32 div_mask)
{
	struct clk_pllv3 *pll;
	struct clk_hw *hw;
	struct clk_init_data init = { 0 };
	int ret;

	pll = kzalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	pll->div_mask = div_mask;
	hw = &pll->clk;
	hw->init = &init;

	init.name = name;
	init.ops = &clk_pllv3_generic_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_data = &(struct clk_parent_data){
		.fw_name = parent_name,
	};
	init.num_parents = 1;

	ret = of_clk_hw_register(node, hw);
	if (ret) {
		kfree(pll);
		return ERR_PTR(ret);
	}

	return hw;
}

/* --- Sandbox PLLv3 --- */

struct clk_hw *sandbox_clk_composite(const char *name,
				  const char * const *parent_names,
				  int num_parents, void __iomem *reg,
				  unsigned long flags)
{
	struct clk_hw *hw = ERR_PTR(-ENOMEM);
	struct clk_divider *div = NULL;
	struct clk_gate *gate = NULL;
	struct clk_mux *mux = NULL;

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		goto fail;

	mux->reg = reg;
	mux->shift = 24;
	mux->mask = 0x7;
	mux->flags = flags;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		goto fail;

	div->reg = reg;
	div->shift = 16;
	div->width = 3;
	div->flags = CLK_DIVIDER_ROUND_CLOSEST | flags;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		goto fail;

	gate->reg = reg;
	gate->bit_idx = 28;
	gate->flags = flags;

	hw = clk_hw_register_composite(NULL, name,
				     parent_names, num_parents,
				     &mux->hw, &clk_mux_ops, &div->hw,
				     &clk_divider_ro_ops,
				     &gate->hw, &clk_gate_ops, flags);
	if (IS_ERR(hw))
		goto fail;

	return hw;

fail:
	kfree(gate);
	kfree(div);
	kfree(mux);
	return ERR_CAST(hw);
}

/* --- Sandbox Gate --- */
/* The CCF core driver itself */
static const struct udevice_id sandbox_clk_ccf_test_ids[] = {
	{ .compatible = "sandbox,clk-ccf" },
	{ }
};

static const char *const usdhc_sels[] = { "pll3_60m", "pll3_80m", };
static const char *const i2c_sels[] = { "pll3_60m", "pll3_80m", };

static int sandbox_clk_ccf_probe(struct udevice *dev)
{
	ofnode node = dev_ofnode(dev);
	struct clk_hw_onecell_data *data;
	struct clk_hw **clks;
	void *base = NULL;
	u32 reg;
	u32 *clk_regs;

	data = devm_kzalloc(dev, sizeof(*data) + sizeof(struct clk_hw) * SANDBOX_CLK_MAX, __GFP_ZERO);
	if (!data)
		return -ENOMEM;

	clk_regs = devm_kzalloc(dev, sizeof(u32) * SANDBOX_CLK_MAX, __GFP_ZERO);
	if (!clk_regs)
		return -ENOMEM;

	data->num = SANDBOX_CLK_MAX;
	clks = data->hws;

	clks[SANDBOX_CLK_PLL3] = sb_clk_pllv3(SANDBOX_PLLV3_USB, node, "pll3_usb_otg", "osc",
				     base + 0x10, 0x3);

	clks[SANDBOX_CLK_PLL3_60M] =
		clk_hw_register_fixed_factor_parent_hw(dev, "pll3_60m",
						       clks[SANDBOX_CLK_PLL3],
						       CLK_SET_RATE_PARENT, 1, 8);
	
	clks[SANDBOX_CLK_PLL3_80M] =
		clk_hw_register_fixed_factor_parent_hw(dev, "pll3_80m",
						       clks[SANDBOX_CLK_PLL3],
						       CLK_SET_RATE_PARENT, 1, 6);

	/* The HW adds +1 to the divider value (2+1) is the divider */
	clk_regs[SANDBOX_CLK_ECSPI_ROOT] = (2 << 19);
	clks[SANDBOX_CLK_ECSPI_ROOT] = clk_hw_register_divider_parent_hw(dev, "ecspi_root", clks[SANDBOX_CLK_PLL3_60M],
									 CLK_SET_RATE_PARENT, &clk_regs[SANDBOX_CLK_ECSPI_ROOT], 19, 6, CLK_DIVIDER_READ_ONLY);
	clk_regs[SANDBOX_CLK_ECSPI0] = 0;
	clks[SANDBOX_CLK_ECSPI0] = clk_hw_register_gate_parent_hw(dev, "ecspi0", clks[SANDBOX_CLK_ECSPI_ROOT],
								  CLK_SET_RATE_PARENT, &clk_regs[SANDBOX_CLK_ECSPI0], 0, 0);

	clks[SANDBOX_CLK_ECSPI1] = clk_hw_register_gate_parent_hw(dev, "ecspi1", clks[SANDBOX_CLK_ECSPI_ROOT], CLK_SET_RATE_PARENT, base + 0x6c, 0, 0);

	/* Select 'pll3_60m' */
	reg = 0;
	clk_regs[SANDBOX_CLK_USDHC1_SEL] = 0;
	clks[SANDBOX_CLK_USDHC1_SEL] = clk_hw_register_mux(dev, "usdhc1_sel", usdhc_sels, ARRAY_SIZE(usdhc_sels),
							   CLK_SET_RATE_NO_REPARENT, &clk_regs[SANDBOX_CLK_USDHC1_SEL], 16, 1, 0);

	/* Select 'pll3_80m' */
	clk_regs[SANDBOX_CLK_USDHC2_SEL] = BIT(17);
	clks[SANDBOX_CLK_USDHC2_SEL] = clk_hw_register_mux(dev, "usdhc2_sel", usdhc_sels, ARRAY_SIZE(usdhc_sels),
							   CLK_SET_RATE_NO_REPARENT, &clk_regs[SANDBOX_CLK_USDHC2_SEL], 17, 1, 0);

	clk_regs[SANDBOX_CLK_I2C] = BIT(28) | BIT(24) | BIT(16);
	clks[SANDBOX_CLK_I2C] = sandbox_clk_composite("i2c2", i2c_sels, ARRAY_SIZE(i2c_sels),
					 &clk_regs[SANDBOX_CLK_I2C], CLK_SET_RATE_UNGATE);
		   

	clk_regs[SANDBOX_CLK_I2C_ROOT] = 0;
	clks[SANDBOX_CLK_I2C_ROOT] = clk_hw_register_gate(dev, "i2c_root", "i2c2", 0, &clk_regs[SANDBOX_CLK_I2C_ROOT], 0, 0);

	return of_clk_add_hw_provider(node, of_clk_hw_onecell_get, data);
}

U_BOOT_DRIVER(sandbox_clk_ccf_full) = {
	.name = "sandbox_clk_ccf_full",
	.id = UCLASS_NOP,
	.probe = sandbox_clk_ccf_probe,
	.of_match = sandbox_clk_ccf_test_ids,
};
