/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard log handling
 *
 */


#ifndef __DECKARD_LOG_H__
#define __DECKARD_LOG_H__

#include <linux/compiler_attributes.h> /* for __printf */

// Log with timestamp, and a prefix
void deckard_log_with_prefix(const char * pchPrefix, const char *fmt, ...) __printf(2, 3);

#ifdef CONFIG_SPL_BUILD
#define deckard_log(fmt, ...) deckard_log_with_prefix("uboot-spl", fmt, ##__VA_ARGS__)
#else
#define deckard_log(fmt, ...) deckard_log_with_prefix("uboot", fmt, ##__VA_ARGS__)
#endif

#endif /* __DECKARD_LOG_H__ */
