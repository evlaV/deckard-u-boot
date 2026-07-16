/* SPDX-License-Identifier: GPL-2.0-only */

#define pr_fmt(fmt) "[MDSS] "fmt
#define LOG_DEBUG

#include <dm/uclass.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <clk.h>
#include <reset.h>
#include <video.h>
#include <command.h>
#include <interconnect.h>
#include <panel.h>

#include "qcom_mipi_dsi.h"

struct qcom_mdss_priv {
	struct reset_ctl reset;
	struct clk_bulk bulk;
	unsigned long core_clk_max_rate;
	struct icc_path *path;
};

#define MIN_IB_BW	400000000UL /* Min ib vote 400MB */

#define	DPU_PERF_DEFAULT_MAX_CORE_CLK_RATE	412500000

int qcom_mdss_probe(struct udevice *dev)
{
	struct qcom_mdss_priv *priv = dev_get_priv(dev);
	struct clk core_clk;
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

	// FIXME: don't hardcode this!
	clk_set_rate(&priv->bulk.clks[2], UINT32_MAX);

	priv->core_clk_max_rate = clk_get_rate(&priv->bulk.clks[2]);
	pr_debug("MDP clocked at: %lu\n", priv->core_clk_max_rate);
	if (!priv->core_clk_max_rate)
		return -EINVAL;

	return clk_enable_bulk(&priv->bulk);
}

static int qcom_mdss_child_pre_probe(struct udevice *dev)
{
	// FIXME: priv is NULL ???
	// struct qcom_mdss_priv *priv = dev_get_parent_priv(dev);
	// struct qcom_mdss_parent_plat *pplat = dev_get_parent_plat(dev);

	// pplat->core_clk_max_rate = priv->core_clk_max_rate;

	return 0;
}

static int qcom_mdss_remove(struct udevice *dev)
{
	struct qcom_mdss_priv *priv = dev_get_priv(dev);

	printf("%s!\n", __func__);

	clk_disable_bulk(&priv->bulk);

	if (!IS_ERR(priv->path))
		icc_put(priv->path);

	reset_assert(&priv->reset);

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
	.probe	= qcom_mdss_probe,
	.remove	= qcom_mdss_remove,
	.priv_auto = sizeof(struct qcom_mdss_priv),
	.per_child_plat_auto = sizeof(struct qcom_mdss_parent_plat),
	.child_pre_probe = qcom_mdss_child_pre_probe,
	.flags	= DM_FLAG_LEAVE_PD_ON | DM_FLAG_OS_PREPARE,
};

static int do_mdss_ctrl(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	struct udevice *dev;
	struct uclass *uc;
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

	if (!strcmp(argv[1], "dump")) {
		printf("# MDSS DUMP\n");
		uclass_get(UCLASS_DSI_HOST, &uc);
		uclass_foreach_dev(dev, uc) {
			printf("DSI dev %s\n", dev->name);
			if (!device_active(dev))
				continue;
			qcom_dsi_do_dump(dev);
		}

		if (!uclass_get_device_by_driver(UCLASS_VIDEO, DM_DRIVER_GET(qcom_dpu), &dev))
			qcom_dpu_dump_state(dev);
		else
		 	printf("Couldn't get DPU device");

		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "panel")) {
		printf("# Panel info\n");
		uclass_get(UCLASS_PANEL, &uc);
		uclass_foreach_dev(dev, uc) {
			printf("Panel %s\n", dev->name);
			if (!device_active(dev))
				continue;
			
			const struct panel_ops *ops = dev_get_driver_ops(dev);
			ops->dump(dev);
		}
	}
	
	return CMD_RET_USAGE;
}

U_BOOT_LONGHELP(mdss,
	"stop - stop MDSS\n"
	"dump - dump DSI/DPU status");
U_BOOT_CMD(mdss, CONFIG_SYS_MAXARGS,  1,  do_mdss_ctrl, "MDSS control", mdss_help_text);
