// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015, 2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2025 Linaro Ltd.
 *
 * Mostly direct port from Linux with adjustments for U-Boot
 */

#include <asm/io.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <div64.h>

#include "clk-alpha-pll.h"


#define PLL_MODE(p)		((p)->offset + 0x0)
# define PLL_OUTCTRL		BIT(0)
# define PLL_BYPASSNL		BIT(1)
# define PLL_RESET_N		BIT(2)
# define PLL_OFFLINE_REQ	BIT(7)
# define PLL_LOCK_COUNT_SHIFT	8
# define PLL_LOCK_COUNT_MASK	0x3f
# define PLL_BIAS_COUNT_SHIFT	14
# define PLL_BIAS_COUNT_MASK	0x3f
# define PLL_VOTE_FSM_ENA	BIT(20)
# define PLL_FSM_ENA		BIT(20)
# define PLL_VOTE_FSM_RESET	BIT(21)
# define PLL_UPDATE		BIT(22)
# define PLL_UPDATE_BYPASS	BIT(23)
# define PLL_FSM_LEGACY_MODE	BIT(24)
# define PLL_OFFLINE_ACK	BIT(28)
# define ALPHA_PLL_ACK_LATCH	BIT(29)
# define PLL_ACTIVE_FLAG	BIT(30)
# define PLL_LOCK_DET		BIT(31)

#define PLL_L_VAL(p)		((p)->offset + (p)->regs[PLL_OFF_L_VAL])
#define PLL_CAL_L_VAL(p)	((p)->offset + (p)->regs[PLL_OFF_CAL_L_VAL])
#define PLL_ALPHA_VAL(p)	((p)->offset + (p)->regs[PLL_OFF_ALPHA_VAL])
#define PLL_ALPHA_VAL_U(p)	((p)->offset + (p)->regs[PLL_OFF_ALPHA_VAL_U])

#define PLL_USER_CTL(p)		((p)->offset + (p)->regs[PLL_OFF_USER_CTL])
# define PLL_POST_DIV_SHIFT	8
# define PLL_POST_DIV_MASK(p)	GENMASK((p)->width ? (p)->width - 1 : 3, 0)
# define PLL_ALPHA_MSB		BIT(15)
# define PLL_ALPHA_EN		BIT(24)
# define PLL_ALPHA_MODE		BIT(25)
# define PLL_VCO_SHIFT		20
# define PLL_VCO_MASK		0x3

#define PLL_USER_CTL_U(p)	((p)->offset + (p)->regs[PLL_OFF_USER_CTL_U])
#define PLL_USER_CTL_U1(p)	((p)->offset + (p)->regs[PLL_OFF_USER_CTL_U1])

#define PLL_CONFIG_CTL(p)	((p)->offset + (p)->regs[PLL_OFF_CONFIG_CTL])
#define PLL_CONFIG_CTL_U(p)	((p)->offset + (p)->regs[PLL_OFF_CONFIG_CTL_U])
#define PLL_CONFIG_CTL_U1(p)	((p)->offset + (p)->regs[PLL_OFF_CONFIG_CTL_U1])
#define PLL_CONFIG_CTL_U2(p)	((p)->offset + (p)->regs[PLL_OFF_CONFIG_CTL_U2])
#define PLL_TEST_CTL(p)		((p)->offset + (p)->regs[PLL_OFF_TEST_CTL])
#define PLL_TEST_CTL_U(p)	((p)->offset + (p)->regs[PLL_OFF_TEST_CTL_U])
#define PLL_TEST_CTL_U1(p)     ((p)->offset + (p)->regs[PLL_OFF_TEST_CTL_U1])
#define PLL_TEST_CTL_U2(p)     ((p)->offset + (p)->regs[PLL_OFF_TEST_CTL_U2])
#define PLL_TEST_CTL_U3(p)     ((p)->offset + (p)->regs[PLL_OFF_TEST_CTL_U3])
#define PLL_STATUS(p)		((p)->offset + (p)->regs[PLL_OFF_STATUS])
#define PLL_OPMODE(p)		((p)->offset + (p)->regs[PLL_OFF_OPMODE])
#define PLL_FRAC(p)		((p)->offset + (p)->regs[PLL_OFF_FRAC])

