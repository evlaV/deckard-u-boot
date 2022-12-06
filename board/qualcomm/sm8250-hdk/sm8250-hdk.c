// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2022 Jonathan Marek <jonathan@marek.ca>
 */

#include <init.h>
#include <env.h>
#include <common.h>
#include <asm/system.h>
#include <dm.h>

DECLARE_GLOBAL_DATA_PTR;

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

void reset_cpu(void)
{
	psci_system_reset();
}

int board_init(void)
{
	// usb_ether_init();
	return 0;
}
