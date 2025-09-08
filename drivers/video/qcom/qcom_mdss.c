/* SPDX-License-Identifier: GPL-2.0-only */

/* MDSS handles UBWC in linux. But coreboot does not care UBWC coreboot for sc7180, revisit it later */

#include <dm/device.h>
#include <dm/device_compat.h>
#include <clk.h>
#include <reset.h>
#include <video.h>

#include "mdssreg.h"

struct qcom_mdss_priv {
	void *regs;
};

/* copy from rk_vop.c
We can parse all sub-nodes here, to make parsing nodes and topology easier
with referencing bootdev_get_sibling_blk() to get sub-nodes device */
int qcom_mdss_probe(struct udevice *dev)
{
printf("=========%s enter\n", __func__);
#if 0
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct rk_vop_priv *priv = dev_get_priv(dev);
	struct rkvop_driverdata *ops =
		(struct rkvop_driverdata *)dev_get_driver_data(dev);
	int ret = 0;
	ofnode port, node;
	struct reset_ctl ahb_rst;

	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	ret = reset_get_by_name(dev, "ahb", &ahb_rst);
	if (ret) {
		dev_err(dev, "failed to get ahb reset (ret=%d)\n", ret);
		return ret;
	}

	ret = reset_assert(&ahb_rst);
	if (ret) {
		dev_err(dev, "failed to assert ahb reset (ret=%d)\n", ret);
		return ret;
	}
	udelay(20);

	ret = reset_deassert(&ahb_rst);
	if (ret) {
		dev_err(dev, "failed to deassert ahb reset (ret=%d)\n", ret);
		return ret;
	}

#if defined(CONFIG_EFI_LOADER)
	debug("Adding to EFI map %d @ %lx\n", plat->size, plat->base);
	efi_add_memory_map(plat->base, plat->size, EFI_RESERVED_MEMORY_TYPE);
#endif

	priv->regs = dev_read_addr_ptr(dev);
	priv->win_offset = ops->win_offset;
	priv->dsp_offset = ops->dsp_offset;

	/*
	 * Try all the ports until we find one that works. In practice this
	 * tries EDP first if available, then HDMI.
	 *
	 * Note that rockchip_vop_set_clk() always uses NPLL as the source
	 * clock so it is currently not possible to use more than one display
	 * device simultaneously.
	 */
	port = dev_read_subnode(dev, "port");
	if (!ofnode_valid(port)) {
		debug("%s(%s): 'port' subnode not found\n",
		      __func__, dev_read_name(dev));
		return -EINVAL;
	}

	for (node = ofnode_first_subnode(port);
	     ofnode_valid(node);
	     node = dev_read_next_subnode(node)) {
		ret = rk_display_init(dev, plat->base, node);
		if (ret)
			debug("Device failed: ret=%d\n", ret);
		if (!ret)
			break;
	}
	video_set_flush_dcache(dev, 1);

	return ret;
#endif
	struct reset_ctl reset;
	struct clk_bulk bulk;
	int ret;

	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

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
printf("================== %s called\n", __func__);
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
//        .remove = rk_vop_remove,
	.priv_auto	= sizeof(struct qcom_mdss_priv),
	.flags  = DM_FLAG_PRE_RELOC | DM_FLAG_LEAVE_PD_ON,
};