const u8 clk_alpha_pll_regs[][PLL_OFF_MAX_REGS] = {
	[CLK_ALPHA_PLL_TYPE_DEFAULT] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL_U] = 0x0c,
		[PLL_OFF_USER_CTL] = 0x10,
		[PLL_OFF_USER_CTL_U] = 0x14,
		[PLL_OFF_CONFIG_CTL] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_STATUS] = 0x24,
	},
	[CLK_ALPHA_PLL_TYPE_HUAYRA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL] = 0x14,
		[PLL_OFF_CONFIG_CTL_U] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_STATUS] = 0x24,
	},
	[CLK_ALPHA_PLL_TYPE_HUAYRA_APSS] =  {
		[PLL_OFF_L_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL] = 0x10,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_CONFIG_CTL_U] = 0x24,
		[PLL_OFF_STATUS] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x30,
		[PLL_OFF_TEST_CTL_U] = 0x34,
	},
	[CLK_ALPHA_PLL_TYPE_HUAYRA_2290] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_CONFIG_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL_U] = 0x14,
		[PLL_OFF_CONFIG_CTL_U1] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_TEST_CTL_U1] = 0x24,
		[PLL_OFF_OPMODE] = 0x28,
		[PLL_OFF_STATUS] = 0x38,
	},
	[CLK_ALPHA_PLL_TYPE_BRAMMO] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL_U] = 0x0c,
		[PLL_OFF_USER_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_STATUS] = 0x24,
	},
	[CLK_ALPHA_PLL_TYPE_FABIA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_USER_CTL_U] = 0x10,
		[PLL_OFF_CONFIG_CTL] = 0x14,
		[PLL_OFF_CONFIG_CTL_U] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_STATUS] = 0x24,
		[PLL_OFF_OPMODE] = 0x2c,
		[PLL_OFF_FRAC] = 0x38,
	},
	[CLK_ALPHA_PLL_TYPE_TRION] = {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_CAL_L_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_USER_CTL_U] = 0x10,
		[PLL_OFF_USER_CTL_U1] = 0x14,
		[PLL_OFF_CONFIG_CTL] = 0x18,
		[PLL_OFF_CONFIG_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL_U1] = 0x20,
		[PLL_OFF_TEST_CTL] = 0x24,
		[PLL_OFF_TEST_CTL_U] = 0x28,
		[PLL_OFF_TEST_CTL_U1] = 0x2c,
		[PLL_OFF_STATUS] = 0x30,
		[PLL_OFF_OPMODE] = 0x38,
		[PLL_OFF_ALPHA_VAL] = 0x40,
	},
	[CLK_ALPHA_PLL_TYPE_AGERA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_CONFIG_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL_U] = 0x14,
		[PLL_OFF_TEST_CTL] = 0x18,
		[PLL_OFF_TEST_CTL_U] = 0x1c,
		[PLL_OFF_STATUS] = 0x2c,
	},
	[CLK_ALPHA_PLL_TYPE_ZONDA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_CONFIG_CTL] = 0x10,
		[PLL_OFF_CONFIG_CTL_U] = 0x14,
		[PLL_OFF_CONFIG_CTL_U1] = 0x18,
		[PLL_OFF_TEST_CTL] = 0x1c,
		[PLL_OFF_TEST_CTL_U] = 0x20,
		[PLL_OFF_TEST_CTL_U1] = 0x24,
		[PLL_OFF_OPMODE] = 0x28,
		[PLL_OFF_STATUS] = 0x38,
	},
	[CLK_ALPHA_PLL_TYPE_LUCID_EVO] = {
		[PLL_OFF_OPMODE] = 0x04,
		[PLL_OFF_STATUS] = 0x0c,
		[PLL_OFF_L_VAL] = 0x10,
		[PLL_OFF_ALPHA_VAL] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_USER_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_CONFIG_CTL_U] = 0x24,
		[PLL_OFF_CONFIG_CTL_U1] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x2c,
		[PLL_OFF_TEST_CTL_U] = 0x30,
		[PLL_OFF_TEST_CTL_U1] = 0x34,
	},
	[CLK_ALPHA_PLL_TYPE_LUCID_OLE] = {
		[PLL_OFF_OPMODE] = 0x04,
		[PLL_OFF_STATE] = 0x08,
		[PLL_OFF_STATUS] = 0x0c,
		[PLL_OFF_L_VAL] = 0x10,
		[PLL_OFF_ALPHA_VAL] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_USER_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_CONFIG_CTL_U] = 0x24,
		[PLL_OFF_CONFIG_CTL_U1] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x2c,
		[PLL_OFF_TEST_CTL_U] = 0x30,
		[PLL_OFF_TEST_CTL_U1] = 0x34,
		[PLL_OFF_TEST_CTL_U2] = 0x38,
	},
	[CLK_ALPHA_PLL_TYPE_PONGO_ELU] = {
		[PLL_OFF_OPMODE] = 0x04,
		[PLL_OFF_STATE] = 0x08,
		[PLL_OFF_STATUS] = 0x0c,
		[PLL_OFF_L_VAL] = 0x10,
		[PLL_OFF_USER_CTL] = 0x14,
		[PLL_OFF_USER_CTL_U] = 0x18,
		[PLL_OFF_CONFIG_CTL] = 0x1c,
		[PLL_OFF_CONFIG_CTL_U] = 0x20,
		[PLL_OFF_CONFIG_CTL_U1] = 0x24,
		[PLL_OFF_CONFIG_CTL_U2] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x2c,
		[PLL_OFF_TEST_CTL_U] = 0x30,
		[PLL_OFF_TEST_CTL_U1] = 0x34,
		[PLL_OFF_TEST_CTL_U2] = 0x38,
		[PLL_OFF_TEST_CTL_U3] = 0x3c,
	},
	[CLK_ALPHA_PLL_TYPE_TAYCAN_ELU] = {
		[PLL_OFF_OPMODE] = 0x04,
		[PLL_OFF_STATE] = 0x08,
		[PLL_OFF_STATUS] = 0x0c,
		[PLL_OFF_L_VAL] = 0x10,
		[PLL_OFF_ALPHA_VAL] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_USER_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_CONFIG_CTL_U] = 0x24,
		[PLL_OFF_CONFIG_CTL_U1] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x2c,
		[PLL_OFF_TEST_CTL_U] = 0x30,
	},
	[CLK_ALPHA_PLL_TYPE_RIVIAN_EVO] = {
		[PLL_OFF_OPMODE] = 0x04,
		[PLL_OFF_STATUS] = 0x0c,
		[PLL_OFF_L_VAL] = 0x10,
		[PLL_OFF_USER_CTL] = 0x14,
		[PLL_OFF_USER_CTL_U] = 0x18,
		[PLL_OFF_CONFIG_CTL] = 0x1c,
		[PLL_OFF_CONFIG_CTL_U] = 0x20,
		[PLL_OFF_CONFIG_CTL_U1] = 0x24,
		[PLL_OFF_TEST_CTL] = 0x28,
		[PLL_OFF_TEST_CTL_U] = 0x2c,
	},
	[CLK_ALPHA_PLL_TYPE_DEFAULT_EVO] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL_U] = 0x0c,
		[PLL_OFF_TEST_CTL] = 0x10,
		[PLL_OFF_TEST_CTL_U] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_USER_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_STATUS] = 0x24,
	},
	[CLK_ALPHA_PLL_TYPE_BRAMMO_EVO] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL_U] = 0x0c,
		[PLL_OFF_TEST_CTL] = 0x10,
		[PLL_OFF_TEST_CTL_U] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_CONFIG_CTL] = 0x1C,
		[PLL_OFF_STATUS] = 0x20,
	},
	[CLK_ALPHA_PLL_TYPE_STROMER] = {
		[PLL_OFF_L_VAL] = 0x08,
		[PLL_OFF_ALPHA_VAL] = 0x10,
		[PLL_OFF_ALPHA_VAL_U] = 0x14,
		[PLL_OFF_USER_CTL] = 0x18,
		[PLL_OFF_USER_CTL_U] = 0x1c,
		[PLL_OFF_CONFIG_CTL] = 0x20,
		[PLL_OFF_STATUS] = 0x28,
		[PLL_OFF_TEST_CTL] = 0x30,
		[PLL_OFF_TEST_CTL_U] = 0x34,
	},
	[CLK_ALPHA_PLL_TYPE_STROMER_PLUS] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_USER_CTL] = 0x08,
		[PLL_OFF_USER_CTL_U] = 0x0c,
		[PLL_OFF_CONFIG_CTL] = 0x10,
		[PLL_OFF_TEST_CTL] = 0x14,
		[PLL_OFF_TEST_CTL_U] = 0x18,
		[PLL_OFF_STATUS] = 0x1c,
		[PLL_OFF_ALPHA_VAL] = 0x24,
		[PLL_OFF_ALPHA_VAL_U] = 0x28,
	},
	[CLK_ALPHA_PLL_TYPE_ZONDA_OLE] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_USER_CTL] = 0x0c,
		[PLL_OFF_USER_CTL_U] = 0x10,
		[PLL_OFF_CONFIG_CTL] = 0x14,
		[PLL_OFF_CONFIG_CTL_U] = 0x18,
		[PLL_OFF_CONFIG_CTL_U1] = 0x1c,
		[PLL_OFF_CONFIG_CTL_U2] = 0x20,
		[PLL_OFF_TEST_CTL] = 0x24,
		[PLL_OFF_TEST_CTL_U] = 0x28,
		[PLL_OFF_TEST_CTL_U1] = 0x2c,
		[PLL_OFF_OPMODE] = 0x30,
		[PLL_OFF_STATUS] = 0x3c,
	},
	[CLK_ALPHA_PLL_TYPE_NSS_HUAYRA] =  {
		[PLL_OFF_L_VAL] = 0x04,
		[PLL_OFF_ALPHA_VAL] = 0x08,
		[PLL_OFF_TEST_CTL] = 0x0c,
		[PLL_OFF_TEST_CTL_U] = 0x10,
		[PLL_OFF_USER_CTL] = 0x14,
		[PLL_OFF_CONFIG_CTL] = 0x18,
		[PLL_OFF_CONFIG_CTL_U] = 0x1c,
		[PLL_OFF_STATUS] = 0x20,
	},

};


