// SPDX-License-Identifier: GPL-2.0+
/*
 * The hypervisor's own log.  hyp_printf() (hyp/hv.c) puts every line it prints into this ring as
 * well as on the debug UART; the console service on TCP port 2222 (hyp/vdev.c) takes the lines out
 * of it and sends them with the guest's console output, between two of its lines.  The UART is
 * slow and has no reader once the board is out of reach of the serial adapter; the ring keeps what
 * was said before the first client connected, and `log uart off` on the :2323 console stops the
 * (synchronous) UART output altogether.
 *
 * Lines are kept whole: when the ring is full the oldest lines go, never half of one.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <hyp/spinlock.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>

#include "hv.h"

#define HLOG_SIZE	(32 * 1024)

static DEFINE_SPINLOCK(hlog_lock);
static __hyp_data char hlog[HLOG_SIZE];
static __hyp_data u32 hlog_head, hlog_tail;	/* free-running; head - tail <= HLOG_SIZE */
static __hyp_data u32 hlog_lost;		/* lines that fell off the tail before anyone took them */
static __hyp_data bool hlog_uart = true;

/* Whether hyp_printf() writes the UART as well as the ring */
bool __hyp hyp_log_uart(void)
{
	return hlog_uart;
}

bool __hyp hyp_log_uart_set(bool on)
{
	hlog_uart = on;

	return hlog_uart;
}

/* Drop the oldest line (the tail moves past the next newline) */
static void __hyp hlog_drop_line(void)
{
	while (hlog_tail != hlog_head) {
		char c = hlog[hlog_tail++ % HLOG_SIZE];

		if (c == '\n')
			break;
	}
	hlog_lost++;
}

/**
 * hyp_log_put() - append one line (it ends with its newline) to the ring
 * @line: the formatted line
 * @len: its length, not counting a terminating NUL
 */
void __hyp hyp_log_put(const char *line, u32 len)
{
	unsigned long flags;

	if (!len || len > HLOG_SIZE)
		return;

	spin_lock_irqsave(&hlog_lock, flags);
	while (HLOG_SIZE - (hlog_head - hlog_tail) < len)
		hlog_drop_line();
	for (u32 i = 0; i < len; i++)
		hlog[hlog_head++ % HLOG_SIZE] = line[i];
	spin_unlock_irqrestore(&hlog_lock, flags);
}

/**
 * hyp_log_take() - move whole lines out of the ring
 * @buf: where they go
 * @size: room in @buf
 *
 * Returns the number of bytes, 0 when the ring is empty or its oldest line does not fit.
 */
u32 __hyp hyp_log_take(char *buf, u32 size)
{
	unsigned long flags;
	u32 n = 0, end = 0, avail;

	spin_lock_irqsave(&hlog_lock, flags);
	avail = hlog_head - hlog_tail;
	/* the longest run of whole lines that fits */
	for (u32 i = 0; i < avail && i < size; i++)
		if (hlog[(hlog_tail + i) % HLOG_SIZE] == '\n')
			end = i + 1;
	for (; n < end; n++)
		buf[n] = hlog[(hlog_tail + n) % HLOG_SIZE];
	hlog_tail += n;
	spin_unlock_irqrestore(&hlog_lock, flags);

	return n;
}

/* How many lines were lost to a full ring since the last call */
u32 __hyp hyp_log_lost(void)
{
	unsigned long flags;
	u32 lost;

	spin_lock_irqsave(&hlog_lock, flags);
	lost = hlog_lost;
	hlog_lost = 0;
	spin_unlock_irqrestore(&hlog_lock, flags);

	return lost;
}
