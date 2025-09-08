/* SPDX-License-Identifier: GPL-2.0-only */

#include <dm/device.h>
#include <dm/device_compat.h>
#include <clk.h>
#include <reset.h>
#include <video.h>

int qcom_mdss_probe(struct udevice *dev)
{
	struct reset_ctl reset;
	struct clk_bulk bulk;
	int ret;

	ret = reset_get_by_index(dev, 0, &reset);
	if (ret) {
		dev_err(dev, "reset get error %d\n", ret);
		return ret;
	}
	reset_assert(&reset);
	reset_deassert(&reset);

	ret = clk_get_bulk(dev, &bulk);
	if (ret) {
		dev_err(dev, "clk bulk get error %d\n", ret);
		return ret;
	}

	return clk_enable_bulk(&bulk);
}

static int qcom_mdss_bind(struct udevice *dev)
{
	return 0;
}

static const struct udevice_id mdss_ids[] = {
	{ .compatible = "qcom,sm8650-mdss" },
	{ }
};

U_BOOT_DRIVER(rockchip_mdss) = {
	.name	= "qcom_mdss",
	.id	= UCLASS_SIMPLE_BUS,
	.of_match = mdss_ids,
	.bind	= qcom_mdss_bind,
	.probe	= qcom_mdss_probe,
	.flags  = DM_FLAG_LEAVE_PD_ON,
};