/*
 * Even though 40 bits are present, use only 32 for ease of calculation.
 */
#define ALPHA_REG_BITWIDTH	40
#define ALPHA_REG_16BIT_WIDTH	16
#define ALPHA_BITWIDTH		32U
#define ALPHA_SHIFT(w)		min(w, ALPHA_BITWIDTH)

#define	ALPHA_PLL_STATUS_REG_SHIFT	8

#define PLL_HUAYRA_M_WIDTH		8
#define PLL_HUAYRA_M_SHIFT		8
#define PLL_HUAYRA_M_MASK		0xff
#define PLL_HUAYRA_N_SHIFT		0
#define PLL_HUAYRA_N_MASK		0xff
#define PLL_HUAYRA_ALPHA_WIDTH		16

#define PLL_STANDBY		0x0
#define PLL_RUN			0x1
#define PLL_OUT_MASK		0x7
#define PLL_RATE_MARGIN		500

/* TRION PLL specific settings and offsets */
#define TRION_PLL_CAL_VAL	0x44
#define TRION_PCAL_DONE		BIT(26)

/* LUCID PLL specific settings and offsets */
#define LUCID_PCAL_DONE		BIT(27)

/* LUCID 5LPE PLL specific settings and offsets */
#define LUCID_5LPE_PCAL_DONE		BIT(11)
#define LUCID_5LPE_ALPHA_PLL_ACK_LATCH	BIT(13)
#define LUCID_5LPE_PLL_LATCH_INPUT	BIT(14)
#define LUCID_5LPE_ENABLE_VOTE_RUN	BIT(21)

