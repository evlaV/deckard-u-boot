/* SPDX-License-Identifier: GPL-2.0+ */

#include <asm/types.h>
#include <string.h>

#define KSYM_NAME_LEN 127

#if CONFIG_IS_ENABLED(SYMBOL_LOOKUP)
const char * __attribute__((weak)) symbol_lookup(unsigned long addr, unsigned long *symaddr, unsigned long *offset,
			   char *namebuf);
#else
static inline const char *symbol_lookup(unsigned long addr, unsigned long *symaddr, unsigned long *offset,
			   char *namebuf)
{
	/* We can't find the actual symbol address so just use the search address */
	if (symaddr)
		*symaddr = addr;
	if (offset)
		*offset = 0;
	strcpy(namebuf, "???");
	namebuf[3] = '\0';
	return namebuf;
}
#endif

#if CONFIG_IS_ENABLED(FRAMEPOINTER)
/**
 * unwind_stack - unwind the callstack and print a backtrace.
 */
void unwind_stack(void);
#endif
