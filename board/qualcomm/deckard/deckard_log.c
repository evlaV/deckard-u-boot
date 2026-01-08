#include <ansi.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "deckard_log.h"

static unsigned long long ub_now_us(void)
{
#ifdef CONFIG_TIMER
	return (unsigned long long) timer_get_us();
#else
	return (unsigned long long) get_timer(0) * 1000ULL;
#endif
}

static bool g_lastLineIsOverwrite = false;

void deckard_log_with_prefix( const char *prefix, bool overwrite, const char *fmt, ...)
{
	va_list ap;
	unsigned long long us = ub_now_us();
	unsigned long long s  = us / 1000000ULL;
	unsigned long long u  = us % 1000000ULL;

	if (!prefix)
		prefix = "?";

	bool newline = ( g_lastLineIsOverwrite != overwrite && !overwrite );

	printf("%s%s[%5llu.%06llu] %s: ",
		newline ?  "\n" : "", overwrite ? "\r" ANSI_CLEAR_LINE : "", s, u, prefix);

	g_lastLineIsOverwrite = overwrite;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
