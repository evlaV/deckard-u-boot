// SPDX-License-Identifier: GPL-2.0+
/**
 * Platform driver for Qualcomm UFS
 *
 * Copyright (C) 2022 Jonathan Marek <jonathan@marek.ca>
 */

#include <clk.h>
#include <common.h>
#include <dm.h>
#include <ufs.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/err.h>

#include "ufs.h"

static int qcom_ufs_link_startup_notify(struct ufs_hba *hba,
					enum ufs_notify_change_status status)
{
	printf("qcom_ufs_link_startup_notify %d\n", status);

	// hba->quirks |= UFSHCD_QUIRK_BROKEN_UFS_HCI_VERSION;
	switch (status) {
	case PRE_CHANGE:
		return 0;//ufshcd_dme_set(hba,
			//	      UIC_ARG_MIB(PA_LOCAL_TX_LCC_ENABLE),
			//	      0);
	case POST_CHANGE:
	;
	}

	return 0;
}

static int qcom_ufs_hce_enable_notify(struct ufs_hba *hba,
				      enum ufs_notify_change_status status)
{
	printf("qcom_ufs_hce_enable_notify %d\n", status);

	switch (status) {
	case PRE_CHANGE:
		return 0;
		//return qcom_ufs_set_hclkdiv(hba);
	case POST_CHANGE:
	;
	}

	return 0;
}

static int qcom_ufs_init(struct ufs_hba *hba)
{
	return 0;
}

static struct ufs_hba_ops cdns_pltfm_hba_ops = {
	.init = qcom_ufs_init,
	.hce_enable_notify = qcom_ufs_hce_enable_notify,
	.link_startup_notify = qcom_ufs_link_startup_notify,
};

static int qcom_ufs_pltfm_probe(struct udevice *dev)
{
	int err = ufshcd_probe(dev, &cdns_pltfm_hba_ops);
	if (err)
		dev_err(dev, "ufshcd_probe() failed %d\n", err);

	return err;
}

static int qcom_ufs_pltfm_bind(struct udevice *dev)
{
	struct udevice *scsi_dev;

	return ufs_scsi_bind(dev, &scsi_dev);
}

static const struct udevice_id qcom_ufs_pltfm_ids[] = {
	{
		.compatible = "qcom,ufs",
	},
	{},
};

U_BOOT_DRIVER(qcom_ufs_pltfm) = {
	.name		= "qcom-ufs-pltfm",
	.id		=  UCLASS_UFS,
	.of_match	= qcom_ufs_pltfm_ids,
	.probe		= qcom_ufs_pltfm_probe,
	.bind		= qcom_ufs_pltfm_bind,
};
