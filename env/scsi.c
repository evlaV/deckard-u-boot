// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on SCSI implmentation.  Env file
 * saved on raw partition, not mountable ( eg ext4 )
*/

#include <part.h>

#include <command.h>
#include <env.h>
#include <env_internal.h>
#include <linux/stddef.h>
#include <malloc.h>
#include <memalign.h>
#include <search.h>
#include <errno.h>
#include <scsi.h>
#include <virtio.h>
#include <asm/global_data.h>

DECLARE_GLOBAL_DATA_PTR;

__weak const char *env_scsi_get_dev_part(void)
{
	return (const char *)CONFIG_ENV_SCSI_DEVICE_AND_PART;
}

static int env_scsi_save_buffer(env_t *env_new)
{
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int ret, part;
	const char *dev_and_part = env_scsi_get_dev_part();

	part = blk_get_device_part_str("scsi", dev_and_part,
					&blk_dev, &info, 1);
	if (part < 0)
		return 1;

	/* NOTE: blk_write() size is in blocks ( 4k ) */
	// TODO Should we check info.size against CONFIG_ENV_SIZE
	ret = blk_write(blk_dev->bdev, info.start, info.size, env_new);
	if (!ret)
		printf("Unable to write env\n");


	return 0;
}

static int env_scsi_save(void)
{
	env_t env_new;
	int err;

	err = env_export(&env_new);
	if (err)
		return err;

	err = env_scsi_save_buffer(&env_new);
	if (err)
		return err;

	gd->env_valid = ENV_VALID;
	puts("done\n");

	return 0;
}

static int env_scsi_erase(void)
{
	env_t env_new;
	int err;

	memset(&env_new, 0, sizeof(env_t));

	err = env_scsi_save_buffer(&env_new);
	if (err)
		return err;

	gd->env_valid = ENV_INVALID;
	puts("done\n");

	return 0;
}

static int env_scsi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, buf, CONFIG_ENV_SIZE);
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int err, part;
	ulong count;
	char *default_bootargs;
	const char *dev_and_part = env_scsi_get_dev_part();

	scsi_scan(true);

	part = blk_get_device_part_str("scsi", dev_and_part,
						&blk_dev, &info, 1);
	if (part < 0)
		goto err_env_relocate;

	count = blk_read(blk_dev->bdev, info.start, CONFIG_ENV_SIZE < info.size * info.blksz ? CONFIG_ENV_SIZE / info.blksz : info.size, buf);
	if (count < 0)
		panic("Failed to read env partition (%ld)\n", count);

	err = env_import(buf, 1, H_EXTERNAL);
	if (!err)
		gd->env_valid = ENV_VALID;

	return err;

err_env_relocate:
	env_set_default(NULL, 0);

	return -EIO;
}

U_BOOT_ENV_LOCATION(scsi) = {
	.location	= ENVL_SCSI,
	ENV_NAME("SCSI")
	.load		= env_scsi_load,
	.save		= ENV_SAVE_PTR(env_scsi_save),
	.erase		= ENV_ERASE_PTR(env_scsi_erase),
};
