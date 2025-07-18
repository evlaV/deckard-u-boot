// SPDX-License-Identifier: GPL-2.0+
/*
* Copyright 2025 Linaro Ltd.
*/
#include <asm/io.h>
#include <blk.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <errno.h>
#include <ext4fs.h>
#include <erofs.h>
#include <fs_loader.h>
#include <linux/err.h>
#include <linux/sizes.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <part.h>
#include <power-domain.h>
#include <remoteproc.h>
#include <reset.h>
#include <scsi.h>
#include <linux/delay.h>

#define FIRMWARE_INTERFACE "scsi"
#define FIRMWARE_PATH_PREFIX "/lib/firmware/"

#define QDSP6SS_RST_EVB 0x10

#define QDSP6SS_BOOT_CORE_START 0x400
#define QDSP6SS_BOOT_CMD 0x404
#define QDSP6SS_BOOT_STATUS 0x408

#define QDSP6SS_RESET_CFG 0x2440
#define QDSP6SS_RESET_CMD 0x2444
#define QDSP6SS_RESET_STATUS 0x2448

enum qcom_dsp_state {
	QCOM_DSP_STATE_INITIALIZED,
	QCOM_DSP_STATE_LOADED,
	QCOM_DSP_STATE_STARTED,
};

struct qcom_dsp {
	char *firmware_name;
	void __iomem *base;
	void __iomem *evb_sel;
	u32 boot_addr;
	struct clk_bulk clks;
	int num_mems;
	enum qcom_dsp_state state;
};

static int qcom_dsp_start(struct udevice *dev)
{
	struct qcom_dsp *dsp = dev_get_priv(dev);

	if (dsp->state == QCOM_DSP_STATE_STARTED) {
		printf("%s is already started: %d\n", dsp->firmware_name,
			dsp->state);
		return 0;
	} else if (dsp->state != QCOM_DSP_STATE_LOADED) {
		printf("%s is in wrong state: %d\n", dsp->firmware_name,
			dsp->state);
		return -EINVAL;
	}

	if (dsp->evb_sel)
		writel(0, dsp->evb_sel);

	writel(dsp->boot_addr >> 4, dsp->base + QDSP6SS_RST_EVB);
	writel(1, dsp->base + QDSP6SS_BOOT_CORE_START);
	udelay(10000);
	writel(1, dsp->base + QDSP6SS_BOOT_CMD);

	udelay(20000);

	dsp->state = QCOM_DSP_STATE_STARTED;

	printf("qcom_dsp: firmware started\n");

	return 0;
}

static int qcom_dsp_stop(struct udevice *dev)
{
	struct qcom_dsp *dsp = dev_get_priv(dev);

	writel(~0u, dsp->base +  QDSP6SS_RESET_CFG);
	writel(1, dsp->base +  QDSP6SS_RESET_CMD);
	udelay(20000);
	writel(0, dsp->base +  QDSP6SS_RESET_CFG);

	printf("qcom_dsp: firmware stopped\n");

	return 0;
}

static int qcom_dsp_load_ext4fs(struct qcom_dsp *dsp, char *buf)
{
	const char *dev_and_part = fs_firmware_get_partition();
	const char *ifname = FIRMWARE_INTERFACE;
	struct blk_desc *dev_desc = NULL;
	struct disk_partition info;
	loff_t len_read = 0;
	int part;
	int ret;

	printf("Loading firmware from partition: %s\n", dev_and_part);

	scsi_scan(true);

	part = blk_get_device_part_str(ifname, dev_and_part, &dev_desc, &info, 1);
	if (part < 0)
		return -ENODEV;

	ext4fs_set_blk_dev(dev_desc, &info);

	ret = ext4fs_mount();
	if (!ret) {
		printf("firmmware_ext4_load: Unable to mount %s %s", ifname,
			dev_and_part);
		return -EINVAL;
	}

	ret = ext4_read_file(dsp->firmware_name, buf, 0, CONFIG_REMOTEPROC_MAX_FW_SIZE,
			     &len_read);

	return len_read;
}


