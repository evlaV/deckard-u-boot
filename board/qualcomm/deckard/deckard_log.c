#include <stdarg.h>
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

void deckard_log_with_prefix(const char *prefix, const char *fmt, ...)
{
	va_list ap;
	unsigned long long us = ub_now_us();
	unsigned long long s  = us / 1000000ULL;
	unsigned long long u  = us % 1000000ULL;

	if (!prefix)
		prefix = "?";

	printf("[%5llu.%06llu] %s: ", s, u, prefix);

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}