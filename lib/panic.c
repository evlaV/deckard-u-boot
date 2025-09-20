/*
 *  linux/lib/vsprintf.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/* vsprintf.c -- Lars Wirzenius & Linus Torvalds. */
/*
 * Wirzenius wrote this portably, Torvalds fucked it up :-)
 */

#include <hang.h>
#if !defined(CONFIG_PANIC_HANG)
#include <command.h>
#endif
#include <linux/delay.h>
#include <stdio.h>
#include <symbols.h>

static void panic_finish(void) __attribute__ ((noreturn));

static void panic_finish(void)
{
	putc('\n');
#if defined(CONFIG_PANIC_HANG)
	hang();
#else
	flush();  /* flush the panic message before reset */

	do_reset(NULL, 0, 0, NULL);
#endif
	while (1)
		;
}

void panic_str(const char *str)
{
	puts(str);
	panic_finish();
}

#if CONFIG_IS_ENABLED(ARM64)
void unwind_stack(void);
#endif

void panic(const char *fmt, ...)
{
#if CONFIG_IS_ENABLED(PRINTF)
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

#ifdef CONFIG_XPL_BUILD
 #ifdef SPL_FRAMEPOINTER
	puts("\n");
	unwind_stack();
 #endif
#else
 #ifdef FRAMEPOINTER
	puts("\n");
	unwind_stack();
 #endif
#endif

#endif
	panic_finish();
}

void __assert_fail(const char *assertion, const char *file, unsigned int line,
		   const char *function)
{
	/* This will not return */
	panic("%s:%u: %s: Assertion `%s' failed.", file, line, function,
	      assertion);
}
