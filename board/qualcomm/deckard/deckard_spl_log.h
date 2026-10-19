/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SPL console log, handed over to U-Boot proper
 *
 * SPL copies everything it prints into a buffer in DRAM and points U-Boot
 * proper at it with /chosen/u-boot,spl-log. With the USB serial console
 * enabled, proper sends it to the USB host ahead of its own output.
 */

#ifndef __DECKARD_SPL_LOG_H
#define __DECKARD_SPL_LOG_H

#include <linux/types.h>

/*
 * Clear of SPL's load buffers (SYS_LOAD_ADDR, the FIT at 0xb0000000), its
 * BSS/stack/malloc area (0xa61f0000 and up) and U-Boot proper's
 * pre-relocation footprint (0xa0000000 and up). Proper copies the log out in
 * misc_init_r(), before anything else is loaded into memory.
 */
#define DECKARD_SPL_LOG_ADDR	0xa4000000UL
#define DECKARD_SPL_LOG_SIZE	0x100000UL
#define DECKARD_SPL_LOG_MAGIC	0x474c5344 /* "DSLG" */

/* /chosen property: address of struct deckard_spl_log */
#define DECKARD_SPL_LOG_PROP	"u-boot,spl-log"

struct deckard_spl_log {
	u32 magic;
	u32 size;	/* capacity of data[] */
	u32 len;	/* bytes stored in data[] */
	u32 dropped;	/* bytes lost because data[] was full */
	char data[];	/* terminal stream, "\n" already expanded to "\r\n" */
};

#ifdef CONFIG_XPL_BUILD
/**
 * deckard_spl_log_fixup() - point U-Boot proper at the SPL log
 *
 * @blob: U-Boot proper's device tree
 * @chosen: offset of its /chosen node
 */
void deckard_spl_log_fixup(void *blob, int chosen);
#endif

#endif /* __DECKARD_SPL_LOG_H */
