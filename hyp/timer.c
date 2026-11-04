// SPDX-License-Identifier: GPL-2.0+
/*
 * Delays and timeouts for EL2 code.
 *
 * U-Boot's udelay() runs on the timer state in the relocated gd, which EL2
 * cannot rely on once the guest owns it, so EL2 counts the generic timer's
 * physical counter itself.  The counter keeps running at the frequency the
 * firmware programmed into CNTFRQ_EL0; this board reads 0 there at EL2 on some
 * cores, and the counter is the 19.2 MHz crystal derivative, so that is the
 * fallback.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <linux/types.h>

#include "hv.h"

#define HYP_TIMER_DEFAULT_HZ	19200000ULL

static __hyp_data u64 cnt_hz = 0;

void __hyp hyp_timer_init(void)
{
	u64 hz = mrs(cntfrq_el0);

	cnt_hz = hz ? hz : HYP_TIMER_DEFAULT_HZ;
	hyp_clean_range(&cnt_hz, sizeof(cnt_hz));
	hprintf("timer: counter at %llu Hz (CNTFRQ_EL0 %llu)\n", cnt_hz, hz);
}

u64 __hyp hyp_ticks(void)
{
	asm volatile("isb");
	return mrs(cntpct_el0);
}

u64 __hyp hyp_ticks_us(void)
{
	return hyp_ticks() * 1000000ULL / cnt_hz;
}

bool __hyp hyp_ticks_elapsed(u64 since, u32 us)
{
	return hyp_ticks() - since >= (u64)us * cnt_hz / 1000000ULL;
}

void __hyp hyp_udelay(u32 us)
{
	u64 start = hyp_ticks();

	while (!hyp_ticks_elapsed(start, us))
		;
}
