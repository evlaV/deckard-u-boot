// SPDX-License-Identifier: GPL-2.0+
/*
 * Qualcomm SM8550 memory map
 *
 * (C) Copyright 2022 Jonathan Marek <jonathan@marek.ca>
 */

#include <common.h>
#include <asm/armv8/mmu.h>

static struct mm_region sm8650_mem_map[] = {
	{
		.virt = 0x0UL, /* Peripheral block */
		.phys = 0x0UL, /* Peripheral block */
		.size = 0x10000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		.virt = 0x80000000UL, /* LOW DDR */
		.phys = 0x80000000UL, /* LOW DDR */
		.size = 0x80000000UL, /* 2GiB  */
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = sm8650_mem_map;
