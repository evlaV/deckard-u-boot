// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, 2017-2018, 2022, The Linux Foundation. All rights reserved.
 */

#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/ofnode.h>
#include <linux/bitops.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/regmap.h>
#include <power/regulator.h>
#include <power-domain-uclass.h>
#include <reset-uclass.h>
#include "gdsc.h"

#define PWR_ON_MASK		BIT(31)
#define EN_REST_WAIT_MASK	GENMASK_ULL(23, 20)
#define EN_FEW_WAIT_MASK	GENMASK_ULL(19, 16)
#define CLK_DIS_WAIT_MASK	GENMASK_ULL(15, 12)
#define SW_OVERRIDE_MASK	BIT(2)
#define HW_CONTROL_MASK		BIT(1)
#define SW_COLLAPSE_MASK	BIT(0)
#define GMEM_CLAMP_IO_MASK	BIT(0)
#define GMEM_RESET_MASK		BIT(4)

/* CFG_GDSCR */
#define GDSC_POWER_UP_COMPLETE		BIT(16)
#define GDSC_POWER_DOWN_COMPLETE	BIT(15)
#define GDSC_RETAIN_FF_ENABLE		BIT(11)
#define CFG_GDSCR_OFFSET		0x4

/* Wait 2^n CXO cycles between all states. Here, n=2 (4 cycles). */
#define EN_REST_WAIT_VAL	0x2
#define EN_FEW_WAIT_VAL		0x8
#define CLK_DIS_WAIT_VAL	0x2

/* Transition delay shifts */
#define EN_REST_WAIT_SHIFT	20
#define EN_FEW_WAIT_SHIFT	16
#define CLK_DIS_WAIT_SHIFT	12

#define RETAIN_MEM		BIT(14)
#define RETAIN_PERIPH		BIT(13)

#define STATUS_POLL_TIMEOUT_US	2000
#define TIMEOUT_US		500

#define domain_to_gdsc(domain) ((struct gdsc *)domain->priv)

enum gdsc_status {
	GDSC_OFF,
	GDSC_ON
};

/* Returns 1 if GDSC status is status, 0 if not, and < 0 on error */
static int gdsc_check_status(struct gdsc *sc, enum gdsc_status status)
{
	unsigned int reg;
	u32 val;
	int ret;

	if (sc->flags & POLL_CFG_GDSCR)
		reg = sc->gdscr + CFG_GDSCR_OFFSET;
	else if (sc->gds_hw_ctrl)
		reg = sc->gds_hw_ctrl;
	else
		reg = sc->gdscr;

	ret = regmap_read(sc->regmap, reg, &val);
	if (ret)
		return ret;

	if (sc->flags & POLL_CFG_GDSCR) {
		switch (status) {
		case GDSC_ON:
			return !!(val & GDSC_POWER_UP_COMPLETE);
		case GDSC_OFF:
			return !!(val & GDSC_POWER_DOWN_COMPLETE);
		}
	}

	switch (status) {
	case GDSC_ON:
		return !!(val & PWR_ON_MASK);
	case GDSC_OFF:
		return !(val & PWR_ON_MASK);
	}

	return -EINVAL;
}

static int gdsc_hwctrl(struct gdsc *sc, bool en)
{
	u32 val = en ? HW_CONTROL_MASK : 0;

	return regmap_update_bits(sc->regmap, sc->gdscr, HW_CONTROL_MASK, val);
}

static int gdsc_poll_status(struct gdsc *sc, enum gdsc_status status)
{
	u64 timeout;

	timeout = timer_get_us() + STATUS_POLL_TIMEOUT_US;
	do {
		if (gdsc_check_status(sc, status))
			return 0;
	} while (timer_get_us() < timeout);

	if (gdsc_check_status(sc, status))
		return 0;

	return -ETIMEDOUT;
}

static int gdsc_update_collapse_bit(struct gdsc *sc, bool val)
{
	u32 reg, mask;
	int ret;

	if (sc->collapse_mask) {
		reg = sc->collapse_ctrl;
		mask = sc->collapse_mask;
	} else {
		reg = sc->gdscr;
		mask = SW_COLLAPSE_MASK;
	}

	ret = regmap_update_bits(sc->regmap, reg, mask, val ? mask : 0);
	if (ret)
		return ret;

	return 0;
}

