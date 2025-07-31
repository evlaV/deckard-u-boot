// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 */

#define LOG_DEBUG

#include <clk-uclass.h>
#include <linux/clk-provider.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <linux/clk-provider.h>
#include <asm/io.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <reset-uclass.h>
#include <power-domain-uclass.h>
#include <soc/qcom/cmd-db.h>
#include <soc/qcom/rpmh.h>
#include <soc/qcom/tcs.h>

#include <dt-bindings/clock/qcom,rpmh.h>

#define CLK_RPMH_ARC_EN_OFFSET		0
#define CLK_RPMH_VRM_EN_OFFSET		4

/**
 * struct bcm_db - Auxiliary data pertaining to each Bus Clock Manager(BCM)
 * @unit: divisor used to convert Hz value to an RPMh msg
 * @width: multiplier used to convert Hz value to an RPMh msg
 * @vcd: virtual clock domain that this bcm belongs to
 * @reserved: reserved to pad the struct
 */
struct bcm_db {
	__le32 unit;
	__le16 width;
	u8 vcd;
	u8 reserved;
};

/**
 * struct clk_rpmh - individual rpmh clock data structure
 * @hw:			handle between common and hardware-specific interfaces
 * @res_name:		resource name for the rpmh clock
 * @div:		clock divider to compute the clock rate
 * @res_addr:		base address of the rpmh resource within the RPMh
 * @res_on_val:		rpmh clock enable value
 * @state:		rpmh clock requested state
 * @aggr_state:		rpmh clock aggregated state
 * @last_sent_aggr_state: rpmh clock last aggr state sent to RPMh
 * @valid_state_mask:	mask to determine the state of the rpmh clock
 * @unit:		divisor to convert rate to rpmh msg in magnitudes of Khz
 * @dev:		device to which it is attached
 * @peer:		pointer to the clock rpmh sibling
 */
struct clk_rpmh {
	const char *name;
	const char *res_name;
	u8 div;
	u32 res_addr;
	u32 res_on_val;
	u32 state;
	u32 aggr_state;
	u32 last_sent_aggr_state;
	u32 valid_state_mask;
	u32 unit;
	struct udevice *dev;
	struct clk_rpmh *peer;
};

struct clk_rpmh_desc {
	const char *name;
	struct clk_rpmh **clks;
	size_t num_clks;
	/* RPMh clock clkaN are optional for this platform */
	bool clka_optional;
};

DEFINE_MUTEX(rpmh_clk_lock);

#define __DEFINE_CLK_RPMH(_name, _clk_name, _res_name,			\
			  _res_en_offset, _res_on, _div)		\
	static struct clk_rpmh clk_rpmh_##_clk_name##_ao;		\
	static struct clk_rpmh clk_rpmh_##_clk_name = {			\
		.name = #_name,						\
		.res_name = _res_name,					\
		.res_addr = _res_en_offset,				\
		.res_on_val = _res_on,					\
		.div = _div,						\
		.peer = &clk_rpmh_##_clk_name##_ao,			\
		.valid_state_mask = (BIT(RPMH_WAKE_ONLY_STATE) |	\
				      BIT(RPMH_ACTIVE_ONLY_STATE) |	\
				      BIT(RPMH_SLEEP_STATE)),		\
	};								\
	static struct clk_rpmh clk_rpmh_##_clk_name##_ao= {		\
		.name = #_name,						\
		.res_name = _res_name,					\
		.res_addr = _res_en_offset,				\
		.res_on_val = _res_on,					\
		.div = _div,						\
		.peer = &clk_rpmh_##_clk_name,				\
		.valid_state_mask = (BIT(RPMH_WAKE_ONLY_STATE) |	\
					BIT(RPMH_ACTIVE_ONLY_STATE)),	\
	}

#define DEFINE_CLK_RPMH_ARC(_name, _res_name, _res_on, _div)		\
	__DEFINE_CLK_RPMH(_name, _name##_##div##_div, _res_name,	\
			  CLK_RPMH_ARC_EN_OFFSET, _res_on, _div)

#define DEFINE_CLK_RPMH_VRM(_name, _suffix, _res_name, _div)		\
	__DEFINE_CLK_RPMH(_name, _name##_suffix, _res_name,		\
			  CLK_RPMH_VRM_EN_OFFSET, 1, _div)

