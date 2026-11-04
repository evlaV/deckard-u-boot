// SPDX-License-Identifier: GPL-2.0+
/*
 * Hypervisor spinlock: the Linux spinlock API, implemented for real.
 *
 * include/linux/compat.h turns spin_lock()/spin_unlock()/... into no-ops
 * (spinlock_t is unused_t there).  This header #undefs those macros so a
 * translation unit that sees both gets working primitives instead of the
 * stubs - include it *after* compat.h (which <dm/device_compat.h> and
 * <dm/devres.h> pull in); a later compat.h include would re-install the
 * no-ops.  Nothing in hyp/ includes compat.h today.
 *
 * Ticket lock: each acquirer claims a number from lock->next with a
 * load-exclusive/store-exclusive pair, then spins on lock->owner until it
 * reaches its number.  FIFO, one exclusive pair per acquisition, and the wait
 * loop is a plain read - a waiter never hammers the line with a failed CAS.
 *
 * Deliberately not the full Linux spinlock (scripts/LOCKING.md): spin_lock() does
 * not touch DAIF - use spin_lock_irqsave() for that - there is no recursion
 * detection (Linux has none either; a recursive spin_lock() deadlocks), and no
 * owner tracking.  Single-core today: the value of these locks is the state
 * they guard between the EL2 trap path and EL1 callers, not contention
 * between CPUs.
 */
#ifndef __HYP_SPINLOCK_H
#define __HYP_SPINLOCK_H

#include <hypervisor.h>
#include <asm/barriers.h>
#include <asm/system.h>
#include <linux/compiler.h>
#include <linux/types.h>

#undef spin_lock_init
#undef spin_lock
#undef spin_trylock
#undef spin_unlock
#undef spin_is_locked
#undef spin_lock_irqsave
#undef spin_unlock_irqrestore
#undef DEFINE_SPINLOCK

struct spinlock {
	u32 owner;				/* ticket currently holding the lock */
	u32 next;				/* ticket handed out to the next acquirer */
	const char *name;
};

#define __SPINLOCK_UNLOCKED			{ 0, 0 }

/*
 * DEFINE_SPINLOCK() puts the object in .data.hyp, not .bss: hyp code runs at
 * its link-time addresses, where .bss is never zeroed - crt0_64.S clears BSS
 * after relocation, at the relocated copy's address - so a resident lock has
 * to be initialized data.  Same reason hyp/gic.c and hyp/mmio.c keep their
 * state in __hyp_data.
 */
#define DEFINE_SPINLOCK(name)		__hyp_data struct spinlock name = { 0, 0, #name }

/**
 * spin_lock_init() - reset a lock to the unlocked state
 * @lock: lock to initialise
 *
 * Both tickets go back to 0.  Only for locks that are not statically defined
 * with DEFINE_SPINLOCK(), e.g. one carved out of the hyp_mem pool.
 */
void spin_lock_init(struct spinlock *lock);

/**
 * spin_lock() - claim the lock, waiting our turn
 * @lock: lock to take
 *
 * Takes the next ticket and waits until lock->owner reaches it.  Does not
 * touch DAIF: use spin_lock_irqsave() if the section must not be interrupted.
 * Recursive use deadlocks, exactly as in Linux.
 */
void spin_lock(struct spinlock *lock);

/**
 * spin_trylock() - claim the lock if nobody holds it
 * @lock: lock to take
 * Return: 1 if we hold it now, 0 if a ticket is in office or another CPU
 *	   beat us to the one we wanted
 */
int spin_trylock(struct spinlock *lock);

/**
 * spin_unlock() - release the lock
 * @lock: lock we hold
 *
 * Publishes the next ticket with a store-release, so everything the critical
 * section wrote is visible before the lock is.
 */
void spin_unlock(struct spinlock *lock);

/**
 * spin_is_locked() - is a ticket in office?
 * @lock: lock to look at
 * Return: 1 if somebody is inside, 0 if the lock is free
 *
 * A diagnostic peek, not a synchronisation primitive: the answer is stale the
 * moment it is read.
 */
int spin_is_locked(const struct spinlock *lock);

/**
 * spin_lock_irqsave() - take the lock with local interrupts masked
 * @lock: lock to take
 * @flags: lvalue to save the previous DAIF in (unsigned long)
 *
 * Linux's spelling.  local_irq_save() masks IRQ+FIQ and hands back what was
 * there before, which spin_unlock_irqrestore() puts back after the section.
 */
#define spin_lock_irqsave(lock, flags)						\
do {										\
	local_irq_save(flags);							\
	spin_lock(lock);							\
} while (0)

/**
 * spin_unlock_irqrestore() - release the lock and put the mask back
 * @lock: lock we hold
 * @flags: the value spin_lock_irqsave() saved
 */
#define spin_unlock_irqrestore(lock, flags)					\
do {										\
	spin_unlock(lock);							\
	local_irq_restore(flags);						\
} while (0)

#endif /* __HYP_SPINLOCK_H */
