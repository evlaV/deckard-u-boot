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

#define DECKARD_UFS_ENV_LUN		2
#define DECKARD_UFS_ENV_PART		1
#define DECKARD_UFS_ENV_PART_ALT	2

#define DECKARD_UFS_BOOT_LUN		2
#define DECKARD_UFS_BOOT_PART_A		3
#define DECKARD_UFS_BOOT_PART_B		6

#define DECKARD_UFS_RESCUE_LUN		3
#define DECKARD_UFS_RESCUE_BOOT_PART	1

#define BOOT_ATTEMPTS_MAX			8

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

	//cleanup some env vars
	if (env_get("FORCE_RESCUE")) {
		env_set("FORCE_RESCUE",NULL); //delete this variable once we consume it
	}

	//now do fdt stuff
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
	struct udevice *dev;
	int ret;

	/* Scan u-boot lun to load fit image */
	ret = uclass_get_device_by_seq(UCLASS_SCSI, 0, &dev);
	if (ret)
		panic("SCSI device not found (%d)\n", ret);

	ret = scsi_scan_dev_lun(dev, true, DECKARD_UFS_BOOT_LUN);
	if (ret)
		panic("Failed to scan SCSI LUN (%d)\n", ret);

	/* Try to go into rescue first if too much attempts */
	if (env_get_ulong("FORCE_RESCUE", 10, 0)) {
		printf("BOOT_ORDER set to Rescue as primary\n");
		spl_boot_list[0] = BOOT_DEVICE_UFS_R;
		spl_boot_list[1] = BOOT_DEVICE_UFS_A;
		spl_boot_list[2] = BOOT_DEVICE_UFS_B;
	} else if (!strcmp(boot_order, "A B")) {
		printf("BOOT_ORDER set to A as primary\n");
		spl_boot_list[0] = BOOT_DEVICE_UFS_A;
		spl_boot_list[1] = BOOT_DEVICE_UFS_B;
		spl_boot_list[2] = BOOT_DEVICE_UFS_R;
	} else {
		printf("BOOT_ORDER set to B as primary\n");
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

static uint get_boot_order_first_entry(void)
{
	char *boot_order;
	u32 spl_boot_list[5];
	boot_order = check_boot_order();
	board_boot_order(spl_boot_list);
	return spl_boot_list[0];
}

static ulong iterate_boot_count(void)
{
	char *boot_count;
	ulong count = 0;
	ulong boot_count_max = BOOT_ATTEMPTS_MAX;
	uint boot_target;
	char * boot_count_target = NULL;
	boot_target = get_boot_order_first_entry();

	switch(boot_target)
	{
		case BOOT_DEVICE_UFS_A:
			boot_count_target = "BOOT_COUNT_A";
			boot_count_max = BOOT_ATTEMPTS_MAX;
			break;
		case BOOT_DEVICE_UFS_B:
			boot_count_target = "BOOT_COUNT_B";
			boot_count_max = BOOT_ATTEMPTS_MAX;
			break;
		case BOOT_DEVICE_UFS_R:
			return BOOT_ATTEMPTS_MAX;
			break;
		default:
			return BOOT_ATTEMPTS_MAX;
			break;
	}

	boot_count = env_get(boot_count_target);

	if (!boot_count) { 
		//variable doesnt exist, so set max boot count
		count = boot_count_max;
	}
	else
	{
		//variable exists, so decrement it until it hits 0
		count = simple_strtoul(boot_count, NULL, 10);
		if(count > 0)
		{
			count--;
		}
	}

	env_set_ulong(boot_count_target, count);

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
	char *boot_order;
	long count;
	int ret;
	int go_to_rescue = 0;
	ulong boot_count = 0;
	ulong boot_count_a = 0;
	ulong boot_count_b = 0;
	int boot_state_a = 0;
	int boot_state_b = 0;

	printf("Hello from deckard_spl, boot wrapper version %ld\n", boot_wrapper_version);

	ret = ufs_probe();
	if (ret)
		panic("Failed to probe for UFS (%d)\n", ret);

	/* Scan boot lun to load env */
	ret = uclass_get_device_by_seq(UCLASS_SCSI, 0, &dev);
	if (ret)
		panic("SCSI device not found (%d)\n", ret);

	ret = scsi_scan_dev_lun(dev, true, DECKARD_UFS_ENV_LUN);
	if (ret)
		panic("Failed to scan SCSI LUN (%d)\n", ret);	

	blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, 0);
	if (!blk_dev)
		panic("blk desc for env not found\n");

	blk_show_device(UCLASS_SCSI, 0);

	if (boot_wrapper_version == 9001)
	{
		go_to_rescue = 1;
		goto skip_env_update;
	}

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

	printf("Environment Variables:\n");
	printf("- BOOT_ORDER   = '%s'\n", env_get("BOOT_ORDER"));
	printf("- BOOT_COUNT_A = '%s'\n", env_get("BOOT_COUNT_A"));
	printf("- BOOT_COUNT_B = '%s'\n", env_get("BOOT_COUNT_B"));
	printf("- BOOT_STATE_A = '%s'\n", env_get("BOOT_STATE_A"));
	printf("- BOOT_STATE_B = '%s'\n", env_get("BOOT_STATE_B"));

	if(env_get("BOOT_STATE_A"))
	{
		boot_state_a = env_get_ulong("BOOT_STATE_A", 10, 0);
		if(boot_state_a)
		{
			//set boot count to max to allow it to be used
			env_set_ulong("BOOT_COUNT_A", BOOT_ATTEMPTS_MAX);
		}
		else
		{
			//set boot count to 0 so it won't try to be used
			env_set_ulong("BOOT_COUNT_A", 0);
		}
		env_set("BOOT_STATE_A",NULL); //always delete this variable after we process it
	}

	if(env_get("BOOT_STATE_B"))
	{
		boot_state_b = env_get_ulong("BOOT_STATE_B", 10, 0);
		if(boot_state_b)
		{
			//set boot count to max to allow it to be used
			env_set_ulong("BOOT_COUNT_B", BOOT_ATTEMPTS_MAX);
		}
		else
		{	
			//set boot count to 0 so it won't try to be used
			env_set_ulong("BOOT_COUNT_B", 0);
		}
		env_set("BOOT_STATE_B",NULL); //always delete this variable after we process it
	}

	if(env_get("BOOT_COUNT_A"))
	{
		boot_count_a = env_get_ulong("BOOT_COUNT_A", 10, 0);
	}

	if(env_get("BOOT_COUNT_B"))
	{
		boot_count_b = env_get_ulong("BOOT_COUNT_B", 10, 0);
	}

	//change BOOT_ORDER if we have exhausted boot attempts
	if (      boot_count_a == 0  &&  boot_count_b == 0 ) 
	{
		go_to_rescue = 1;
	}
	else if ( boot_count_a == 0  &&  boot_count_b  > 0 ) 
	{
		env_set("BOOT_ORDER", "B A");
	}
	else if ( boot_count_a >  0  &&  boot_count_b == 0 ) 
	{
		env_set("BOOT_ORDER", "A B");
	}
	else
	{
		//do nothing, normal state
	}

	boot_count = iterate_boot_count();
	printf("Boot count on target slot is %lu\n",boot_count);

	/* Export and write env in both partitions */
    /* NOTE: blk_write() size is in blocks ( 4k )*/
	ret = env_export(buf1);
	if (!ret) {
		ret = blk_write(blk_dev->bdev, info1.start, info1.size, buf1);
		if (!ret)
			printf("Unable to write env\n");
		ret = blk_write(blk_dev->bdev, info2.start, info2.size, buf1);
		if (!ret)
			printf("Unable to write env_alt\n");
	} else
		printf("Failed to export environment");
	
skip_env_update:
	if (go_to_rescue) {
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
		/* Since we scan a second LUN, it gets added as seq id 1 */
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, 1);
		if (!blk_dev) {
			printf("blk desc for %d %d not found\n", UCLASS_SCSI, 1);
			return -ENODEV;
		}

		blk_show_device(UCLASS_SCSI, 1);
	} else {
		struct udevice *dev;

		/* Scan the rescue LUN */
		ret = uclass_get_device_by_seq(UCLASS_SCSI, 0, &dev);
		if (ret)
			panic("SCSI device not found (%d)\n", ret);

		ret = scsi_scan_dev_lun(dev, true, DECKARD_UFS_RESCUE_LUN);
		if (ret)
			panic("Failed to scan rescue SCSI LUN (%d)\n", ret);

		/* Since we scan a third LUN, it gets added as seq id 2 */
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, 2);
		if (!blk_dev) {
			printf("blk desc for %d %d not found\n", UCLASS_SCSI, 2);
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