#define DEFINE_CLK_RPMH_BCM(_name, _res_name)				\
	static struct clk_rpmh clk_rpmh_##_name = {			\
		.res_name = _res_name,					\
		.valid_state_mask = BIT(RPMH_ACTIVE_ONLY_STATE),	\
		.div = 1,						\
		.hw.init = &(struct clk_init_data){			\
			.ops = &clk_rpmh_bcm_ops,			\
			.name = #_name,					\
		},							\
	}

static inline struct clk_rpmh *to_clk_rpmh(struct clk *clk)
{
	struct clk_rpmh_desc *desc = (struct clk_rpmh_desc *)dev_get_driver_data(clk->dev);

	if (clk->id >= desc->num_clks) {
		dev_err(clk->dev, "Invalid clk id %lu\n", clk->id);
		return NULL;
	}

	return desc->clks[clk->id];
}

static inline bool has_state_changed(struct clk_rpmh *c, u32 state)
{
	return (c->last_sent_aggr_state & BIT(state))
		!= (c->aggr_state & BIT(state));
}

static int clk_rpmh_send(struct clk_rpmh *c, enum rpmh_state state,
			 struct tcs_cmd *cmd, bool wait)
{
	if (wait)
		return rpmh_write(c->dev, state, cmd, 1);

	return rpmh_write_async(c->dev, state, cmd, 1);
}

static int clk_rpmh_send_aggregate_command(struct clk_rpmh *c)
{
	struct tcs_cmd cmd = { 0 };
	u32 cmd_state, on_val;
	enum rpmh_state state = RPMH_SLEEP_STATE;
	int ret;
	bool wait;

	cmd.addr = c->res_addr;
	cmd_state = c->aggr_state;
	on_val = c->res_on_val;

	for (; state <= RPMH_ACTIVE_ONLY_STATE; state++) {
		if (has_state_changed(c, state)) {
			if (cmd_state & BIT(state))
				cmd.data = on_val;

			wait = cmd_state && state == RPMH_ACTIVE_ONLY_STATE;
			dev_err(c->dev, "set %s state of %s to %d (was aggr %#x)\n",
					rpmh_state_str(state), c->res_name,
					cmd.data, c->last_sent_aggr_state);
			ret = clk_rpmh_send(c, state, &cmd, wait);
			if (ret) {
				dev_err(c->dev, "set %s state of %s failed: (%d)\n",
					rpmh_state_str(state), c->res_name, ret);
				return ret;
			}
		}
	}

	c->last_sent_aggr_state = c->aggr_state;
	c->peer->last_sent_aggr_state =  c->last_sent_aggr_state;

	return 0;
}

/*
 * Update state and aggregate state values based on enable value.
 */
static int clk_rpmh_aggregate_state_send_command(struct clk_rpmh *c,
						bool enable)
{
	int ret;

	c->state = enable ? c->valid_state_mask : 0;
	c->aggr_state = c->state | c->peer->state;
	c->peer->aggr_state = c->aggr_state;

	ret = clk_rpmh_send_aggregate_command(c);
	if (!ret)
		return 0;

	if (ret && enable)
		c->state = 0;
	else if (ret)
		c->state = c->valid_state_mask;

	WARN(1, "clk: %s failed to %s\n", c->res_name,
	     enable ? "enable" : "disable");
	return ret;
}

static int clk_rpmh_prepare(struct clk *clk)
{
	struct clk_rpmh *c = to_clk_rpmh(clk);
	int ret = 0;

	debug("%s: Enabling %s (%s)\n", __func__, c->name, c->res_name);

	mutex_lock(&rpmh_clk_lock);
	ret = clk_rpmh_aggregate_state_send_command(c, true);
	mutex_unlock(&rpmh_clk_lock);

	return ret;
}

static int clk_rpmh_unprepare(struct clk *hw)
{
	int ret;
	struct clk_rpmh *c = to_clk_rpmh(hw);

	mutex_lock(&rpmh_clk_lock);
	ret = clk_rpmh_aggregate_state_send_command(c, false);
	mutex_unlock(&rpmh_clk_lock);

	return ret;
};

/* On-board TCXO, TOFIX get from DT */
#define TCXO_RATE	38400000

static unsigned long clk_rpmh_get_rate(struct clk *hw)
{
	struct clk_rpmh *r = to_clk_rpmh(hw);

	/*
	 * RPMh clocks have a fixed rate. Return static rate.
	 */
	return TCXO_RATE / r->div;
}

