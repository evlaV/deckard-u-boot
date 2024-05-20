/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2022 Jonathan Marek <jonathan@marek.ca>
 *
 */

#ifndef __CONFIGS_SM8650_HDK_H
#define __CONFIGS_SM8650_HDK_H

#include <linux/sizes.h>
#include <asm/arch/sysmap-sm8650.h>

#define CFG_SYS_BAUDRATE_TABLE	{ 115200 }

#define CFG_SYS_INIT_RAM_ADDR 0xf0000000
#define CFG_SYS_INIT_RAM_SIZE 0x00100000

#endif
