#include <common.h>
#include <dm.h>
#include <env.h>
#include <init.h>
#include <asm/system.h>
#include <dm/device-internal.h>
#include <ufs.h>
#include <scsi.h>
#include <spl_load.h>
#include <spl.h>
#include <stdlib.h>
#include <env_internal.h>

#define BOOT_DEVICE_UFS_A	(BOOT_DEVICE_NONE + 1)
#define BOOT_DEVICE_UFS_B	(BOOT_DEVICE_NONE + 2)
#define BOOT_DEVICE_UFS_R	(BOOT_DEVICE_NONE + 3)

#define DECKARD_UFS_ENV_LUN		1
#define DECKARD_UFS_ENV_PART		12
#define DECKARD_UFS_ENV_PART_ALT	13

#define DECKARD_UFS_BOOT_LUN		2
#define DECKARD_UFS_BOOT_PART_A		1
#define DECKARD_UFS_BOOT_PART_B		3

#define DECKARD_UFS_RESCUE_LUN		3
#define DECKARD_UFS_RESCUE_BOOT_PART	1

#define BOOT_ATTEMPTS_SWAP		8
#define BOOT_ATTEMPTS_RESCUE		6

DECLARE_GLOBAL_DATA_PTR;

static ulong boot_wrapper_version __section(".data");

int g_boot_wrapper_version;
/**
 * Save x0 register value, which should encode the boot wrapper version
 */
void save_boot_params(ulong r0)
{
	boot_wrapper_version = r0;
	save_boot_params_ret();
}

void spl_perform_fixups(struct spl_image_info *spl_image)
{
	/* Use relocated FDT */
	void *blob = (void *)(((ulong)spl_image->load_addr + spl_image->size + 3) & ~3);
	int chosen;
	int ret;

	chosen = fdt_find_or_add_subnode(blob, 0, "chosen");
	if (chosen < 0) {
		printf("%s: could not find/create '/chosen'\n", __func__);
		return;
	}

	if (spl_image->boot_device == BOOT_DEVICE_UFS_A)
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "A");
	else if (spl_image->boot_device == BOOT_DEVICE_UFS_B)
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "B");
	else
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "R");
	if (ret) {
		printf("%s: could not add u-boot,spl-boot-device property\n", __func__);
		return;
	}

	ret = fdt_setprop_u32(blob, chosen, "boot-wrapper-version", boot_wrapper_version);
	if (ret) {
		printf("%s: could not add u-boot,spl-boot-device property\n", __func__);
		return;
	}
}

void board_boot_order(u32 *spl_boot_list)
{
	char *boot_order = env_get("BOOT_ORDER");

	/* Try to go into rescue first if too much attempts */
	if (env_get_ulong("FORCE_RESCUE", 10, 0)) {
		spl_boot_list[0] = BOOT_DEVICE_UFS_R;
		spl_boot_list[1] = BOOT_DEVICE_UFS_A;
		spl_boot_list[2] = BOOT_DEVICE_UFS_B;
	} else if (!strcmp(boot_order, "A B")) {
		spl_boot_list[0] = BOOT_DEVICE_UFS_A;
		spl_boot_list[1] = BOOT_DEVICE_UFS_B;
		spl_boot_list[2] = BOOT_DEVICE_UFS_R;
	} else {
		spl_boot_list[0] = BOOT_DEVICE_UFS_B;
		spl_boot_list[1] = BOOT_DEVICE_UFS_A;
		spl_boot_list[2] = BOOT_DEVICE_UFS_R;
	}
}

static char *check_boot_order(void)
{
	char *boot_order = env_get("BOOT_ORDER");

	if (!boot_order || 
	    (strcmp(boot_order, "A B") && strcmp(boot_order, "B A"))) {
		printf("Invalid BOOT_ORDER (%s), default to 'A B'\n", boot_order);
		env_set("BOOT_ORDER", "A B");
		return env_get("BOOT_ORDER");
	}

	return boot_order;
}

static ulong increase_boot_count(void)
{
	char *boot_count = env_get("BOOT_COUNT");
	ulong count;

	if (!boot_count) {
		printf("Missing BOOT_COUNT, restarting from 0\n");
		count = 0;
	} else
		count = simple_strtoul(boot_count, NULL, 10);

	count += 1;

	/* Set back BOOT_COUNT to 0, so we can retry after getting back from rescue */
	if (count > BOOT_ATTEMPTS_RESCUE) {
		printf("Attempting Rescue & resetting BOOT_COUNT to 0\n");
		env_set_ulong("BOOT_COUNT", 0);
	} else {
		printf("Increasing BOOT_COUNT to %ld\n", count);
		env_set_ulong("BOOT_COUNT", count);
	}

	return count;
}