static const struct clk_ops clk_rpmh_ops = {
	.enable	= clk_rpmh_prepare,
	.disable	= clk_rpmh_unprepare,
	.get_rate	= clk_rpmh_get_rate,
	.request = clk_rpmh_prepare, // HACK: like linux .prepare? just calls rpmh_prepare...
};

/* Resource name must match resource id present in cmd-db */
DEFINE_CLK_RPMH_ARC(bi_tcxo, "xo.lvl", 0x3, 1);
DEFINE_CLK_RPMH_ARC(bi_tcxo, "xo.lvl", 0x3, 2);
DEFINE_CLK_RPMH_ARC(bi_tcxo, "xo.lvl", 0x3, 4);
DEFINE_CLK_RPMH_ARC(qlink, "qphy.lvl", 0x1, 4);

DEFINE_CLK_RPMH_VRM(ln_bb_clk1, _a2, "lnbclka1", 2);
DEFINE_CLK_RPMH_VRM(ln_bb_clk2, _a2, "lnbclka2", 2);
DEFINE_CLK_RPMH_VRM(ln_bb_clk3, _a2, "lnbclka3", 2);

DEFINE_CLK_RPMH_VRM(ln_bb_clk1, _a4, "lnbclka1", 4);
DEFINE_CLK_RPMH_VRM(ln_bb_clk2, _a4, "lnbclka2", 4);
DEFINE_CLK_RPMH_VRM(ln_bb_clk3, _a4, "lnbclka3", 4);

DEFINE_CLK_RPMH_VRM(ln_bb_clk2, _g4, "lnbclkg2", 4);
DEFINE_CLK_RPMH_VRM(ln_bb_clk3, _g4, "lnbclkg3", 4);

DEFINE_CLK_RPMH_VRM(rf_clk1, _a, "rfclka1", 1);
DEFINE_CLK_RPMH_VRM(rf_clk2, _a, "rfclka2", 1);
DEFINE_CLK_RPMH_VRM(rf_clk3, _a, "rfclka3", 1);
DEFINE_CLK_RPMH_VRM(rf_clk4, _a, "rfclka4", 1);
DEFINE_CLK_RPMH_VRM(rf_clk5, _a, "rfclka5", 1);

DEFINE_CLK_RPMH_VRM(rf_clk1, _d, "rfclkd1", 1);
DEFINE_CLK_RPMH_VRM(rf_clk2, _d, "rfclkd2", 1);
DEFINE_CLK_RPMH_VRM(rf_clk3, _d, "rfclkd3", 1);
DEFINE_CLK_RPMH_VRM(rf_clk4, _d, "rfclkd4", 1);

DEFINE_CLK_RPMH_VRM(rf_clk3, _a2, "rfclka3", 2);

DEFINE_CLK_RPMH_VRM(clk1, _a1, "clka1", 1);
DEFINE_CLK_RPMH_VRM(clk2, _a1, "clka2", 1);
DEFINE_CLK_RPMH_VRM(clk3, _a1, "clka3", 1);
DEFINE_CLK_RPMH_VRM(clk4, _a1, "clka4", 1);
DEFINE_CLK_RPMH_VRM(clk5, _a1, "clka5", 1);

DEFINE_CLK_RPMH_VRM(clk3, _a2, "clka3", 2);
DEFINE_CLK_RPMH_VRM(clk4, _a2, "clka4", 2);
DEFINE_CLK_RPMH_VRM(clk5, _a2, "clka5", 2);
DEFINE_CLK_RPMH_VRM(clk6, _a2, "clka6", 2);
DEFINE_CLK_RPMH_VRM(clk7, _a2, "clka7", 2);
DEFINE_CLK_RPMH_VRM(clk8, _a2, "clka8", 2);

DEFINE_CLK_RPMH_VRM(div_clk1, _div2, "divclka1", 2);

