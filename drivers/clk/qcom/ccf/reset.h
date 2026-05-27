/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#ifndef __QCOM_CLK_RESET_H__
#define __QCOM_CLK_RESET_H__

#include <reset.h>

struct qcom_reset_map {
	unsigned int reg;
	u8 bit;
	u16 udelay;
	u32 bitmask;
};

struct regmap;

struct qcom_reset_controller {
	const struct qcom_reset_map *reset_map;
	int nr_resets;
	struct regmap *regmap;
};

int qcom_reset_register(struct udevice *dev, const struct qcom_reset_map *reset_map,
			int nr_resets, struct regmap *regmap);

#endif
