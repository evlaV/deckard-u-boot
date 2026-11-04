// SPDX-License-Identifier: GPL-2.0+
/*
 * Hypervisor mutex: the Linux mutex API, spinning implementation.
 *
 * Same compat.h caveat as include/hyp/spinlock.h: compat.h's DEFINE_MUTEX,
 * mutex_init, mutex_lock and mutex_unlock are no-ops, so this header #undefs
 * them (include it after compat.h).
 *
 * A mutex here is a lock with an owner: lock() spins until it owns it,
 * trylock() claims it only if free, unlock() releases it.  It is *not* Linux's
 * sleeping mutex - there is no wait queue and no scheduler in this hypervisor,
 * so a blocked caller burns CPU in the same yield loop a spinlock uses.  The
 * difference that matters is the owner field: trylock() on a held mutex always
 * fails, whoever holds it, which makes a reentrant caller visible instead of
 * silently succeeding.
 *
 * Recursive mutex_lock() deadlocks, exactly like Linux's.  There is no
 * might_sleep() warning: the guest runs with DAIF masked from the handoff on,
 * so "would sleeping be legal here?" is not a question this build can answer.
 * See scripts/LOCKING.md.
 */
#ifndef __HYP_MUTEX_H
#define __HYP_MUTEX_H

#include <hypervisor.h>
#include <linux/types.h>

#undef DEFINE_MUTEX
#undef mutex_init
#undef mutex_lock
#undef mutex_trylock
#undef mutex_unlock
#undef mutex_is_locked

struct mutex {
	u32 owner;				/* 0 = free, else CPU affinity + 1 */
};

/*
 * DEFINE_MUTEX() puts the object in .data.hyp, not .bss: hyp code runs at its
 * link-time addresses, where .bss is never zeroed - crt0_64.S clears BSS after
 * relocation, at the relocated copy's address - so a resident lock has to be
 * initialized data.  Same reason hyp/gic.c and hyp/mmio.c keep their state in
 * __hyp_data.
 */
#define DEFINE_MUTEX(name)			__hyp_data struct mutex name = { 0 }

/**
 * mutex_init() - reset a mutex to the unlocked state
 * @mutex: mutex to initialise
 *
 * Only for mutexes that are not statically defined with DEFINE_MUTEX().
 */
void mutex_init(struct mutex *mutex);

/**
 * mutex_lock() - take the mutex, spinning until it is ours
 * @mutex: mutex to take
 *
 * Spins, because there is nothing to sleep in here.  Recursive locking
 * deadlocks, as in Linux.
 */
void mutex_lock(struct mutex *mutex);

/**
 * mutex_trylock() - take the mutex if nobody owns it
 * @mutex: mutex to take
 * Return: 1 if we own it now, 0 if somebody (or we) already did
 */
int mutex_trylock(struct mutex *mutex);

/**
 * mutex_unlock() - release the mutex
 * @mutex: mutex we own
 *
 * Clears the owner with a store-release.  It does not check that the caller
 * owns the mutex - taking a lock you do not hold is a bug we leave to the
 * owner field to make visible after the fact.
 */
void mutex_unlock(struct mutex *mutex);

/**
 * mutex_is_locked() - does anybody own it?
 * @mutex: mutex to look at
 * Return: 1 if owned, 0 if free
 *
 * A diagnostic peek, not a synchronisation primitive.
 */
int mutex_is_locked(const struct mutex *mutex);

#endif /* __HYP_MUTEX_H */
