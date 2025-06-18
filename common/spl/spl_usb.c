// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2014
 * Texas Instruments, <www.ti.com>
 *
 * Dan Murphy <dmurphy@ti.com>
 *
 * Derived work from spl_mmc.c
 */

#include <log.h>
#include <spl.h>
#include <errno.h>
#include <stdio.h>
#include <usb.h>
#include <fat.h>

static int usb_stor_curr_dev = -1; /* current device */

#ifndef CONFIG_SPL_OS_BOOT
int spl_start_uboot(void)
{
	return 1;
}
#endif

#ifdef CONFIG_SPL_FS_FAT
int spl_usb_load_fat(struct blk_desc *stor_dev,
		     struct spl_image_info *spl_image,
		     struct spl_boot_device *bootdev, int partition,
		     const char *filename)
{
	int ret;

	if (!spl_start_uboot()) {
		ret = spl_load_image_fat_os(spl_image, bootdev, stor_dev,
					    partition);
		if (!ret)
			return 0;
	}

	ret = spl_load_image_fat(spl_image, bootdev, stor_dev,
				 partition, filename);
	if (ret)
		return ret;

	return 0;
}
#endif

#ifdef CONFIG_SPL_FS_EXT4
int spl_usb_load_ext(struct blk_desc *stor_dev,
		     struct spl_image_info *spl_image,
		     struct spl_boot_device *bootdev, int partition,
		     const char *filename)
{
	int ret;

	if (!spl_start_uboot()) {
		ret = spl_load_image_ext_os(spl_image, bootdev, stor_dev,
					    partition);
		if (!ret)
			return 0;
	}

	ret = spl_load_image_ext(spl_image, bootdev, stor_dev, partition,
				 filename);
	if (ret)
		return ret;

	return 0;
}
#endif

int spl_usb_load(struct spl_image_info *spl_image,
		 struct spl_boot_device *bootdev, int partition,
		 const char *filename)
{
	int err = 0;
	struct blk_desc *stor_dev;
	static bool usb_init_pending = true;

	if (usb_init_pending) {
		usb_stop();
		err = usb_init();
		usb_init_pending = false;
	}

	if (err) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		printf("%s: usb init failed: err - %d\n", __func__, err);
#endif
		return err;
	}

	/* try to recognize storage devices immediately */
	usb_stor_curr_dev = usb_stor_scan(1);
	stor_dev = blk_get_devnum_by_uclass_id(UCLASS_USB, usb_stor_curr_dev);
	if (!stor_dev)
		return -ENODEV;


#ifdef CONFIG_SPL_FS_FAT
	debug("boot mode - FAT\n");
	err = spl_usb_load_fat(stor_dev, spl_image, bootdev, partition, filename);
	if (!err)
		return 0;
#endif

#ifdef CONFIG_SPL_FS_EXT4
	debug("boot mode - EXT4\n");
	err = spl_usb_load_ext(stor_dev, spl_image, bootdev, partition, filename);
	if (!err)
		return 0;
#endif

	if (err) {
		puts("Error loading from USB device\n");
		return err;
	}

	return 0;
}

static int spl_usb_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	return spl_usb_load(spl_image, bootdev,
			    CONFIG_SYS_USB_FAT_BOOT_PARTITION,
			    CONFIG_SPL_FS_LOAD_PAYLOAD_NAME);
}
SPL_LOAD_IMAGE_METHOD("USB", 0, BOOT_DEVICE_USB, spl_usb_load_image);