/* LUCID EVO PLL specific settings and offsets */
#define LUCID_EVO_PCAL_NOT_DONE		BIT(8)
#define LUCID_EVO_ENABLE_VOTE_RUN       BIT(25)
#define LUCID_EVO_PLL_L_VAL_MASK        GENMASK(15, 0)
#define LUCID_EVO_PLL_CAL_L_VAL_SHIFT	16
#define LUCID_OLE_PLL_RINGOSC_CAL_L_VAL_SHIFT	24

/* PONGO ELU PLL specific setting and offsets */
#define PONGO_PLL_OUT_MASK		GENMASK(1, 0)
#define PONGO_PLL_L_VAL_MASK		GENMASK(11, 0)
#define PONGO_XO_PRESENT		BIT(10)
#define PONGO_CLOCK_SELECT		BIT(12)

/* ZONDA PLL specific */
#define ZONDA_PLL_OUT_MASK	0xf
#define ZONDA_STAY_IN_CFA	BIT(16)
#define ZONDA_PLL_FREQ_LOCK_DET	BIT(29)

#define wait_for_pll_update(pll) \
	wait_for_pll(pll, PLL_UPDATE, 1, "update")

#define wait_for_pll_update_ack_set(pll) \
	wait_for_pll(pll, ALPHA_PLL_ACK_LATCH, 0, "update_ack_set")

#define wait_for_pll_update_ack_clear(pll) \
	wait_for_pll(pll, ALPHA_PLL_ACK_LATCH, 1, "update_ack_clear")

#define wait_for_pll_enable_lock(pll) \
	wait_for_pll(pll, PLL_LOCK_DET, 0, "enable")

