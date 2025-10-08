/* SPDX-License-Identifier: GPL-2.0-only */

#include <dm/uclass.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <clk.h>
#include <reset.h>
#include <video.h>
#include <command.h>

struct qcom_mdss_priv {
	struct reset_ctl reset;
	struct clk_bulk bulk;
};

int qcom_mdss_probe(struct udevice *dev)
{
	struct qcom_mdss_priv *priv = dev_get_priv(dev);
	int ret;

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

static int do_mdss_ctrl(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	struct udevice *dev;
	int ret;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "stop")) {
		ret = uclass_get_device_by_driver(UCLASS_SIMPLE_BUS,
				DM_DRIVER_GET(qcom_mdss),
				&dev);
		if (ret) {
			printf("Failed to get MDSS !\n");
			return CMD_RET_FAILURE;
		}

		ret = device_remove(dev, DM_REMOVE_NORMAL);
		if (ret){
			printf("Failed to remove MDSS !\n");
			return CMD_RET_FAILURE;
		}

		return CMD_RET_SUCCESS;
	}
	
	return CMD_RET_USAGE;
}

U_BOOT_LONGHELP(mdss,
	"stop - stop MDSS");
U_BOOT_CMD(mdss, CONFIG_SYS_MAXARGS,  1,  do_mdss_ctrl, "MDSS control", mdss_help_text);
