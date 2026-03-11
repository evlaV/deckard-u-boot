/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (C) 2019 DENX Software Engineering
 * Lukasz Majewski, DENX Software Engineering, lukma@denx.de
 *
 * Copyright (c) 2010-2011 Jeremy Kerr <jeremy.kerr@canonical.com>
 * Copyright (C) 2011-2012 Linaro Ltd <mturquette@linaro.org>
 */
#ifndef __LINUX_CLK_PROVIDER_H
#define __LINUX_CLK_PROVIDER_H

/*
 * flags used across common struct clk.  these flags should only affect the
 * top-level framework.  custom flags for dealing with hardware specifics
 * belong in struct clk_foo
 *
 * Please update clk_flags[] in drivers/clk/clk.c when making changes here!
 */
#define CLK_SET_RATE_GATE	BIT(0) /* must be gated across rate change */
#define CLK_SET_PARENT_GATE	BIT(1) /* must be gated across re-parent */
#define CLK_SET_RATE_PARENT	BIT(2) /* propagate rate change up one level */
#define CLK_IGNORE_UNUSED	BIT(3) /* do not gate even if unused */
				/* unused */
				/* unused */
#define CLK_GET_RATE_NOCACHE	BIT(6) /* do not use the cached clk rate */
#define CLK_SET_RATE_NO_REPARENT BIT(7) /* don't re-parent on rate change */
#define CLK_GET_ACCURACY_NOCACHE BIT(8) /* do not use the cached clk accuracy */
#define CLK_RECALC_NEW_RATES	BIT(9) /* recalc rates after notifications */
#define CLK_SET_RATE_UNGATE	BIT(10) /* clock needs to run to set rate */
#define CLK_IS_CRITICAL		BIT(11) /* do not gate, ever */
/* parents need enable during gate/ungate, set rate and re-parent */
#define CLK_OPS_PARENT_ENABLE	BIT(12)
/* duty cycle call may be forwarded to the parent clock */
#define CLK_DUTY_CYCLE_PARENT	BIT(13)

/*
 * U-Boot: for compatibility include the correct clk-provider.h
 * for either uccf (U-Boot CCF) or ccf_full (Linux CCF)
 */
#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
#include "clk-provider-ccf_full.h"
#else
#include "clk-provider-uccf.h"
#endif

#endif /* __LINUX_CLK_PROVIDER_H */