static int qcom_dsp_load_erofs(struct qcom_dsp *dsp, char *buf)
{
	const char *dev_and_part = fs_firmware_get_partition();
	const char *ifname = FIRMWARE_INTERFACE;
	loff_t len_read = 0;
	int ret;

	printf("Loading firmware from partition: %s\n", dev_and_part);

	scsi_scan(true);

	ret = fs_set_blk_dev(ifname, dev_and_part, FS_TYPE_EROFS);
	if (ret < 0) {
		printf("firmmware_erofs_load: Unable to set blkdev %s %s\n", ifname,
			dev_and_part);
		return ret;
	}

	ret = fs_read(dsp->firmware_name, (ulong)buf, 0, CONFIG_REMOTEPROC_MAX_FW_SIZE,
			&len_read);
	if (ret < 0) {
		printf("firmmware_erofs_load: fail to read %s %s\n", ifname,
			dev_and_part);
		return ret;
	}

	return len_read;
}

static int qcom_dsp_load(struct udevice *dev, ulong addr, ulong size)
{
	struct qcom_dsp *dsp = dev_get_priv(dev);
	int ret;

	if (dsp->state != QCOM_DSP_STATE_INITIALIZED) {
		printf("%s is in wrong state: %d\n", dsp->firmware_name,
			dsp->state);
		return -EINVAL;
	}

	ret = rproc_elf_load_image(dev, addr, size);
	if (ret < 0) {
		printf("qcom_dsp: %s: failed loading elf\n", __func__);
		return ret;
	}

	dsp->boot_addr = rproc_elf_get_boot_addr(dev, addr);

	dsp->state = QCOM_DSP_STATE_LOADED;

	printf("qcom_dsp: %s: dsp firmware loaded boot_addr: 0x%x\n", __func__,
		dsp->boot_addr);

	return 0;
}

static int qcom_dsp_init(struct udevice *dev)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, buf, CONFIG_REMOTEPROC_MAX_FW_SIZE);
	struct qcom_dsp *dsp = dev_get_priv(dev);
	int chip_id;
	int read;
	int ret;

#if 0
	read = qcom_dsp_load_ext4fs(dsp, buf);
	if (read < 0) {
		printf("ext4fs: failed to load: %s\n", dsp->firmware_name);
		return read;
	}
#else
	read = qcom_dsp_load_erofs(dsp, buf);
	if (read < 0) {
		printf("erofs: failed to load: %s\n", dsp->firmware_name);
		return read;
	}
#endif

	dsp->state = QCOM_DSP_STATE_INITIALIZED;

	chip_id = 0;

	ret = rproc_load(chip_id, (ulong)buf, read);
	if (ret < 0) {
		printf("rproc_load: failed to load: %s\n", dsp->firmware_name);
		return ret;
	}

	return 0;
}

static const struct dm_rproc_ops qcom_dsp_ops = {
	.init = qcom_dsp_init,
	.stop = qcom_dsp_stop,
	.load = qcom_dsp_load,
	.start = qcom_dsp_start,
};

static int qcom_dsp_clk_init(struct udevice *dev)
{
	struct qcom_dsp *dsp = dev_get_priv(dev);
	int ret;

	ret = clk_get_bulk(dev, &dsp->clks);
	if (ret == -ENOSYS || ret == -ENOENT)
		return 0;
	if (ret < 0)
		return ret;

	ret = clk_enable_bulk(&dsp->clks);
	if (ret < 0) {
		printf("qcom_dsp: %s fail to init get bulk\n", __func__);
		clk_release_bulk(&dsp->clks);
		return ret;
	}

	return 0;
}

static int qcom_dsp_probe(struct udevice *dev)
{
	struct qcom_dsp *dsp = dev_get_priv(dev);
	const char *firmware_name;
	int ret;

	dsp->base = (void __iomem *)dev_read_addr_name(dev, "base");
	dsp->evb_sel = (void __iomem *)dev_read_addr_name(dev, "evb_sel");

	ret = qcom_dsp_clk_init(dev);
	if (ret < 0)
		return ret;

	ret = dev_read_string_index(dev, "firmware-name", 0, &firmware_name);
	if (ret < 0)
		return ret;

	dsp->firmware_name = FIRMWARE_PATH_PREFIX;

	strncat(dsp->firmware_name, firmware_name, EXT2_PATH_MAX);

	return 0;
}

static int qcom_dsp_remove(struct udevice *dev)
{
	return 0;
}

static const struct udevice_id qcom_dsp_ids[] = {
	{ .compatible = "qcom,sm8650-hexagon" },
	{ },
};

U_BOOT_DRIVER(qcom_dsp) = {
	.name = "qcom_dsp",
	.of_match = qcom_dsp_ids,
	.id = UCLASS_REMOTEPROC,
	.ops = &qcom_dsp_ops,
	.probe = qcom_dsp_probe,
	.remove = qcom_dsp_remove,
	.priv_auto = sizeof(struct qcom_dsp),
};
