/* SPDX-License-Identifier: GPL-2.0-only */

#include <dm/uclass.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <clk.h>
#include <reset.h>
#include <video.h>
#include <command.h>
#include <interconnect.h>

struct qcom_mdss_priv {
	struct reset_ctl reset;
	struct clk_bulk bulk;
	struct icc_path *path;
};

#define MIN_IB_BW	400000000UL /* Min ib vote 400MB */

int qcom_mdss_probe(struct udevice *dev)
{
	struct qcom_mdss_priv *priv = dev_get_priv(dev);
	int ret;

	priv->path = of_icc_get(dev, "mdp0-mem");
	if (!IS_ERR(priv->path))
		icc_set_bw(priv->path, 0, Bps_to_icc(MIN_IB_BW));

	ret = reset_get_by_index(dev, 0, &priv->reset);
	if (ret) {
		dev_err(dev, "reset get error %d\n", ret);
		return ret;
	}
	reset_assert(&priv->reset);
	reset_deassert(&priv->reset);

	ret = clk_get_bulk(dev, &priv->bulk);
	if (ret) {
		dev_err(dev, "clk bulk get error %d\n", ret);
		return ret;
	}

	return clk_enable_bulk(&priv->bulk);
}

static int qcom_mdss_bind(struct udevice *dev)
{
	return 0;
}

static int qcom_mdss_remove(struct udevice *dev)
{
	struct qcom_mdss_priv *priv = dev_get_priv(dev);

	reset_assert(&priv->reset);

	clk_disable_bulk(&priv->bulk);

	if (!IS_ERR(priv->path))
		icc_put(priv->path);

	return 0;
}

static const struct udevice_id mdss_ids[] = {
	{ .compatible = "qcom,sm8650-mdss" },
	{ }
};

U_BOOT_DRIVER(qcom_mdss) = {
	.name	= "qcom_mdss",
	.id	= UCLASS_SIMPLE_BUS,
	.of_match = mdss_ids,
	.bind	= qcom_mdss_bind,
	.probe	= qcom_mdss_probe,
	.remove	= qcom_mdss_remove,
	.priv_auto = sizeof(struct qcom_mdss_priv),
	.flags  = DM_FLAG_LEAVE_PD_ON,
};