static int gdsc_toggle_logic(struct gdsc *sc, enum gdsc_status status,
		bool wait)
{
	int ret;

	if (status == GDSC_ON && sc->rsupply) {
		ret = regulator_enable(sc->rsupply);
		if (ret < 0)
			return ret;
	}

	ret = gdsc_update_collapse_bit(sc, status == GDSC_OFF);

	/* If disabling votable gdscs, don't poll on status */
	if ((sc->flags & VOTABLE) && status == GDSC_OFF && !wait) {
		/*
		 * Add a short delay here to ensure that an enable
		 * right after it was disabled does not put it in an
		 * unknown state
		 */
		udelay(TIMEOUT_US);
		return 0;
	}

	if (sc->gds_hw_ctrl) {
		/*
		 * The gds hw controller asserts/de-asserts the status bit soon
		 * after it receives a power on/off request from a master.
		 * The controller then takes around 8 xo cycles to start its
		 * internal state machine and update the status bit. During
		 * this time, the status bit does not reflect the true status
		 * of the core.
		 * Add a delay of 1 us between writing to the SW_COLLAPSE bit
		 * and polling the status bit.
		 */
		udelay(1);
	}

	ret = gdsc_poll_status(sc, status);
	WARN(ret, "%s status stuck at 'o%s'\n", sc->pd.name, status ? "ff" : "n");

	if (!ret && status == GDSC_OFF && sc->rsupply) {
		ret = regulator_disable(sc->rsupply);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static inline int gdsc_deassert_reset(struct gdsc *sc)
{
	debug("%s: %s: resets not yet supported!\n", sc->pd.name, __func__);
	// int i;

	// for (i = 0; i < sc->reset_count; i++)
	// 	reset_deassert(sc->rcdev, sc->resets[i]);
	return 0;
}

static inline int gdsc_assert_reset(struct gdsc *sc)
{
	debug("%s: %s: resets not yet supported!\n", sc->pd.name, __func__);
	// int i;

	// for (i = 0; i < sc->reset_count; i++)
	// 	reset_assert(sc->rcdev, sc->resets[i]);
	return 0;
}

static inline void gdsc_force_mem_on(struct gdsc *sc)
{
	int i;
	u32 mask = RETAIN_MEM;

	if (!(sc->flags & NO_RET_PERIPH))
		mask |= RETAIN_PERIPH;

	for (i = 0; i < sc->cxc_count; i++)
		regmap_update_bits(sc->regmap, sc->cxcs[i], mask, mask);
}

static inline void gdsc_clear_mem_on(struct gdsc *sc)
{
	int i;
	u32 mask = RETAIN_MEM;

	if (!(sc->flags & NO_RET_PERIPH))
		mask |= RETAIN_PERIPH;

	for (i = 0; i < sc->cxc_count; i++)
		regmap_update_bits(sc->regmap, sc->cxcs[i], mask, 0);
}

static inline void gdsc_deassert_clamp_io(struct gdsc *sc)
{
	regmap_update_bits(sc->regmap, sc->clamp_io_ctrl,
			   GMEM_CLAMP_IO_MASK, 0);
}

static inline void gdsc_assert_clamp_io(struct gdsc *sc)
{
	regmap_update_bits(sc->regmap, sc->clamp_io_ctrl,
			   GMEM_CLAMP_IO_MASK, 1);
}

static inline void gdsc_assert_reset_aon(struct gdsc *sc)
{
	regmap_update_bits(sc->regmap, sc->clamp_io_ctrl,
			   GMEM_RESET_MASK, 1);
	udelay(1);
	regmap_update_bits(sc->regmap, sc->clamp_io_ctrl,
			   GMEM_RESET_MASK, 0);
}

static void gdsc_retain_ff_on(struct gdsc *sc)
{
	u32 mask = GDSC_RETAIN_FF_ENABLE;

	regmap_update_bits(sc->regmap, sc->gdscr, mask, mask);
}

static int _gdsc_enable(struct gdsc *sc)
{
	int ret;

	if (sc->pwrsts == PWRSTS_ON)
		return gdsc_deassert_reset(sc);

	if (sc->flags & SW_RESET) {
		gdsc_assert_reset(sc);
		udelay(1);
		gdsc_deassert_reset(sc);
	}

	if (sc->flags & CLAMP_IO) {
		if (sc->flags & AON_RESET)
			gdsc_assert_reset_aon(sc);
		gdsc_deassert_clamp_io(sc);
	}

	ret = gdsc_toggle_logic(sc, GDSC_ON, false);
	if (ret)
		return ret;

	if (sc->pwrsts & PWRSTS_OFF)
		gdsc_force_mem_on(sc);

	/*
	 * If clocks to this power domain were already on, they will take an
	 * additional 4 clock cycles to re-enable after the power domain is
	 * enabled. Delay to account for this. A delay is also needed to ensure
	 * clocks are not enabled within 400ns of enabling power to the
	 * memories.
	 */
	udelay(1);

	if (sc->flags & RETAIN_FF_ENABLE)
		gdsc_retain_ff_on(sc);

	/* Turn on HW trigger mode if supported */
	if (sc->flags & HW_CTRL) {
		ret = gdsc_hwctrl(sc, true);
		if (ret)
			return ret;
		/*
		 * Wait for the GDSC to go through a power down and
		 * up cycle.  In case a firmware ends up polling status
		 * bits for the gdsc, it might read an 'on' status before
		 * the GDSC can finish the power cycle.
		 * We wait 1us before returning to ensure the firmware
		 * can't immediately poll the status bits.
		 */
		udelay(1);
	}

	return 0;
}

static int gdsc_enable(struct power_domain *domain)
{
	return _gdsc_enable(domain_to_gdsc(domain));
}

static int gdsc_disable(struct power_domain *domain)
{
	struct gdsc *sc = domain_to_gdsc(domain);
	int ret;

	if (sc->pwrsts == PWRSTS_ON)
		return gdsc_assert_reset(sc);

	/* Turn off HW trigger mode if supported */
	if (sc->flags & HW_CTRL) {
		ret = gdsc_hwctrl(sc, false);
		if (ret < 0)
			return ret;
		/*
		 * Wait for the GDSC to go through a power down and
		 * up cycle.  In case we end up polling status
		 * bits for the gdsc before the power cycle is completed
		 * it might read an 'on' status wrongly.
		 */
		udelay(1);

		ret = gdsc_poll_status(sc, GDSC_ON);
		if (ret)
			return ret;
	}

	if (sc->pwrsts & PWRSTS_OFF)
		gdsc_clear_mem_on(sc);

	/*
	 * If the GDSC supports only a Retention state, apart from ON,
	 * leave it in ON state.
	 * There is no SW control to transition the GDSC into
	 * Retention state. This happens in HW when the parent
	 * domain goes down to a Low power state
	 */
	if (sc->pwrsts == PWRSTS_RET_ON)
		return 0;

	ret = gdsc_toggle_logic(sc, GDSC_OFF, true);
	if (ret)
		return ret;

	if (sc->flags & CLAMP_IO)
		gdsc_assert_clamp_io(sc);

	return 0;
}

static int gdsc_init(struct gdsc *sc)
{
	u32 mask, val;
	int on, ret;

	/*
	 * Disable HW trigger: collapse/restore occur based on registers writes.
	 * Disable SW override: Use hardware state-machine for sequencing.
	 * Configure wait time between states.
	 */
	mask = HW_CONTROL_MASK | SW_OVERRIDE_MASK |
	       EN_REST_WAIT_MASK | EN_FEW_WAIT_MASK | CLK_DIS_WAIT_MASK;

	if (!sc->en_rest_wait_val)
		sc->en_rest_wait_val = EN_REST_WAIT_VAL;
	if (!sc->en_few_wait_val)
		sc->en_few_wait_val = EN_FEW_WAIT_VAL;
	if (!sc->clk_dis_wait_val)
		sc->clk_dis_wait_val = CLK_DIS_WAIT_VAL;

	val = sc->en_rest_wait_val << EN_REST_WAIT_SHIFT |
		sc->en_few_wait_val << EN_FEW_WAIT_SHIFT |
		sc->clk_dis_wait_val << CLK_DIS_WAIT_SHIFT;

	ret = regmap_update_bits(sc->regmap, sc->gdscr, mask, val);
	if (ret)
		return ret;

	// /* Force gdsc ON if only ON state is supported */
	// if (sc->pwrsts == PWRSTS_ON) {
	// 	ret = gdsc_toggle_logic(sc, GDSC_ON, false);
	// 	if (ret)
	// 		return ret;
	// }

	on = gdsc_check_status(sc, GDSC_ON);
	if (on < 0)
		return on;

	if (on) {
		/* The regulator must be on, sync the kernel state */
		if (sc->rsupply) {
			ret = regulator_enable(sc->rsupply);
			if (ret < 0)
				return ret;
		}

		/*
		 * Votable GDSCs can be ON due to Vote from other masters.
		 * If a Votable GDSC is ON, make sure we have a Vote.
		 */
		if (sc->flags & VOTABLE) {
			ret = gdsc_update_collapse_bit(sc, false);
			if (ret)
				goto err_disable_supply;
		}

		/*
		 * Make sure the retain bit is set if the GDSC is already on,
		 * otherwise we end up turning off the GDSC and destroying all
		 * the register contents that we thought we were saving.
		 */
		if (sc->flags & RETAIN_FF_ENABLE)
			gdsc_retain_ff_on(sc);

		/* Turn on HW trigger mode if supported */
		if (sc->flags & HW_CTRL) {
			ret = gdsc_hwctrl(sc, true);
			if (ret < 0)
				goto err_disable_supply;
		}

	} else if (sc->flags & ALWAYS_ON) {
		/* If ALWAYS_ON GDSCs are not ON, turn them ON */
		_gdsc_enable(sc);
		on = true;
	}

	if (on || (sc->pwrsts & PWRSTS_RET))
		gdsc_force_mem_on(sc);
	else
		gdsc_clear_mem_on(sc);

	// if (sc->flags & ALWAYS_ON)
	// 	sc->pd.flags |= GENPD_FLAG_ALWAYS_ON;
	// if (!sc->pd.power_off)
	// 	sc->pd.power_off = gdsc_disable;
	// if (!sc->pd.power_on)
	// 	sc->pd.power_on = gdsc_enable;
	// if (sc->flags & HW_CTRL_TRIGGER) {
	// 	sc->pd.set_hwmode_dev = gdsc_set_hwmode;
	// 	sc->pd.get_hwmode_dev = gdsc_get_hwmode;
	// }

	// ret = pm_genpd_init(&sc->pd, NULL, !on);
	// if (ret)
	// 	goto err_disable_supply;

	return 0;

err_disable_supply:
	if (on && sc->rsupply)
		regulator_disable(sc->rsupply);

	return ret;
}

static int qcom_gdsc_of_xlate(struct power_domain *pd,
			struct ofnode_phandle_args *args)
{
	struct gdsc_desc *desc = (struct gdsc_desc *)dev_get_driver_data(pd->dev);

	if (args->args_count != 1) {
		debug("Invalid args_count: %d\n", args->args_count);
		return -EINVAL;
	}

	pd->id = args->args[0];

	if (pd->id > desc->num_gdscs) {
		dev_err(pd->dev, "Invalid GDSC ID %ld\n", pd->id);
		return -EINVAL;
	}

	/* Assign per-GDSC private data */
	desc->gdsc[pd->id]->regmap = desc->regmap;
	pd->priv = desc->gdsc[pd->id];

	gdsc_init(desc->gdsc[pd->id]);

	return 0;
}

static const struct power_domain_ops qcom_gdsc_ops = {
	.on = gdsc_enable,
	.off = gdsc_disable,
	.of_xlate = qcom_gdsc_of_xlate,
};

U_BOOT_DRIVER(qcom_gdsc) = {
	.name = "qcom_gdsc",
	.id = UCLASS_POWER_DOMAIN,
	.ops = &qcom_gdsc_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

int gdsc_register(struct udevice *dev, struct gdsc **gdscs, int num_gdscs,
		  struct regmap *regmap)
{
	int ret;
	char *name;
	struct gdsc_desc *desc;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->dev = dev;
	desc->gdsc = gdscs;
	desc->num_gdscs = num_gdscs;
	desc->regmap = regmap;

	name = kzalloc(64, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	ret = snprintf(name, 64, "%s.%s", desc->dev->name, "gdsc");
	if (ret < 0)
		return ret;

	return device_bind_with_driver_data(desc->dev, DM_DRIVER_REF(qcom_gdsc),
					    strdup(name), (ulong)desc, dev_ofnode(dev), NULL);
}
