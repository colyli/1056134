/*
 * Spin and read/write lock operations.
 *
 * Copyright (C) 2001-2004 Paul Mackerras <paulus@au.ibm.com>, IBM
 * Copyright (C) 2001 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2002 Dave Engebretsen <engebret@us.ibm.com>, IBM
 *   Rework to support virtual processors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/stringify.h>
#include <linux/smp.h>

/* waiting for a spinlock... */
#if defined(CONFIG_PPC_SPLPAR)
#include <asm/hvcall.h>
#include <asm/smp.h>

void __spin_yield(arch_spinlock_t *lock)
{
	unsigned int lock_value, holder_cpu, yield_count;

	lock_value = lock->slock;
	if (lock_value == 0)
		return;
	holder_cpu = lock_value & 0xffff;
	BUG_ON(holder_cpu >= NR_CPUS);
	yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);
	if ((yield_count & 1) == 0)
		return;		/* virtual cpu is currently running */
	rmb();
	if (lock->slock != lock_value)
		return;		/* something has changed */
	plpar_hcall_norets(H_CONFER,
		get_hard_smp_processor_id(holder_cpu), yield_count);
}
EXPORT_SYMBOL_GPL(__spin_yield);

void arch_spin_unlock_wait(arch_spinlock_t *lock)
{
	arch_spinlock_t lock_val;

	smp_mb();

	/*
	 * Atomically load and store back the lock value (unchanged). This
	 * ensures that our observation of the lock value is ordered with
	 * respect to other lock operations.
	 */
	__asm__ __volatile__(
"1:	" PPC_LWARX(%0, 0, %2, 0) "\n"
"	stwcx. %0, 0, %2\n"
"	bne- 1b\n"
	: "=&r" (lock_val), "+m" (*lock)
	: "r" (lock)
	: "cr0", "xer");

	if (arch_spin_value_unlocked(lock_val))
		goto out;

	while (lock->slock) {
		HMT_low();
		if (SHARED_PROCESSOR)
			__spin_yield(lock);
	}
	HMT_medium();

out:
	smp_mb();
}
#ifdef CONFIG_PPC64
EXPORT_SYMBOL_GPL(arch_spin_unlock_wait);
#endif

/*
 * Waiting for a read lock or a write lock on a rwlock...
 * This turns out to be the same for read and write locks, since
 * we only know the holder if it is write-locked.
 */
void __rw_yield(arch_rwlock_t *rw)
{
	int lock_value;
	unsigned int holder_cpu, yield_count;

	lock_value = rw->lock;
	if (lock_value >= 0)
		return;		/* no write lock at present */
	holder_cpu = lock_value & 0xffff;
	BUG_ON(holder_cpu >= NR_CPUS);
	yield_count = be32_to_cpu(lppaca_of(holder_cpu).yield_count);
	if ((yield_count & 1) == 0)
		return;		/* virtual cpu is currently running */
	rmb();
	if (rw->lock != lock_value)
		return;		/* something has changed */
	plpar_hcall_norets(H_CONFER,
		get_hard_smp_processor_id(holder_cpu), yield_count);
}
#endif
