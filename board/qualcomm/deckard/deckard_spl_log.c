// SPDX-License-Identifier: GPL-2.0+
/*
 * SPL console log, handed over to U-Boot proper. See deckard_spl_log.h.
 */

#include <console.h>
#include <mapmem.h>
#include <linux/libfdt.h>

#include "deckard_spl_log.h"

/*
 * console_tee_write() runs from the very first character SPL prints, before
 * BSS is cleared, so its state lives in .data.
 */
static bool s_log_ready __section(".data");

static void spl_log_putc(struct deckard_spl_log *log, char c)
{
	if (log->len < log->size)
		log->data[log->len++] = c;
	else
		log->dropped++;
}

void console_tee_write(const char *s, unsigned int len)
{
	struct deckard_spl_log *log;
	unsigned int i;

	log = map_sysmem(DECKARD_SPL_LOG_ADDR, DECKARD_SPL_LOG_SIZE);

	if (!s_log_ready) {
		log->magic = DECKARD_SPL_LOG_MAGIC;
		log->size = DECKARD_SPL_LOG_SIZE - sizeof(*log);
		log->len = 0;
		log->dropped = 0;
		s_log_ready = true;
	}

	for (i = 0; i < len; i++) {
		if (s[i] == '\n')
			spl_log_putc(log, '\r');
		spl_log_putc(log, s[i]);
	}
}

void deckard_spl_log_fixup(void *blob, int chosen)
{
	if (s_log_ready)
		fdt_setprop_u32(blob, chosen, DECKARD_SPL_LOG_PROP,
				DECKARD_SPL_LOG_ADDR);
}
