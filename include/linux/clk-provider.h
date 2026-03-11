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
 * U-Boot: for compatibility include the correct clk-provider.h
 * for either uccf (U-Boot CCF) or ccf_full (Linux CCF)
 */
#if CONFIG_IS_ENABLED(CLK_CCF_FULL)
#include "clk-provider-ccf_full.h"
#else
#include "clk-provider-uccf.h"
#endif

#endif /* __LINUX_CLK_PROVIDER_H */