#define pll_alpha_width(p)					\
		((PLL_ALPHA_VAL_U(p) - PLL_ALPHA_VAL(p) == 4) ?	\
				 ALPHA_REG_BITWIDTH : ALPHA_REG_16BIT_WIDTH)

static void update_bits_32(phys_addr_t addr, uint32_t mask, uint32_t val)
{
	uint32_t reg;

	reg = readl(addr);

	reg &= ~mask;

	writel(reg | (val & mask), addr);
}

static void clk_alpha_pll_write_config(phys_addr_t base, unsigned int reg,
					unsigned int val)
{
	if (val)
		writel(val, base + reg);
}

static int wait_for_pll(struct clk_alpha_pll *pll, u32 mask, bool inverse,
			const char *action)
{
	u32 val;
	int count;
	const char *name = pll->name;

	val = readl(pll->base + PLL_MODE(pll));

	/* Pongo PLLs using a 32KHz reference can take upwards of 1500us to lock. */
	for (count = 1500; count > 0; count--) {
		val = readl(pll->base + PLL_MODE(pll));
		if (inverse && !(val & mask))
			return 0;
		else if ((val & mask) == mask)
			return 0;

		udelay(1);
	}

	WARN(1, "%s failed to %s!\n", name, action);
	return -ETIMEDOUT;
}

static int trion_pll_is_enabled(struct clk_alpha_pll *pll)
{
	u32 mode_val, opmode_val;
	phys_addr_t base = pll->base;

	mode_val = readl(base + PLL_MODE(pll));
	opmode_val = readl(base + PLL_OPMODE(pll));

	return ((opmode_val & PLL_RUN) && (mode_val & PLL_OUTCTRL));
}

int alpha_pll_lucid_evo_enable(struct clk_alpha_pll *pll)
{
	phys_addr_t base = pll->base;
	u32 val;
	int ret;

	/* U-Boot: actually from Linux prepare cb */
	/* Return early if calibration is not needed. */
	val = readl(pll->base + PLL_MODE(pll));
	if (!(val & LUCID_EVO_PCAL_NOT_DONE))
		return 0;

	val = readl(base + PLL_USER_CTL(pll));

	/* If in FSM mode, just vote for it */
	/* U-Boot: logic handled by drivers currently */
	// if (val & LUCID_EVO_ENABLE_VOTE_RUN) {
	// 	ret = clk_enable_regmap(hw);
	// 	if (ret)
	// 		return ret;
	// 	return wait_for_pll_enable_lock(pll);
	// }

	/* Check if PLL is already enabled */
	if (trion_pll_is_enabled(pll))
		return 0;

	setbits_32(base + PLL_MODE(pll), PLL_RESET_N);

	/* Set operation mode to RUN */
	writel(PLL_RUN, base + PLL_OPMODE(pll));

	ret = wait_for_pll_enable_lock(pll);
	if (ret)
		return ret;

	/* Enable the PLL outputs */
	setbits_32(base + PLL_USER_CTL(pll), PLL_OUT_MASK);

	/* Enable the global PLL outputs */
	setbits_32(base + PLL_MODE(pll), PLL_OUTCTRL);

	/* Ensure that the write above goes through before returning. */
	mb();
	return ret;
}

static unsigned long
alpha_pll_calc_rate(u64 prate, u32 l, u32 a, u32 alpha_width)
{
	return (prate * l) + ((prate * a) >> ALPHA_SHIFT(alpha_width));
}

static unsigned long
alpha_pll_round_rate(unsigned long rate, unsigned long prate, u32 *l, u64 *a,
		     u32 alpha_width)
{
	u64 remainder;
	u64 quotient;

	quotient = rate;
	remainder = do_div(quotient, prate);
	*l = quotient;

	if (!remainder) {
		*a = 0;
		return rate;
	}

	/* Upper ALPHA_BITWIDTH bits of Alpha */
	quotient = remainder << ALPHA_SHIFT(alpha_width);

	remainder = do_div(quotient, prate);

	if (remainder)
		quotient++;

	*a = quotient;
	return alpha_pll_calc_rate(prate, *l, *a, alpha_width);
}

/*
 * Due to limited number of bits for fractional rate programming, the
 * rounded up rate could be marginally higher than the requested rate.
 */
