// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2015 Google, Inc
 */

#include <clk-uclass.h>
#include <dm.h>
#include <dm/devres.h>
#include <errno.h>
#include <malloc.h>
#include <asm/clk.h>
#include <linux/clk-provider.h>

struct sandbox_clk_hw {
	struct clk_hw hw;
	enum sandbox_clk_id id;
};

static ulong sandbox_clk_recalc_rate(struct clk_hw *hw, ulong parent_rate)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong id = shw->id;

	if (!priv->probed)
		return -ENODEV;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->rate[id];
}

static long sandbox_clk_round_rate(struct clk_hw *hw, ulong rate, ulong *parent_rate)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong id = shw->id;

	if (!priv->probed)
		return -ENODEV;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	if (!rate)
		return -EINVAL;

	return rate;
}

static int sandbox_clk_set_rate(struct clk_hw *hw, ulong rate, ulong parent_rate)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong old_rate;
	ulong id = shw->id;

	if (!priv->probed)
		return -ENODEV;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	if (!rate)
		return -EINVAL;

	old_rate = priv->rate[id];
	priv->rate[id] = rate;

	return 0;
}

static int sandbox_clk_enable(struct clk_hw *hw)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong id = shw->id;

	if (!priv->probed)
		return -ENODEV;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->enabled[id] = true;

	return 0;
}

static void sandbox_clk_disable(struct clk_hw *hw)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong id = shw->id;

	if (!priv->probed)
		return;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return;

	priv->enabled[id] = false;
}

static int sandbox_clk_request(struct clk_hw *hw)
{
	struct sandbox_clk_hw *shw = container_of(hw, struct sandbox_clk_hw, hw);
	struct sandbox_clk_priv *priv = dev_get_priv(hw->clk->dev);
	ulong id = shw->id;

	if (id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	priv->requested[id] = true;
	return 0;
}

static struct clk_ops sandbox_clk_ops = {
	.round_rate	= sandbox_clk_round_rate,
	.recalc_rate	= sandbox_clk_recalc_rate,
	.set_rate	= sandbox_clk_set_rate,
	.enable		= sandbox_clk_enable,
	.disable	= sandbox_clk_disable,
	.init		= sandbox_clk_request,
};

static struct sandbox_clk_hw sandbox_clk_hws[SANDBOX_CLK_ID_COUNT] = {
	{
		.hw.init = &(struct clk_init_data) {
			.name = "spi",
			.ops = &sandbox_clk_ops,
		},
		.id = SANDBOX_CLK_ID_SPI,
	},
	{
		.hw.init = &(struct clk_init_data) {
			.name = "i2c",
			.ops = &sandbox_clk_ops,
		},
		.id = SANDBOX_CLK_ID_I2C,
	},
	{
		.hw.init = &(struct clk_init_data) {
			.name = "uart1",
			.ops = &sandbox_clk_ops,
		},
		.id = SANDBOX_CLK_ID_UART1,
	},
	{
		.hw.init = &(struct clk_init_data) {
			.name = "uart2",
			.ops = &sandbox_clk_ops,
		},
		.id = SANDBOX_CLK_ID_UART2,
	},
	{
		.hw.init = &(struct clk_init_data) {
			.name = "bus",
			.ops = &sandbox_clk_ops,
		},
		.id = SANDBOX_CLK_ID_BUS,
	},
};

struct clk_hw *sbox_get_hw(struct ofnode_phandle_args *clkspec, void *data)
{
	struct sandbox_clk_hw *shws = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= SANDBOX_CLK_ID_COUNT) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return &shws[idx].hw;
}

static int sandbox_clk_probe(struct udevice *dev)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);
	struct sandbox_clk_hw *shws;
	int ret;

	/* We can't re-use the struct clk_hw since it will be modified. This driver may be probed multiple times. */
	shws = devm_kcalloc(dev, SANDBOX_CLK_ID_COUNT, sizeof(struct sandbox_clk_hw), GFP_KERNEL);
	if (!shws)
		return -ENOMEM;

	priv->probed = true;

	for (int i = 0; i < SANDBOX_CLK_ID_COUNT; i++) {
		memcpy(&shws[i], &sandbox_clk_hws[i], sizeof(struct sandbox_clk_hw));
		ret = devm_clk_hw_register(dev, &shws[i].hw);
		if (ret)
			return ret;
	}

	return devm_of_clk_add_hw_provider(dev, sbox_get_hw, shws);
}

static const struct udevice_id sandbox_clk_ids[] = {
	{ .compatible = "sandbox,clk" },
	{ }
};

U_BOOT_DRIVER(sandbox_clk) = {
	.name		= "sandbox_clk",
	.id		= UCLASS_NOP,
	.of_match	= sandbox_clk_ids,
	.probe		= sandbox_clk_probe,
	.priv_auto	= sizeof(struct sandbox_clk_priv),
};

ulong sandbox_clk_query_rate(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->rate[id];
}

int sandbox_clk_query_enable(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;

	return priv->enabled[id];
}

int sandbox_clk_query_requested(struct udevice *dev, int id)
{
	struct sandbox_clk_priv *priv = dev_get_priv(dev);

	if (id < 0 || id >= SANDBOX_CLK_ID_COUNT)
		return -EINVAL;
	return priv->requested[id];
}

static int clk_fixed_rate_probe(struct udevice *dev)
{
	ofnode node = dev_ofnode(dev);
	const char *clk_name;
	struct clk_hw *hw;
	u32 rate = 0;
	int ret;

	ofnode_read_u32(node, "clock-frequency", &rate);

	clk_name = ofnode_read_string(node, "clock-output-names");
	if (!clk_name)
		clk_name = ofnode_get_name(node);

	hw = clk_hw_register_fixed_rate(dev, clk_name, NULL, 0, rate);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	ret = of_clk_add_hw_provider(node, of_clk_hw_simple_get, hw);
	if (ret) {
		clk_hw_unregister_fixed_rate(hw);
		return ret;
	}

	return 0;
}

static const struct udevice_id sandbox_clk_fixed_rate_match[] = {
	{ .compatible = "sandbox,fixed-clock" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(sandbox_fixed_clock) = {
	.name = "sandbox_fixed_clock",
	.id = UCLASS_NOP,
	.of_match = sandbox_clk_fixed_rate_match,
	.probe = clk_fixed_rate_probe,
};
