// SPDX-License-Identifier: GPL-2.0+
/*
 * Hypervisor locking primitives: ticket spinlock and owner-tracked mutex.
 *
 * The API is Linux's (include/hyp/spinlock.h, include/hyp/mutex.h); the
 * implementation is ours because nothing in this tree has a working lock:
 * include/linux/compat.h makes spin_* and mutex_* no-ops, and U-Boot has no
 * SMP, no scheduler and no sleeping.  What these locks are for is the state
 * the hypervisor shares between its EL2 trap path and EL1 callers - the vGIC
 * list registers, the stage-2 watch table - not contention between CPUs
 * (there is one core today).  scripts/LOCKING.md has the reasoning.
 *
 * Spinlock: a ticket lock.  An acquirer takes its number from lock->next with
 * a load-exclusive/store-exclusive pair, then waits for lock->owner to reach
 * it.  Fair, and a waiter never contends with the holder: it only reads.
 * Release is a single store-release (stlr), which is also the release barrier
 * for everything the critical section wrote.
 *
 * Everything here is __hyp: the code and the data it touches stay in the
 * resident .text.hyp/.data.hyp sections, so EL1 and EL2 run against the same
 * copy of both.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <log.h>
#include <asm/barriers.h>
#include <hyp/spinlock.h>
#include <hyp/mutex.h>
#include <linux/bitfield.h>
#include <linux/compiler.h>
#include <linux/types.h>

#include "hv.h"

/*
 * Idle hint for a spin loop.  YIELD is a plain hint at any EL - a NOP today,
 * and the hook for WFE/SEV once there is more than one core.
 */
static __hyp void hyp_relax(void)
{
	asm volatile("yield" ::: "memory");
}

/*
 * Claim the next ticket.  ldxr/stxr on lock->next is the whole arbitration:
 * a failed store-exclusive means another acquirer took the number first, so
 * we re-read the tail and try again.  Returns the ticket we now hold.
 */
static __hyp u32 fetch_ticket(struct spinlock *lock)
{
	u32 tail, ticket;
	int failed;

	do {
		asm volatile("ldxr %w0, [%1]"
			     : "=r"(tail)
			     : "r"(&lock->next)
			     : "memory");
		ticket = tail + 1;
		asm volatile("stxr %w0, %w1, [%2]"
			     : "=&r"(failed)
			     : "r"(ticket), "r"(&lock->next)
			     : "memory");
	} while (failed);

	return tail;
}

void __hyp spin_lock_init(struct spinlock *lock)
{
	lock->owner = 0;
	lock->next = 0;
}

void __hyp spin_lock(struct spinlock *lock)
{
	u32 ticket = fetch_ticket(lock);
	ulong start = hyp_ticks();

	/* Order the claim ahead of the critical section, then wait our turn. */
	dmb();
	while (true) {
		for (ulong i = 0; i < 1000; i++) {
			if (READ_ONCE(lock->owner) == ticket)
				goto out;
			hyp_relax();
		}
		if (hyp_ticks_elapsed(start, 5 * 1000 * 1000)) {
			printf("[HYP] cpu%d: Lock %s held for 5 seconds! DEADLOCKED\n", hyp_cpu_id(mrs(mpidr_el1)), lock->name);
			start = ULONG_MAX;
		}
	}
out:
	dmb();
}

int __hyp spin_trylock(struct spinlock *lock)
{
	u32 tail;
	int failed;

	asm volatile("ldxr %w0, [%1]"
		     : "=r"(tail)
		     : "r"(&lock->next)
		     : "memory");

	/* Somebody else's ticket is still in office: drop the monitor and fail. */
	if (READ_ONCE(lock->owner) != tail) {
		asm volatile("clrex" ::: "memory");
		return 0;
	}

	asm volatile("stxr %w0, %w1, [%2]"
		     : "=&r"(failed)
		     : "r"(tail + 1), "r"(&lock->next)
		     : "memory");
	dmb();

	return !failed;
}

void __hyp spin_unlock(struct spinlock *lock)
{
	/*
	 * stlr publishes the new owner with release semantics: the store is
	 * not visible before the critical section's writes are.
	 */
	asm volatile("stlr %w0, [%1]"
		     :
		     : "r"(lock->owner + 1), "r"(&lock->owner)
		     : "memory");
}

int __hyp spin_is_locked(const struct spinlock *lock)
{
	return READ_ONCE(lock->owner) != READ_ONCE(lock->next);
}

/*
 * Who owns a mutex: the vCPU slot + 1, so 0 stays "unowned".  MPIDR_EL1 reads
 * 0x81000000 on every core of this board, which made every owner identical;
 * TPIDR_EL2 (hyp_cpu_num()) is what says which core this is.  A core that has no
 * context yet - only CPU0 early in hv_init() - gets the 0xff slot.
 */
static __hyp u32 cpu_id(void)
{
	int n = hyp_cpu_num();

	return (n < 0 ? 0xff : n) + 1;
}

void __hyp mutex_init(struct mutex *mutex)
{
	mutex->owner = 0;
}

int __hyp mutex_trylock(struct mutex *mutex)
{
	u32 owner;
	int failed;

	asm volatile("ldxr %w0, [%1]"
		     : "=r"(owner)
		     : "r"(&mutex->owner)
		     : "memory");
	if (owner) {
		asm volatile("clrex" ::: "memory");
		return 0;
	}

	asm volatile("stxr %w0, %w1, [%2]"
		     : "=&r"(failed)
		     : "r"(cpu_id()), "r"(&mutex->owner)
		     : "memory");
	dmb();

	return !failed;
}

/*
 * Spins, like every other lock in this hypervisor: there is nothing to sleep
 * in.  Recursive locking deadlocks here exactly as it does in Linux.
 */
void __hyp mutex_lock(struct mutex *mutex)
{
	while (!mutex_trylock(mutex))
		hyp_relax();
}

void __hyp mutex_unlock(struct mutex *mutex)
{
	u32 free = 0;

	asm volatile("stlr %w0, [%1]"
		     :
		     : "r"(free), "r"(&mutex->owner)
		     : "memory");
}

int __hyp mutex_is_locked(const struct mutex *mutex)
{
	return READ_ONCE(mutex->owner) != 0;
}
