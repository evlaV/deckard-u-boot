// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <dm/device.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <reset-uclass.h>
#include <linux/delay.h>

#include "reset.h"

#define to_qcom_reset_controller(r) \
	(dev_get_priv((r)->dev))

static int qcom_reset_set_assert(struct reset_ctl *rcdev, bool assert)
{
	struct qcom_reset_controller *rst;
	const struct qcom_reset_map *map;
	u32 mask;

	rst = to_qcom_reset_controller(rcdev);
	map = &rst->reset_map[rcdev->id];
	mask = map->bitmask ? map->bitmask : BIT(map->bit);

	regmap_update_bits(rst->regmap, map->reg, mask, assert ? mask : 0);

	/* Read back the register to ensure write completion, ignore the value */
	regmap_read(rst->regmap, map->reg, &mask);

	return 0;
}

static int qcom_reset_assert(struct reset_ctl *rcdev)
{
	return qcom_reset_set_assert(rcdev, true);
}

static int qcom_reset_deassert(struct reset_ctl *rcdev)
{
	return qcom_reset_set_assert(rcdev, false);
}

static int qcom_reset_of_xlate(struct reset_ctl *rcdev,
			struct ofnode_phandle_args *args)
{
	struct qcom_reset_controller *desc = dev_get_priv(rcdev->dev);

	if (args->args_count != 1) {
		debug("Invalid args_count: %d\n", args->args_count);
		return -EINVAL;
	}

	rcdev->id = args->args[0];

	if (rcdev->id > desc->nr_resets) {
		dev_err(rcdev->dev, "Invalid GDSC ID %ld\n", rcdev->id);
		return -EINVAL;
	}

	return 0;
}

static struct reset_ops qcom_reset_ops = {
	.rst_assert = qcom_reset_assert,
	.rst_deassert = qcom_reset_deassert,
	.of_xlate = qcom_reset_of_xlate,
};

U_BOOT_DRIVER(qcom_reset) = {
	.name = "qcom_reset",
	.id = UCLASS_RESET,
	.ops = &qcom_reset_ops,
	.flags = DM_FLAG_PRE_RELOC,
};

int qcom_reset_register(struct udevice *dev, const struct qcom_reset_map *reset_map,
			int nr_resets, struct regmap *regmap)
{
	int ret;
	char *name;
	struct qcom_reset_controller *desc;
	struct udevice *rst_dev;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->reset_map = reset_map;
	desc->nr_resets = nr_resets;
	desc->regmap = regmap;

	name = kzalloc(64, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	ret = snprintf(name, 64, "%s.%s", dev->name, "reset");
	if (ret < 0)
		return ret;

	printf("%s: desc %p\n", __func__, desc);
	ret = device_bind_with_driver_data(dev, DM_DRIVER_REF(qcom_reset),
					    strdup(name), 0, dev_ofnode(dev), &rst_dev);
	if (ret)
		return ret;

	dev_set_priv(rst_dev, desc);

	return 0;
}