void qcom_spl_board_init(void)
{
	struct udevice *dev;
	struct disk_partition info1;
	struct disk_partition info2;
	struct blk_desc *blk_dev;
	void *buf1 = (void *)(CONFIG_SYS_LOAD_ADDR);
	void *buf2 = NULL;
	ulong boot_count = 0;
	char *boot_order;
	long count;
	int ret;

	printf("Hello from deckard_spl, boot wrapper version %ld\n", boot_wrapper_version);

	ret = ufs_probe();
	if (ret)
		panic("Failed to probe for UFS (%d)\n", ret);

	ret = uclass_get_device_by_seq(UCLASS_SCSI, 0, &dev);
	if (ret)
		panic("SCSI device not found (%d)\n", ret);

	ret = scsi_scan_dev(dev, true);
	if (ret)
		panic("Failed to scan SCSI (%d)\n", ret);	

	blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, DECKARD_UFS_ENV_LUN);
	if (!blk_dev)
		panic("blk desc for env not found\n");

	blk_show_device(UCLASS_SCSI, DECKARD_UFS_ENV_LUN);

	if (boot_wrapper_version == 9001)
		goto skip_env_update;

	/* Read spl_env partition */
	ret = part_get_info(blk_dev, DECKARD_UFS_ENV_PART, &info1);
	if (ret == -ENOENT)
		panic("partition %d for env not found\n",
		       DECKARD_UFS_ENV_PART);

	count = blk_read(blk_dev->bdev, info1.start, info1.size, buf1);
	if (count < 0)
		panic("Failed to read env partition (%ld)\n", count);

	/* Load spl_alt_env right after the spl_env */
	buf2 = buf1 + (info1.size * blk_dev->blksz);

	/* Read spl_alt_env partition */
	ret = part_get_info(blk_dev, DECKARD_UFS_ENV_PART_ALT, &info2);
	if (ret == -ENOENT)
		panic("partition %d for env_alt not found\n",
		       DECKARD_UFS_ENV_PART_ALT);

	count = blk_read(blk_dev->bdev, info2.start, info2.size, buf2);
	if (count < 0)
		panic("Failed to read env_alt partition (%ld)\n", count);

	/* Open coded env_import_redund */
	ret = env_import(buf1, 1, 0);
	if (ret) {
		ret = env_import(buf2, 1, 0);
		if (ret)
			printf("Failed to import environment\n");
	}

	/* Sanitize BOOT_ORDER && BOOT_COUNT */
	boot_order = check_boot_order();
	boot_count = increase_boot_count();

	printf("Environment Variables:\n");
	printf("- BOOT_ORDER = '%s'\n", boot_order);
	printf("- BOOT_COUNT = '%ld'\n", boot_count);

	/* Swap back after a certain amount of attempts */
	if (boot_count == BOOT_ATTEMPTS_SWAP) {
		printf("Swapping back boot_order\n");
		if (!strcmp(boot_order, "A B"))
			env_set("BOOT_ORDER", "B A");
		else
			env_set("BOOT_ORDER", "A B");
	}

	/* Export and write env in both partitions */
	ret = env_export(buf1);
	if (!ret) {
		ret = blk_write(blk_dev->bdev, info1.start, ENV_SIZE, buf1);
		if (!ret)
			printf("Unable to write env\n");
		ret = blk_write(blk_dev->bdev, info2.start, ENV_SIZE, buf1);
		if (!ret)
			printf("Unable to write env_alt\n");
	} else
		printf("Failed to export environment");
	
skip_env_update:
	/* If it still fails after a certain amount of attempts, go into rescue */
	if (boot_wrapper_version == 9001 || boot_count > BOOT_ATTEMPTS_RESCUE) {
		printf("Forcing rescue boot\n");
		env_set_ulong("FORCE_RESCUE", 1);
	}
}

static ulong deckard_ufs_fit_read(struct spl_load_info *load, ulong file_offset,
				  ulong size, void *buf)
{
	struct blk_desc *blk_dev = (struct blk_desc *)load->priv;
	long count;

	debug("%s: off %lx size %lx buf %lx\n", __func__, file_offset, size, (ulong)buf);

	count = blk_read(blk_dev->bdev, file_offset / blk_dev->blksz,
			size / blk_dev->blksz, buf);
	if (count < 0)
		return 0;

	return count * blk_dev->blksz;
}

static int deckard_ufs_load_image(struct spl_image_info *spl_image,
				  struct spl_boot_device *bootdev)
{
	struct blk_desc *blk_dev;
	struct spl_load_info load;
	struct disk_partition info;
	unsigned int partid = DECKARD_UFS_BOOT_PART_A;
	int ret;

	if (bootdev->boot_device == BOOT_DEVICE_UFS_B)
		partid = DECKARD_UFS_BOOT_PART_B;
	else if (bootdev->boot_device == BOOT_DEVICE_UFS_R)
		partid = DECKARD_UFS_RESCUE_BOOT_PART;

	if (bootdev->boot_device == BOOT_DEVICE_UFS_A ||
	    bootdev->boot_device == BOOT_DEVICE_UFS_B) {
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, DECKARD_UFS_BOOT_LUN);
		if (!blk_dev) {
			printf("blk desc for %d %d not found\n", UCLASS_SCSI, DECKARD_UFS_BOOT_LUN);
			return -ENODEV;
		}

		blk_show_device(UCLASS_SCSI, 1);
	} else {
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, DECKARD_UFS_RESCUE_LUN);
		if (!blk_dev) {
			printf("blk desc for %d %d not found\n", UCLASS_SCSI, DECKARD_UFS_RESCUE_LUN);
			return -ENODEV;
		}

		blk_show_device(UCLASS_SCSI, 2);
	}

	ret = part_get_info(blk_dev, partid, &info);
	if (ret == -ENOENT) {
		printf("partition %d for blk desc for %d %d not found\n",
		       partid, UCLASS_SCSI, DECKARD_UFS_BOOT_LUN);
		return -ENODEV;
	}

	load.read = deckard_ufs_fit_read;
	load.priv = blk_dev;
	load.bl_len = blk_dev->blksz;

	return spl_load(spl_image, bootdev, &load,
			info.size * blk_dev->blksz,
			info.start * blk_dev->blksz);
}

SPL_LOAD_IMAGE_METHOD("UFS_A", 0, BOOT_DEVICE_UFS_A, deckard_ufs_load_image);
SPL_LOAD_IMAGE_METHOD("UFS_B", 0, BOOT_DEVICE_UFS_B, deckard_ufs_load_image);
SPL_LOAD_IMAGE_METHOD("RESCUE", 0, BOOT_DEVICE_UFS_R, deckard_ufs_load_image);