static int alpha_pll_check_rate_margin(struct clk_alpha_pll *pll,
			unsigned long rrate, unsigned long rate)
{
	unsigned long rate_margin = rate + PLL_RATE_MARGIN;

	if (rrate > rate_margin || rrate < rate) {
		pr_err("%s: Rounded rate %lu not within range [%lu, %lu)\n",
		       pll->name, rrate, rate, rate_margin);
		return -EINVAL;
	}

	return 0;
}

static int __alpha_pll_trion_set_rate(struct clk_alpha_pll *pll, unsigned long rate,
				      unsigned long prate, u32 latch_bit, u32 latch_ack)
{
	unsigned long rrate;
	u32 val, l, alpha_width = pll_alpha_width(pll);
	u64 a;
	int ret;

	rrate = alpha_pll_round_rate(rate, prate, &l, &a, alpha_width);
	printf("Using rate %lu (%lu * (%u + %llu * 2 ** -40))\n", rate, prate, l, a);

	ret = alpha_pll_check_rate_margin(pll, rrate, rate);
	if (ret < 0)
		return ret;

	update_bits_32(pll->base + PLL_L_VAL(pll), LUCID_EVO_PLL_L_VAL_MASK,  l);
	writel(a, pll->base + PLL_ALPHA_VAL(pll));

	/* Latch the PLL input */
	update_bits_32(pll->base + PLL_MODE(pll), latch_bit, latch_bit);

	/* Wait for 2 reference cycles before checking the ACK bit. */
	udelay(1);
	val = readl(pll->base + PLL_MODE(pll));
	if (!(val & latch_ack)) {
		pr_err("Lucid PLL latch failed. Output may be unstable!\n");
		return -EINVAL;
	}

	/* Return the latch input to 0 */
	update_bits_32(pll->base + PLL_MODE(pll), latch_bit, 0);

	if (trion_pll_is_enabled(pll)) {
		ret = wait_for_pll_enable_lock(pll);
		if (ret)
			return ret;
	}

	/* Wait for PLL output to stabilize */
	udelay(100);
	printf("%s: %s success!\n", pll->name, __func__);
	return 0;
}

// alpha_pll_lucid_5lpe_set_rate() in Linux
int alpha_trion_pll_set_rate(struct clk_alpha_pll *pll, unsigned long rate,
				    unsigned long prate)
{
	return __alpha_pll_trion_set_rate(pll, rate, prate,
					  LUCID_5LPE_PLL_LATCH_INPUT,
					  LUCID_5LPE_ALPHA_PLL_ACK_LATCH);
}

void clk_lucid_ole_pll_configure(struct clk_alpha_pll *pll,
				 const struct alpha_pll_config *config)
{
	u32 lval = config->l;

	lval |= TRION_PLL_CAL_VAL << LUCID_EVO_PLL_CAL_L_VAL_SHIFT;
	lval |= TRION_PLL_CAL_VAL << LUCID_OLE_PLL_RINGOSC_CAL_L_VAL_SHIFT;
	clk_alpha_pll_write_config(pll->base, PLL_L_VAL(pll), lval);
	clk_alpha_pll_write_config(pll->base, PLL_ALPHA_VAL(pll), config->alpha);
	clk_alpha_pll_write_config(pll->base, PLL_CONFIG_CTL(pll), config->config_ctl_val);
	clk_alpha_pll_write_config(pll->base, PLL_CONFIG_CTL_U(pll), config->config_ctl_hi_val);
	clk_alpha_pll_write_config(pll->base, PLL_CONFIG_CTL_U1(pll), config->config_ctl_hi1_val);
	clk_alpha_pll_write_config(pll->base, PLL_USER_CTL(pll), config->user_ctl_val);
	clk_alpha_pll_write_config(pll->base, PLL_USER_CTL_U(pll), config->user_ctl_hi_val);
	clk_alpha_pll_write_config(pll->base, PLL_TEST_CTL(pll), config->test_ctl_val);
	clk_alpha_pll_write_config(pll->base, PLL_TEST_CTL_U(pll), config->test_ctl_hi_val);
	clk_alpha_pll_write_config(pll->base, PLL_TEST_CTL_U1(pll), config->test_ctl_hi1_val);
	clk_alpha_pll_write_config(pll->base, PLL_TEST_CTL_U2(pll), config->test_ctl_hi2_val);

	/* Disable PLL output */
	clrbits_32(pll->base + PLL_MODE(pll), PLL_OUTCTRL);

	/* Set operation mode to STANDBY and de-assert the reset */
	writel(PLL_STANDBY, pll->base + PLL_OPMODE(pll));
	setbits_32(pll->base + PLL_MODE(pll), PLL_RESET_N);
}
