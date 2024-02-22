// SPDX-License-Identifier: BSD-3-Clause
/*
 * Clock drivers for Qualcomm SM8650
 *
 * (C) Copyright 2022 Jonathan Marek <jonathan@marek.ca>
 *
 */

#include <common.h>
#include <clk-uclass.h>
#include <dm.h>
#include <errno.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include "clock-qcom.h"

ulong msm_set_rate(struct clk *clk, ulong rate)
{
	return 0;
}

int msm_enable(struct clk *clk)
{
	return 0;
}