static struct clk_rpmh *sm8650_rpmh_clocks[] = {
	[RPMH_CXO_CLK]		= &clk_rpmh_bi_tcxo_div2,
	[RPMH_CXO_CLK_A]	= &clk_rpmh_bi_tcxo_div2_ao,
	[RPMH_LN_BB_CLK1]	= &clk_rpmh_clk6_a2,
	[RPMH_LN_BB_CLK1_A]	= &clk_rpmh_clk6_a2_ao,
	[RPMH_LN_BB_CLK2]	= &clk_rpmh_clk7_a2,
	[RPMH_LN_BB_CLK2_A]	= &clk_rpmh_clk7_a2_ao,
	[RPMH_LN_BB_CLK3]	= &clk_rpmh_clk8_a2,
	[RPMH_LN_BB_CLK3_A]	= &clk_rpmh_clk8_a2_ao,
	[RPMH_RF_CLK1]		= &clk_rpmh_clk1_a1,
	[RPMH_RF_CLK1_A]	= &clk_rpmh_clk1_a1_ao,
	[RPMH_RF_CLK2]		= &clk_rpmh_clk2_a1,
	[RPMH_RF_CLK2_A]	= &clk_rpmh_clk2_a1_ao,
	/*
	 * The clka3 RPMh resource is missing in cmd-db
	 * for current platforms, while the clka3 exists
	 * on the PMK8550, the clock is unconnected and
	 * unused.
	 */
	[RPMH_RF_CLK4]		= &clk_rpmh_clk4_a2,
	[RPMH_RF_CLK4_A]	= &clk_rpmh_clk4_a2_ao,
	[RPMH_RF_CLK5]		= &clk_rpmh_clk5_a2,
	[RPMH_RF_CLK5_A]	= &clk_rpmh_clk5_a2_ao,
	// [RPMH_IPA_CLK]		= &clk_rpmh_ipa,
};

static const struct clk_rpmh_desc clk_rpmh_sm8650 = {
	.name = "rpmhcc-sm8650",
	.clks = sm8650_rpmh_clocks,
	.num_clks = ARRAY_SIZE(sm8650_rpmh_clocks),
	.clka_optional = true,
};

static int clk_rpmh_probe(struct udevice *dev)
{
	struct clk_rpmh **hw_clks;
	struct clk_rpmh *rpmh_clk;
	const struct clk_rpmh_desc *desc;
	int ret, i;

	desc = (struct clk_rpmh_desc *)dev_get_driver_data(dev);
	if (!desc)
		return -ENODEV;

	device_set_name(dev, desc->name);

	hw_clks = desc->clks;

	for (i = 0; i < desc->num_clks; i++) {
		const char *name;
		u32 res_addr;
		size_t aux_data_len;
		const struct bcm_db *data;

		if (!hw_clks[i])
			continue;

		name = hw_clks[i]->name;

		rpmh_clk = hw_clks[i];
		res_addr = cmd_db_read_addr(rpmh_clk->res_name);
		if (!res_addr) {
			hw_clks[i] = NULL;

			if (desc->clka_optional &&
			    !strncmp(rpmh_clk->res_name, "clka", sizeof("clka") - 1))
				continue;

			dev_err(dev, "missing RPMh resource address for %s\n",
				rpmh_clk->res_name);
			return -ENODEV;
		}

		data = cmd_db_read_aux_data(rpmh_clk->res_name, &aux_data_len);
		if (IS_ERR(data)) {
			ret = PTR_ERR(data);
			dev_err(dev,
				"error reading RPMh aux data for %s (%d)\n",
				rpmh_clk->res_name, ret);
			return ret;
		}

		/* Convert unit from Khz to Hz */
		if (aux_data_len == sizeof(*data))
			rpmh_clk->unit = le32_to_cpu(data->unit) * 1000ULL;

		rpmh_clk->res_addr += res_addr;
		rpmh_clk->dev = dev;

		// ret = devm_clk_hw_register(&dev->dev, hw_clks[i]);
		// if (ret) {
		// 	dev_err(&dev->dev, "failed to register %s\n", name);
		// 	return ret;
		// }
	}

	/* typecast to silence compiler warning */
	// ret = devm_of_clk_add_hw_provider(&dev->dev, of_clk_rpmh_hw_get,
	// 				  (void *)desc);
	// if (ret) {
	// 	dev_err(&dev->dev, "Failed to add clock provider\n");
	// 	return ret;
	// }

	debug("%s: Registered RPMh clocks\n", dev->name);

	return 0;
}

static const struct udevice_id clk_rpmh_match_table[] = {
	{ .compatible = "qcom,sm8650-rpmh-clk", .data = (ulong)&clk_rpmh_sm8650},
	{ }
};

U_BOOT_DRIVER(clk_rpmh) = {
	.name = "clk-rpmh",
	.id = UCLASS_CLK,
	.of_match = clk_rpmh_match_table,
	.probe = clk_rpmh_probe,
	.ops = &clk_rpmh_ops,
};
