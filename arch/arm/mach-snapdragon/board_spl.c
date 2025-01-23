// SPDX-License-Identifier: GPL-2.0+
/*
 * Common initialisation for Qualcomm Snapdragon boards.
 *
 * Copyright (c) 2024 Linaro Ltd.
 * Author: Caleb Connolly <caleb.connolly@linaro.org>
 */

#define LOG_CATEGORY LOGC_BOARD
#define pr_fmt(fmt) "QCOM: " fmt

#include <asm/armv8/mmu.h>
#include <asm/io.h>
#include <asm/system.h>
#include <dm/device.h>
#include <env.h>
#include <fdt_support.h>
#include <init.h>
#include <linux/bug.h>
#include <linux/sizes.h>
#include <fdt_support.h>

#include "qcom-priv.h"

#include <spl.h>

struct legacy_img_hdr *spl_get_load_buffer(ssize_t offset, size_t size)
{
	return (void *)(CONFIG_SYS_LOAD_ADDR);
}

/* Define board_boot_order() from board code to actually boot from something */
u32 spl_boot_device()
{
	return BOOT_DEVICE_NONE;
}

/* We support booting SPL with an internal DT */
int board_fdt_blob_setup(void **fdtp)
{
	struct fdt_header *internal_fdt = (struct fdt_header *)*fdtp;

	if (fdt_check_header(internal_fdt))
		panic("Internal FDT is invalid!\n");

	/*
	 * Parse the /memory node while we're here,
	 * this makes it easy to do other things early.
	 */
	return qcom_parse_memory(internal_fdt, true);
}

void __weak qcom_spl_board_init(void)
{
}

void spl_board_init(void)
{
	icache_enable();

	qcom_spl_board_init();
}
