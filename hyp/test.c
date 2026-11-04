// SPDX-License-Identifier: GPL-2.0+
/*
 * Hypervisor self-test harness - the plan is scripts/TESTING.md.
 *
 * Built only with CONFIG_HYP_TEST.  hyp_test_run() is called from
 * hv_start_vm() once EL2 setup is complete - stage-2 installed, GIC and the
 * guest's virtual CPU interface up, EL2 stage-1 on - so every API a case
 * exercises is live.  CPU0 never leaves EL2 for the suite: the cases that need
 * a real EL1 context run on a core hyp_smp_start() brings up for them, and
 * report back through hyp_test_hvc().
 *
 * Cases assert on registers, structs, return values and memory - never on
 * console wording - so rewording a printf cannot change a verdict.  Each case
 * prints one line, the run ends with a SUMMARY line, and the counters also
 * live in hyp_test_result, which gdb reads straight off the target (this
 * target cannot `call`).
 *
 * A failing case halts the board: one broken invariant here usually dominoes
 * into the cases after it, and a half-passing run is worse than a stopped one.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYPTEST] "fmt
#define HYP_CONSOLE_TAG "[HYPTEST] "

#include <asm/barriers.h>
#include <asm-generic/sections.h>
#include <asm/io.h>
#include <asm/ptrace.h>
#include <asm/system.h>
#include <hang.h>
#include <hyp/mutex.h>
#include <hyp/spinlock.h>
#include <hypervisor.h>
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/psci.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <log.h>
#include <stdarg.h>
#include <stdbool.h>
#if IS_ENABLED(CONFIG_HYP_USB)
#include "net.h"
#endif

#include "hv.h"

/* crt0_64.S: where the guest resumes after the EL2 -> EL1 handoff */
extern void relocation_return(void);

/*
 * Set once hyp_test_run() has been through the table, so a reader can tell a
 * clean run from a run that never got here.
 */
#define HYPTEST_MAGIC		0x48595031U	/* "HYP1" */

/*
 * Stages t_el1_payload() reports through, in the order it runs them.  The EL2
 * side scores every one; the payload itself decides nothing.
 */
#define HYPTEST_EL1_STAGES	8
#define HYPTEST_EL1_MPIDR	0	/* MRS MPIDR_EL1 as the guest sees it */
#define HYPTEST_EL1_HVC		1	/* an HVC served by EL2 */
#define HYPTEST_EL1_IAR		2	/* its virtual interrupt, acked at EL1 */
#define HYPTEST_EL1_EOIR	3	/* ...and what its EOI leaves behind */
#define HYPTEST_EL1_ADDR	4	/* the address it is about to fault on */
#define HYPTEST_EL1_STORE	5	/* a store into a watched stage-2 block */
#define HYPTEST_EL1_LOAD	6	/* ...and a load back out of it */
#define HYPTEST_EL1_BEATS	7	/* heartbeats while CPU0 reads the row */

struct hyp_test_result {
	u32 magic;		/* HYPTEST_MAGIC once the suite has run */
	int cases;
	int passed;
	int failed;
	int first_failed;	/* index into cases[], -1 while clean */
};

/*
 * One stage of the EL1 test core, as EL2 recorded it.
 *
 * A whole cache line each: the report is written by the core that trapped its
 * own HVC and read by CPU0, and hyp_clean_range() works a line at a time - two
 * reports sharing a line let one core write its stale copy of the other's
 * stage straight over it.
 */
struct hyp_test_el1_report {
	int cpu;		/* which core reported it */
	int done;
	u64 value;		/* what the stage measured at EL1 */
	u64 expect;		/* what the stub says it should have been */
} __aligned(64);

/* gdb reads this block directly: `print hyp_test_result` */
static __hyp_data struct hyp_test_result hyp_test_result;
static __hyp_data struct hyp_test_el1_report el1_report[HYPTEST_EL1_STAGES];

/* The case being run, and how many assertions it has failed so far */
static __hyp_data const char *cur_case;
static __hyp_data int cur_fails;

struct hyp_test_case {
	const char *id;
	int (*run)(void);	/* 0 = clean, non-zero is reported as a failure too */
};

/*
 * One FAIL line.  hprintf() is the serialising print path (hyp/hv.h), so it
 * cannot be interleaved with another core's output.
 */
static void __hyp t_fail(const char *fmt, ...)
{
	char detail[96];
	va_list args;

	va_start(args, fmt);
	vsnprintf(detail, sizeof(detail), fmt, args);
	va_end(args);

	hprintf("FAIL %s: %s\n", cur_case, detail);
	cur_fails++;
}

/*
 * One assertion.  A failing check is recorded and the case keeps going, so a
 * single run reports everything a case found rather than only the first.
 */
#define t_check(cond, ...)						\
	do {								\
		if (!(cond))						\
			t_fail(__VA_ARGS__);				\
	} while (0)

/*
 * A check the case cannot survive: the rest of it would run against state that
 * is no longer there.  Report it and stop the case.
 */
#define t_fail_ret(...) (t_fail(__VA_ARGS__), -1)

/*
 * The .hyp section is the hypervisor's own: relocate_code() deliberately skips
 * it, so every address this file can name must still sit inside the link-time
 * [__hyp_start, __hyp_stop) span even though U-Boot moved out from under it.
 * If this stops holding, the resident hypervisor is gone and every trap
 * handler is running the guest's copy.
 */
static int __hyp t_hyp_resident(void)
{
	u64 start = (u64)__hyp_start, stop = (u64)__hyp_stop;
	u64 data = (u64)&hyp_test_result;
	u64 code = (u64)t_hyp_resident;

	t_check(start < stop, ".hyp span is empty: [%#llx, %#llx)", start, stop);
	t_check(data >= start && data < stop,
		"hyp data %#llx outside .hyp [%#llx, %#llx)", data, start, stop);
	t_check(code >= start && code < stop,
		"hyp code %#llx outside .hyp [%#llx, %#llx)", code, start, stop);
	return 0;
}

/*
 * The handoff has to land inside the copy the guest actually runs: the offset
 * hv_start_vm() stashed is the one U-Boot really moved by, and
 * relocation_return + that offset is inside the relocated image.  A handoff
 * outside it means the guest is eret'ing into memory nobody copied.
 */
static int __hyp t_handoff_offset(void)
{
	u64 off = hyp_reloc_off();
	u64 resume = off + (u64)relocation_return;
	u64 lo = off + (u64)__image_copy_start;
	u64 hi = off + (u64)__image_copy_end;

	t_check(off != 0, "reloc offset is 0: U-Boot never relocated");
	t_check(resume >= lo && resume < hi,
		"handoff %#llx outside relocated image [%#llx, %#llx)",
		resume, lo, hi);
	return 0;
}

/*
 * HCR_EL2 is the configuration every guest core runs under: hv_start_vm()
 * starts from what XBL/Gunyah left and keeps only the traps it actually
 * serves, and hyp/smp.c replays the finished value onto each core it wakes.
 * A stray bit is not subtle - TGE set makes the eret into EL1 invalid, TVM or
 * TTLB set stops U-Boot setting up its own MMU, TSC set stops PSCI CPU_ON
 * reaching EL3, IMO clear sends the UART's interrupt to the guest instead of
 * here.  Asserting the whole field set is what keeps a future edit to
 * hv_start_vm() from quietly changing the guest's world.
 */
static int __hyp t_hcr_guest_config(void)
{
	u64 hcr = hyp_hcr_value();

	t_check(hcr == mrs(hcr_el2),
		"HCR_EL2 stashed for the cores (%#llx) is not what this one runs (%#llx)",
		hcr, mrs(hcr_el2));

	/* stage-2 on, and its walks are allowed to read memory */
	t_check(FIELD_GET(HCR_VM, hcr) == 1, "HCR_EL2.VM clear: no stage-2");
	t_check(FIELD_GET(HCR_PTW, hcr) == 1, "HCR_EL2.PTW clear: stage-2 walks trap");

	/* physical interrupts/SError land at EL2; virtual ones come from the
	 * vGIC's list registers, not from HCR flipping a line */
	t_check(FIELD_GET(HCR_FMO | HCR_IMO | HCR_AMO, hcr) == 0x7,
		"physical FIQ/IRQ/SError not taken at EL2: %#llx", hcr);
	t_check(FIELD_GET(HCR_VF | HCR_VI | HCR_VSE, hcr) == 0,
		"virtual interrupt injected by HCR rather than the vGIC: %#llx", hcr);

	/* barriers and TLBI at Inner Shareable: this is an 8-core cluster */
	t_check(FIELD_GET(HCR_BSU_MASK, hcr) == 1,
		"HCR_EL2.BSU is not Inner Shareable: %#llx", FIELD_GET(HCR_BSU_MASK, hcr));

	/* the guest runs cache maintenance, barriers, ID reads and WFE natively;
	 * trapping them here is how a hypervisor makes a guest slow.  WFI/WFIT
	 * trap on purpose: handle_wfx() polls for the wakeup, because a PE that
	 * sleeps in a real WFI is not woken by the interrupt routed to EL2. */
	t_check((hcr & (HCR_FB | HCR_DC | HCR_TWE)) == 0,
		"guest cacheability/WFE trapped: %#llx", hcr);
	t_check(FIELD_GET(HCR_TWI, hcr) == 1, "HCR_EL2.TWI clear: the guest sleeps in a real WFI");
	t_check(FIELD_GET(HCR_TID_MASK, hcr) == 0, "guest ID register reads trapped");
	t_check(FIELD_GET(HCR_TSC, hcr) == 1,
		"HCR_EL2.TSC clear: a guest PSCI CPU_ON would start a core at EL1 with no EL2 state");
	t_check(FIELD_GET(HCR_TACR | HCR_TSW | HCR_TPCP | HCR_TPU, hcr) == 0,
		"guest ACTLR/cache/barrier accesses trapped: %#llx", hcr);
	t_check((hcr & (HCR_TTLB | HCR_TVM | HCR_TDZ | HCR_TRVM)) == 0,
		"guest TLBI/MMU-system-register accesses trapped: %#llx", hcr);
	t_check(FIELD_GET(HCR_CD | HCR_ID, hcr) == 0,
		"guest debug or implementation-defined sysregs trapped: %#llx", hcr);
	/* Inherited Gunyah leftovers that trap things we do not serve:
	 * fine-grained traps, nested-virtualization mode and bit 1 (SWIO in
	 * ./scripts/isa.py's layout).  APK and API are the opposite polarity:
	 * they are 1 for "do not trap" PAuth key accesses and PAC instructions
	 * (a clear bit sends them through the EC=0x18/EC=0x9 paths as zeroes). */
	t_check((hcr & (HCR_SWIO | HCR_FGT | HCR_NV)) == 0,
		"Gunyah trap/mode leftovers still set: %#llx", hcr);
	t_check((hcr & (HCR_APK | HCR_API)) == (HCR_APK | HCR_API),
		"guest PAuth key/instruction use trapped (APK/API clear): %#llx", hcr);

	/* the one trap kept on purpose: unallocated group-3 sysreg accesses come
	 * here instead of being undefined at EL1 (the GIC CPU interface itself is
	 * served by the hardware virtual interface, not by this) */
	t_check(FIELD_GET(HCR_TIDCP, hcr) == 1,
		"HCR_EL2.TIDCP clear: unallocated group-3 accesses go undefined");

	/* the two bits that decide whether the handoff works at all */
	t_check(FIELD_GET(HCR_TGE, hcr) == 0,
		"HCR_EL2.TGE set: EL2 is not in VM mode, an eret to EL1 is invalid");
	t_check(FIELD_GET(HCR_RW, hcr) == 1, "HCR_EL2.RW clear: guest would take AArch32");

	return 0;
}

/*
 * Locking cases.  They run at EL2 on CPU0 before the guest exists: the
 * primitives are hyp code and their state lives in .data.hyp, so the only
 * thing a case can corrupt is itself.  DAIF here is the *current-EL* mask -
 * at this point EL2's own bank, the same one local_irq_save() and
 * local_irq_restore() write.
 */
#define DAIF_D		BIT(9)
#define DAIF_A		BIT(8)
#define DAIF_I		BIT(7)
#define DAIF_F		BIT(6)
#define DAIF_IRQFIQ	(DAIF_I | DAIF_F)

static DEFINE_SPINLOCK(test_spin);
static DEFINE_MUTEX(test_mutex);

/*
 * MSR DAIF then MRS DAIF in one instruction stream can read the old value, so
 * every read here is preceded by an isb - same reason as cmd/hyplock.c's
 * read_daif().
 */
static __hyp u64 read_daif(void)
{
	isb();

	return (u64)mrs(daif);
}

/*
 * The ticket spinlock: trylock claims a free lock and refuses a held one,
 * unlock hands the ticket on, and the owner/next pair says which of those
 * states the lock is really in.  A lock that reports itself held while it is
 * free, or a trylock that hands out a ticket it then refuses, shows up as a
 * pair that does not match.
 */
static int __hyp t_lock_spin_trylock(void)
{
	struct spinlock *l = &test_spin;
	u32 ticket;

	spin_lock_init(l);
	t_check(l->owner == 0 && l->next == 0, "init left owner=%u next=%u",
		l->owner, l->next);
	t_check(!spin_is_locked(l), "freshly initialised lock reports held");

	t_check(spin_trylock(l) == 1, "spin_trylock() failed on a free lock");
	t_check(spin_is_locked(l) == 1, "held lock reports free");
	t_check(l->owner == 0 && l->next == 1, "held pair is owner=%u next=%u",
		l->owner, l->next);

	t_check(spin_trylock(l) == 0, "spin_trylock() took a lock already held");
	t_check(l->next == 1, "refused trylock handed out a ticket: next=%u",
		l->next);

	spin_unlock(l);
	t_check(!spin_is_locked(l), "still held after unlock: owner=%u next=%u",
		l->owner, l->next);
	t_check(spin_trylock(l) == 1, "spin_trylock() failed after an unlock");
	spin_unlock(l);

	/*
	 * spin_lock() takes its ticket and waits for owner to reach it;
	 * uncontended it is in office the moment the number is handed out.
	 */
	ticket = l->next;
	spin_lock(l);
	t_check(spin_is_locked(l) == 1, "spin_lock() left the lock free");
	t_check(l->owner == ticket && l->next == ticket + 1,
		"spin_lock() pair is %u/%u, wanted %u/%u", l->owner, l->next,
		ticket, ticket + 1);
	spin_unlock(l);
	t_check(l->owner == l->next, "unlock did not hand the ticket on: %u/%u",
		l->owner, l->next);

	return 0;
}

/*
 * spin_lock_irqsave() / spin_unlock_irqrestore(): the pair records the
 * caller's own DAIF, masks IRQ+FIQ for the section, leaves D and A alone, and
 * puts back exactly what it saved.  The case unmasks first because EL2 is
 * reached with DAIF masked, and putting that inherited value back is the
 * teardown: a case that touches machine state leaves it as it found it.
 */
static int __hyp t_lock_irqsave_daif(void)
{
	struct spinlock *l = &test_spin;
	unsigned long flags;
	u64 inherited, unmasked, inside, after, pmr;

	spin_lock_init(l);

	/*
	 * A live peripheral line would be taken the instant this case unmasks
	 * IRQs, and exception entry re-masks DAIF - the measurement would then
	 * be about the device, not about the lock.  Gate EL2's physical
	 * interface with its priority mask for the duration and put it back.
	 */
	pmr = (u64)mrs(icc_pmr_el1);
	msr(icc_pmr_el1, 0);

	inherited = read_daif();
	local_irq_enable();
	unmasked = read_daif();
	t_check((unmasked & DAIF_IRQFIQ) == 0,
		"local_irq_enable() left I/F set: daif=%#llx", unmasked);
	t_check((unmasked & (DAIF_D | DAIF_A)) == (inherited & (DAIF_D | DAIF_A)),
		"local_irq_enable() touched D/A: %#llx -> %#llx", inherited,
		unmasked);

	spin_lock_irqsave(l, flags);
	inside = read_daif();
	t_check(flags == unmasked, "saved %#lx is not the DAIF on entry %#llx",
		flags, unmasked);
	t_check((inside & DAIF_IRQFIQ) == DAIF_IRQFIQ,
		"IRQ/FIQ not masked inside the section: daif=%#llx", inside);
	t_check((inside & (DAIF_D | DAIF_A)) == (unmasked & (DAIF_D | DAIF_A)),
		"the section changed D/A: %#llx -> %#llx", unmasked, inside);
	t_check(inside != unmasked, "the mask never changed: %#llx", inside);
	t_check(spin_is_locked(l) == 1, "the lock is not held inside the section");
	t_check(spin_trylock(l) == 0, "spin_trylock() took it from inside");
	t_check((read_daif() & DAIF_IRQFIQ) == DAIF_IRQFIQ,
		"the refused trylock unmasked IRQ/FIQ");

	spin_unlock_irqrestore(l, flags);
	after = read_daif();
	t_check(after == unmasked, "restore gave %#llx, not the saved %#lx",
		after, flags);
	t_check(!spin_is_locked(l), "lock still held after the restore");

	msr(icc_pmr_el1, pmr);
	t_check((u64)mrs(icc_pmr_el1) == pmr, "teardown left pmr=%#lx, wanted %#llx",
		(unsigned long)mrs(icc_pmr_el1), pmr);
	msr(daif, inherited);
	after = read_daif();
	t_check(after == inherited, "teardown left daif=%#llx, wanted %#llx",
		after, inherited);

	return 0;
}

/*
 * The mutex carries its owner: vCPU slot + 1, so 0 is free.  trylock() on a
 * held mutex always fails - whoever holds it, including this same core - which
 * is what makes a reentrant caller visible instead of silently succeeding.
 */
static int __hyp t_lock_mutex_owner(void)
{
	struct mutex *m = &test_mutex;
	u32 me = (u32)hyp_cpu_num() + 1;

	mutex_init(m);
	t_check(m->owner == 0 && !mutex_is_locked(m), "init left owner=%u",
		m->owner);

	t_check(mutex_trylock(m) == 1, "mutex_trylock() failed on a free mutex");
	t_check(m->owner == me, "owner %#x is not this core's slot+1 %#x",
		m->owner, me);
	t_check(mutex_is_locked(m) == 1, "held mutex reports free");
	t_check(mutex_trylock(m) == 0, "mutex_trylock() took a held mutex");
	t_check(m->owner == me, "refused trylock rewrote owner to %#x", m->owner);

	mutex_unlock(m);
	t_check(m->owner == 0 && !mutex_is_locked(m), "unlock left owner=%u",
		m->owner);

	mutex_lock(m);
	t_check(m->owner == me, "mutex_lock() owner %#x is not %#x", m->owner,
		me);
	mutex_unlock(m);
	t_check(m->owner == 0, "second unlock left owner=%u", m->owner);

	return 0;
}

/*
 * t_drain_lrs() - hand every list register of *this* core back
 *
 * gic_irq_ack() mirrors a physical interrupt it caught into a list register and
 * disarms the line, and the cases here arm that same line again on purpose.  A
 * list register left over from an earlier catch would then answer for a trigger
 * this case never caused, so every case that waits for one starts by clearing
 * the interface.  A hardware-backed register needs its physical interrupt
 * deactivated too (ICC_DIR), or the distributor keeps it active.
 */
static void __hyp t_drain_lrs(int pintid)
{
	int n;

	for (n = 0; n < hyp_gic_nr_lrs(); n++) {
		u64 lr = hyp_gic_read_lr(n);

		if (!(lr & LR_STATE_MASK))
			continue;
		if ((lr & LR_HW) && pintid >= 0) {
			asm volatile("msr	icc_dir_el1, %0" :: "r"((u64)pintid));
			asm volatile("isb");
		}
		hyp_gic_write_lr(n, 0);
	}
	hyp_gic_sync();
}

/*
 * An interrupt that lands in ordinary EL2 code has to come back to it.  The
 * suite runs at EL2 with DAIF.I clear - exactly the state hyp_enter_guest()
 * leaves EL2 in - so this is not hypothetical.  Make the interrupt happen on
 * purpose: re-arm the one interrupt source this board really has (the console
 * UART, which gic_irq_ack() disarms after every catch), unmask, and drive that
 * UART with output.  If elx_irq_handler() cannot return into our own code,
 * none of the checks below ever run.
 */
#define HYPTEST_IRQ_SPINS	200000

static int __hyp t_irq_el2_return(void)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(false);
	volatile u32 marker = 0x5aa5a5a5;
	u64 before, spsr, elr;
	u32 entries;
	int i, lr_seen = -1, nlrs = hyp_gic_nr_lrs();
	int irq = hyp_gic_console_irq();

	before = read_daif();
	t_drain_lrs(irq);			/* nothing may answer for this case */
	entries = pcpu->irq_entries;

	hyp_gic_arm_console_irq();	/* the source this case is about to fire */
	local_irq_disable();		/* this case owns when its IRQ is taken */
	local_irq_enable();		/* the interrupt lands somewhere below */
	hprintf("console output is the trigger\n");

	for (i = 0; i < HYPTEST_IRQ_SPINS && lr_seen < 0; i++) {
		int n;

		for (n = 0; n < nlrs; n++) {
			u64 lr = hyp_gic_read_lr(n);

			if ((lr & LR_STATE_MASK) && LR_GET_VINTID(lr) == irq) {
				lr_seen = n;
				break;
			}
		}
	}

	t_check(lr_seen >= 0, "irq %d never reached a list register in %d spins",
		irq, i);

	/*
	 * What the entry interrupted.  The frame the entry built is popped by
	 * elx_return() and its stack slot is reused immediately - reading
	 * pcpu->vcpu.regs afterwards reads whatever the console buffer put
	 * there next - so the durable record is the one the handler left in
	 * this core's context.  SPSR naming EL2h and an ELR inside the resident
	 * hypervisor is the whole claim of this case: the interrupt hit our own
	 * EL2 C code and the handler went back into it.
	 */
	t_check(pcpu->irq_entries > entries, "no IRQ entry was recorded (%u -> %u)",
		entries, pcpu->irq_entries);
	spsr = pcpu->irq_entry_spsr;
	elr = pcpu->irq_entry_elr;
	t_check((spsr & PSR_MODE_MASK) == PSR_MODE_EL2h,
		"irq entry SPSR %#llx does not name EL2h", spsr);
	t_check(elr >= (u64)__hyp_start && elr < (u64)__hyp_stop,
		"irq entry ELR %#llx is not hypervisor code", elr);

	t_check(marker == 0x5aa5a5a5, "interrupted C frame was clobbered: %#x",
		marker);

	/*
	 * Hand the list register back and deactivate the physical interrupt,
	 * then put the masking back as this case found it.
	 */
	if (lr_seen >= 0) {
		hyp_gic_write_lr(lr_seen, 0);
		asm volatile("msr	icc_dir_el1, %0" :: "r"((u64)irq));
		asm volatile("isb");
		hyp_gic_sync();
	}
	msr(daif, before);

	return 0;
}

/*
 * The distributor the hypervisor runs on is the one bring-up took from the DT,
 * it implements SPIs, and the interrupt it armed for the console is one of
 * them.  These are the values every other user of the GIC has to be able to
 * rely on; hyp/gic.c is what reads the DT.
 */
static int __hyp t_gic_dist_geometry(void)
{
	u64 dist = hyp_gic_dist_base();
	int max_intid = hyp_gic_nr_intids();
	int irq = hyp_gic_console_irq();

	t_check(dist, "hyp_gic_dist_base() reports no distributor");
	t_check((dist & 0xfff) == 0, "distributor base %#llx is not page aligned", dist);
	t_check(max_intid >= 32, "distributor implements no SPIs (highest INTID %d)",
		max_intid);
	t_check(irq >= 32 && irq <= max_intid,
		"console irq %d is outside the implemented SPI range [32, %d]", irq,
		max_intid);

	return 0;
}

/*
 * Every DT core either owns its own redistributor bank inside the region
 * bring-up described, on a stride boundary, or has none at all - and the bank
 * bring-up found for this core by reading MPIDR_EL1 is the one the lookup finds
 * for the affinity the DT gives this core.  A mismatch there is the DT and the
 * hardware disagreeing about who owns what.
 */
static int __hyp t_gic_rdist_affinity(void)
{
	u64 self = hyp_gic_rdist_self(), span = 0, stride = 0;
	int i, banks = 0;

	hyp_gic_rdist_region(&span, &stride);
	t_check(self && span && stride,
		"no redistributor region (own bank %#llx span %#llx stride %#llx)", self,
		span, stride);
	if (!self || !span || !stride)
		return 0;

	for (i = 0; i < hyp_cpu_count(); i++) {
		u64 mpidr = hyp_cpu_mpidr(i);
		u64 bank = hyp_gic_rdist_of(mpidr);
		u64 off;

		if (!bank)
			continue;
		banks++;
		off = bank - self;
		t_check(off < span,
			"cpu%d bank %#llx is %#llx from this core's bank, past the %#llx region",
			i, bank, off, span);
		t_check(off % stride == 0,
			"cpu%d bank %#llx sits %#llx from this core's bank, not on stride %#llx",
			i, bank, off, stride);
	}

	t_check(banks >= 1, "no DT core has a redistributor bank");
	t_check(hyp_gic_rdist_self(), "bring-up found no redistributor for this core");
	t_check(hyp_gic_rdist_of(hyp_cpu_mpidr(0)) == hyp_gic_rdist_self(),
		"the DT's cpu0 affinity finds bank %#llx, bring-up found %#llx",
		hyp_gic_rdist_of(hyp_cpu_mpidr(0)), hyp_gic_rdist_self());

	return 0;
}

/*
 * The physical CPU interface is reachable from EL2 through the system-register
 * bank - which is what every ack in hyp/gic.c assumes.  With the physical
 * priority mask at 0 nothing can sit above it, so ICC_IAR1_EL1 answers with a
 * spurious INTID instead of faulting: the interface is really there.
 */
static int __hyp t_gic_cpuif_sre(void)
{
	u64 sre = mrs(icc_sre_el2);
	u64 pmr = mrs(icc_pmr_el1);
	u64 iar;

	t_check(sre & ICC_SRE_SRE, "ICC_SRE_EL2.SRE clear (%#llx): no sysreg interface",
		sre);
	t_check(sre & ICC_SRE_ENEL1, "ICC_SRE_EL2.ENABLE clear (%#llx): EL1 gets none",
		sre);

	msr(icc_pmr_el1, 0);
	asm volatile("isb");
	iar = mrs(icc_iar1_el1) & ICC_IAR_INTID;
	msr(icc_pmr_el1, pmr);
	asm volatile("isb");

	t_check(iar >= ICC_IAR_SPURIOUS,
		"ICC_IAR1_EL1 handed back INTID %llu with nothing above PMR", iar);
	t_check(mrs(icc_pmr_el1) == pmr, "ICC_PMR_EL1 %#llx was not restored",
		(u64)mrs(icc_pmr_el1));

	return 0;
}

/*
 * The guest's virtual interface is on, sized the way the GIC implements it, and
 * running the group the guest's IRQs live in.
 */
static int __hyp t_gic_vgic_cr0(void)
{
	u64 vtr = mrs(ich_vtr_el2);
	u64 hcr = mrs(ich_hcr_el2);
	u64 vmcr = mrs(ich_vmcr_el2);
	int impl = (int)FIELD_GET(ICH_VTR_LISTREGS, vtr) + 1;
	int used = hyp_gic_nr_lrs();

	t_check(hcr & ICH_HCR_EN, "ICH_HCR_EL2.En clear (%#llx): the guest has no vGIC",
		hcr);
	t_check(impl >= 1, "ICH_VTR_EL2 %#llx reports no list registers", vtr);
	t_check(used >= 1 && used <= impl,
		"hyp_gic_nr_lrs() %d is outside the %d the GIC implements", used, impl);
	t_check((int)FIELD_GET(ICH_VTR_PRIBITS, vtr) + 1 >= 2,
		"ICH_VTR_EL2 %#llx implements fewer than 2 priority levels", vtr);
	t_check(vmcr & ICH_VMCR_VENG1,
		"ICH_VMCR_EL2.VENG1 clear (%#llx): virtual Group 1 is off", vmcr);

	return 0;
}

/*
 * A list register written by the hypervisor reads back field for field: state,
 * the physical-backing flag, group, priority and the virtual INTID the guest
 * will acknowledge.  The physical interface is quieted first so a live line
 * cannot land in the slot mid-measurement.
 */
static int __hyp t_vgic_lr_fields(void)
{
	u64 pmr = mrs(icc_pmr_el1);
	u64 want = LR_PENDING | LR_GRP1 | LR_PRIORITY(0x40) | LR_VINTID(0x1234);
	u64 lr;

	msr(icc_pmr_el1, 0);
	asm volatile("isb");

	hyp_gic_write_lr(0, want);
	lr = hyp_gic_read_lr(0);

	t_check(lr == want, "ICH_LR0_EL2 reads %#llx, not the %#llx written", lr, want);
	t_check((lr & LR_STATE_MASK) == LR_PENDING, "ICH_LR0_EL2 is not Pending (%#llx)",
		lr);
	t_check(!(lr & LR_HW), "ICH_LR0_EL2 claims a physical interrupt (%#llx)", lr);
	t_check(lr & LR_GRP1, "ICH_LR0_EL2 is not Group 1 (%#llx)", lr);
	t_check(LR_GET_PRIORITY(lr) == 0x40, "ICH_LR0_EL2 priority is %llu, not 0x40",
		LR_GET_PRIORITY(lr));
	t_check(LR_GET_VINTID(lr) == 0x1234, "ICH_LR0_EL2 vINTID is %d, not 0x1234",
		LR_GET_VINTID(lr));

	hyp_gic_write_lr(0, 0);
	hyp_gic_sync();
	t_check(hyp_gic_read_lr(0) == 0, "ICH_LR0_EL2 did not clear (%#llx)",
		hyp_gic_read_lr(0));

	msr(icc_pmr_el1, pmr);
	asm volatile("isb");
	t_check(mrs(icc_pmr_el1) == pmr, "ICC_PMR_EL1 %#llx was not restored",
		(u64)mrs(icc_pmr_el1));

	return 0;
}

/*
 * What the guest's ICC_EOIR1_EL1 leaves behind - State 0b00 - has to be seen by
 * hyp_gic_sync() and hand the slot back: the same list register then takes a
 * new interrupt.  A list register that never retires is a vGIC that runs out of
 * interrupts after nlr of them.
 */
static int __hyp t_vgic_lr_retire(void)
{
	u64 pmr = mrs(icc_pmr_el1);
	u64 lr;

	msr(icc_pmr_el1, 0);
	asm volatile("isb");

	hyp_gic_write_lr(0, LR_PENDING | LR_GRP1 | LR_PRIORITY(HYP_IRQ_PRIO) |
			 LR_VINTID(0x77));
	lr = hyp_gic_read_lr(0);
	t_check((lr & LR_STATE_MASK) == LR_PENDING,
		"ICH_LR0_EL2 is not pending before the retire (%#llx)", lr);

	/* End-of-interrupt the way the guest does it: drop State, keep the rest. */
	hyp_gic_write_lr(0, lr & ~LR_STATE_MASK);
	hyp_gic_sync();

	lr = hyp_gic_read_lr(0);
	t_check((lr & LR_STATE_MASK) == 0,
		"ICH_LR0_EL2 is still active after hyp_gic_sync() (%#llx)", lr);

	hyp_gic_write_lr(0, LR_ACTIVE | LR_GRP1 | LR_VINTID(0x78));
	lr = hyp_gic_read_lr(0);
	t_check((lr & LR_STATE_MASK) == LR_ACTIVE,
		"ICH_LR0_EL2 did not take a new interrupt (%#llx)", lr);
	t_check(LR_GET_VINTID(lr) == 0x78, "ICH_LR0_EL2 vINTID is %d, not 0x78",
		LR_GET_VINTID(lr));

	hyp_gic_write_lr(0, 0);
	hyp_gic_sync();

	msr(icc_pmr_el1, pmr);
	asm volatile("isb");
	t_check(mrs(icc_pmr_el1) == pmr, "ICC_PMR_EL1 %#llx was not restored",
		(u64)mrs(icc_pmr_el1));

	return 0;
}

/*
 * A real device interrupt, served at EL2 and mirrored into the guest's view of
 * its interrupts: the list register has to name the physical interrupt it came
 * from, in the group the guest takes as an IRQ, at the priority we arm device
 * interrupts at.  (irq.el2-return covers the other half: getting back into the
 * code the interrupt hit.)
 */
static int __hyp t_gic_irq_to_el2(void)
{
	int irq = hyp_gic_console_irq(), nlrs = hyp_gic_nr_lrs();
	int i, seen = -1;
	u64 before, lr;

	t_check(irq >= 0, "the DT named no console interrupt");
	if (irq < 0)
		return 0;

	before = read_daif();
	hyp_gic_arm_console_irq();	/* every catch disarms what it acked */
	local_irq_disable();
	local_irq_enable();
	hprintf("console output is the trigger\n");

	for (i = 0; i < HYPTEST_IRQ_SPINS && seen < 0; i++) {
		int n;

		for (n = 0; n < nlrs; n++) {
			u64 v = hyp_gic_read_lr(n);

			if ((v & LR_STATE_MASK) && LR_GET_VINTID(v) == irq) {
				seen = n;
				break;
			}
		}
	}

	t_check(seen >= 0, "console irq %d never reached a list register in %d spins",
		irq, i);
	if (seen < 0) {
		msr(daif, before);
		return 0;
	}

	lr = hyp_gic_read_lr(seen);
	t_check(lr & LR_HW, "ICH_LR%d_EL2 has no physical backing (%#llx)", seen, lr);
	t_check(lr & LR_GRP1, "ICH_LR%d_EL2 is not Group 1 (%#llx)", seen, lr);
	t_check(LR_GET_PRIORITY(lr) == HYP_IRQ_PRIO,
		"ICH_LR%d_EL2 priority is %llu, not the armed %d", seen,
		LR_GET_PRIORITY(lr), HYP_IRQ_PRIO);
	t_check(LR_GET_PINTID(lr) == irq,
		"ICH_LR%d_EL2 pINTID is %llu, not the console irq %d", seen,
		LR_GET_PINTID(lr), irq);
	t_check(LR_GET_VINTID(lr) == irq,
		"ICH_LR%d_EL2 vINTID %d does not mirror its pINTID %d", seen,
		LR_GET_VINTID(lr), irq);

	hyp_gic_write_lr(seen, 0);
	asm volatile("msr	icc_dir_el1, %0" :: "r"((u64)irq));
	asm volatile("isb");
	hyp_gic_sync();
	msr(daif, before);

	return 0;
}

/*
 * An interrupt EL2 owns (hyp_gic_own_irq(), what the USB controller will be) is
 * consumed at EL2: the handler runs, and nothing is injected for the guest.
 * The console interrupt is the trigger again, as in gic.irq-to-el2.
 */
static __hyp_data volatile u32 t_owned_hits = 0;

static void __hyp t_owned_handler(int intid)
{
	t_owned_hits++;
	hyp_gic_disarm_irq(intid);	/* the level line stays asserted until the guest drains it */
}

static int __hyp t_gic_irq_owned(void)
{
	int irq = hyp_gic_console_irq(), nlrs = hyp_gic_nr_lrs();
	int i, n, rc;
	u64 before;

	t_check(irq >= 0, "the DT named no console interrupt");
	if (irq < 0)
		return 0;

	t_drain_lrs(-1);
	t_owned_hits = 0;
	before = read_daif();

	rc = hyp_gic_own_irq(irq, true, t_owned_handler);
	t_check(rc == 0, "hyp_gic_own_irq(%d) returned %d", irq, rc);
	if (rc) {
		msr(daif, before);
		return 0;
	}

	local_irq_disable();
	local_irq_enable();
	hprintf("console output is the trigger\n");

	for (i = 0; i < HYPTEST_IRQ_SPINS && !t_owned_hits; i++)
		;

	/* a started core printing at the same moment raises the console interrupt again */
	t_check(t_owned_hits >= 1, "the owned handler never ran in %d spins", i);

	for (n = 0; n < nlrs; n++) {
		u64 v = hyp_gic_read_lr(n);

		t_check(!((v & LR_STATE_MASK) && LR_GET_VINTID(v) == irq),
			"ICH_LR%d_EL2 holds the owned irq %d for the guest (%#llx)",
			n, irq, v);
	}

	hyp_gic_disown_irq(irq);
	msr(daif, before);

	return 0;
}

/*
 * MMIO spy cases: the stage-2 carve, and replaying an access with no guest.
 *
 * The spy is the stage-2 map itself.  hyp_mmio_watch() takes a range's 2 MiB
 * block out of the map, every access to it then faults into EL2, and
 * hyp_s2_emulate() replays the access the guest was trying to make - out of the
 * frame EL2 parked for it plus FAR_EL2 - and writes it through to the real
 * hardware.  CPU0 has no guest and no frame during the suite, so a case supplies
 * both: scratch inside the hypervisor's own memory, and a synthesized pt_regs
 * whose ELR names a real instruction word.  Everything a genuine fault runs
 * through - the decode of direction/size/Rt out of the trapping instruction, the
 * watch hook, the HCR_EL2.VM toggle around the physical access, the value landing
 * in the guest's Rt, ELR stepping over the trapped instruction - is driven and
 * asserted here without a guest.
 */

/*
 * Scratch: a 2 MiB-aligned chunk of the DT's hyp_mem reservation, so exactly one
 * stage-2 block covers it.  hyp_mem_alloc() hands out 2 MiB and ALIGN()ing the
 * result up cannot leave that chunk (ALIGN(x, a) < x + a), so every byte the
 * suite touches here is inside memory the allocator just returned.
 */
#define HYPTEST_WATCH_RECS	4
/*
 * An SGI number: ICC_SGI1R_EL1 carries the interrupt id in a 4-bit field, so
 * these are the only ids an emulated SGI can name.  No device on this board
 * raises one, so nothing real can answer for these cases.
 */
#define HYPTEST_SGI_VINTID	0xb

static __hyp_data u64 t_scratch;
static __hyp_data u32 t_watch_calls __aligned(64);

/* One cache line per record: the hook runs on the core that took the access and
 * CPU0 is the one that reads it back.
 */
struct t_watch_rec {
	u32 cpu;
	u32 store;
	u32 size;
	u64 far;
	u64 val;
} __aligned(64);

static __hyp_data struct t_watch_rec t_watch_recs[HYPTEST_WATCH_RECS] __aligned(64);

static int __hyp t_mmio_prepare(void)
{
	void *p;

	if (t_scratch)
		return 0;

	p = hyp_mem_alloc(2 * SZ_2M);
	if (!p)
		return -ENOMEM;
	t_scratch = ALIGN((u64)(unsigned long)p, SZ_2M);

	return 0;
}

/* Records only.  A case has to be able to tell the device's value from a hook's
 * override, so this hook never writes through *val.
 */
static void __hyp t_watch_hook(int cpu, u64 far, bool store, u32 size, u64 *val,
			       bool from_guest)
{
	u32 i = t_watch_calls;

	if (i < HYPTEST_WATCH_RECS) {
		t_watch_recs[i].cpu = (u32)cpu;
		t_watch_recs[i].store = store ? 1u : 0u;
		t_watch_recs[i].size = size;
		t_watch_recs[i].far = far;
		t_watch_recs[i].val = *val;
		hyp_clean_range(&t_watch_recs[i], sizeof(t_watch_recs[i]));
	}

	t_watch_calls = i + 1;
	hyp_clean_range(&t_watch_calls, sizeof(t_watch_calls));
}

static void __hyp t_watch_reset(void)
{
	u32 i, j;
	u64 *w;

	t_watch_calls = 0;
	hyp_clean_range(&t_watch_calls, sizeof(t_watch_calls));

	for (i = 0; i < HYPTEST_WATCH_RECS; i++) {
		w = (u64 *)&t_watch_recs[i];
		for (j = 0; j < sizeof(t_watch_recs[i]) / sizeof(u64); j++)
			w[j] = 0;
		hyp_clean_range(&t_watch_recs[i], sizeof(t_watch_recs[i]));
	}
}

/* Record `i` as memory holds it, not as this core last saw it */
static struct t_watch_rec __hyp t_watch_read(int i)
{
	hyp_clean_range(&t_watch_recs[i], sizeof(t_watch_recs[i]));

	return t_watch_recs[i];
}

/*
 * The frame hyp_s2_emulate() replays from: an ELR naming a real instruction
 * word, an ESR saying "data abort with a valid syndrome", and x5 as the register
 * that instruction uses.  hyp_s2_emulate() takes direction, size and Rt from the
 * instruction itself (this core's data-abort ISS is not the architectural
 * layout - hyp/mmio.c), so the instruction is as much under test as the frame.
 */
static __hyp_data struct pt_regs t_regs;
static __hyp_data u32 t_insn;

#define T_INSN_STR_X	0xf9000005u	/* str x5, [x0] */
#define T_INSN_LDR_X	0xf9400005u	/* ldr x5, [x0] */

static int __hyp t_replay(struct hyp_percpu *pcpu, u32 insn, u64 far,
			 u64 store_val, u64 *out)
{
	struct pt_regs *save = pcpu->vcpu.regs;
	int rc;

	t_insn = insn;
	t_regs.elr = (u64)&t_insn;
	t_regs.esr = (0x24UL << 26) | BIT(24);	/* EC=0x24 data abort, ISV=1 */
	t_regs.regs[5] = store_val;
	msr(far_el2, far);
	asm volatile("isb");

	pcpu->vcpu.regs = &t_regs;
	rc = hyp_s2_emulate(pcpu);
	pcpu->vcpu.regs = save;

	if (out)
		*out = t_regs.regs[5];

	return rc;
}

/*
 * Watching a range takes its stage-2 block out of the map; unwatching puts it
 * back with its attributes intact.  Asserted by reading the descriptor out of
 * the tables, never by touching the region: while the block is out, an EL2
 * access to it faults here too.
 */
static int __hyp t_mmio_watch_carve(void)
{
	u64 before = 0, after = 0;
	int i, n = hyp_cpu_count();

	if (t_mmio_prepare())
		return -ENOMEM;

	/* Test-owned memory must not share a block with a core's EL2 stack: a
	 * fault on a watched stack is not a case any run recovers from.
	 */
	for (i = 0; i < n; i++) {
		u64 stack = hyp_mem_stack(i);

		t_check(stack <= t_scratch || stack - SZ_8K >= t_scratch + SZ_2M,
			"cpu%d stack top %#llx shares the scratch block [%#llx, %#llx)",
			i, stack, t_scratch, t_scratch + SZ_2M);
	}

	if (hyp_s2_block(t_scratch, &before))
		return t_fail_ret("stage-2 has no block for %#llx", t_scratch);
	if (!(before & S2_VALID)) {
		t_fail("the scratch block %#llx is already invalid (%#llx)",
		       t_scratch, before);
		return -EINVAL;
	}

	hyp_mmio_watch(t_scratch, t_scratch + 8, t_watch_hook);
	if (hyp_s2_block(t_scratch, &after)) {
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("the block vanished while it was watched");
	}
	if (after & S2_VALID) {
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("watching left the block valid (%#llx)", after);
	}
	if ((after & ~S2_VALID) != (before & ~S2_VALID)) {
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("watching changed the attributes: %#llx -> %#llx",
				  before, after);
	}
	if ((after & S2_OA_MASK) != t_scratch) {
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("watching moved the block: OA=%#llx",
				  after & S2_OA_MASK);
	}

	hyp_mmio_unwatch(t_scratch);
	if (hyp_s2_block(t_scratch, &after) || after != before) {
		t_fail("unwatch did not put the block back: %#llx -> %#llx",
		       before, after);
		return -EIO;
	}

	return 0;
}

/*
 * A trapped store, replayed: the hook sees it first, the value reaches the real
 * address, HCR_EL2 comes back exactly as it was, and ELR steps over the
 * instruction so the guest would carry on.
 */
static int __hyp t_mmio_replay_store(void)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(false);
	u64 addr = t_scratch + 0x40, value = 0x5a5af00dfeedbeefULL;
	u64 hcr_before, hcr_after, out = 0;
	struct t_watch_rec rec;
	int rc;

	t_watch_reset();
	hyp_mmio_watch(addr, addr + 8, t_watch_hook);

	hcr_before = mrs(hcr_el2);
	rc = t_replay(pcpu, T_INSN_STR_X, addr, value, &out);
	hcr_after = mrs(hcr_el2);

	t_check(rc == 0, "hyp_s2_emulate() returned %d for a watched store", rc);
	t_check(t_watch_calls == 1, "the watch hook ran %u times for one store",
		t_watch_calls);

	rec = t_watch_read(0);
	t_check(rec.store == 1, "the hook saw a load (%u), not a store", rec.store);
	t_check(rec.size == 8, "the hook saw a %u-byte access, not 8", rec.size);
	t_check(rec.far == addr, "FAR was %#llx, not the watched address %#llx",
		rec.far, addr);
	t_check(rec.val == value, "the hook was handed %#llx, not %#llx",
		rec.val, value);
	t_check(rec.cpu == (u32)pcpu->id, "the hook reported cpu%u, not cpu%u",
		rec.cpu, (unsigned)pcpu->id);

	t_check(hcr_after == hcr_before, "HCR_EL2 is %#llx after the replay, was %#llx",
		hcr_after, hcr_before);
	t_check(t_regs.elr == (u64)&t_insn + 4, "ELR went to %#llx, not four past the insn",
		t_regs.elr);

	hyp_mmio_unwatch(addr);
	t_check(readq(addr) == value, "memory at %#llx holds %#llx, not the replayed %#llx",
		addr, readq(addr), value);

	return 0;
}

/*
 * The same for a load: the value comes off the real address and lands in the
 * register the trapped instruction names.
 */
static int __hyp t_mmio_replay_load(void)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(false);
	u64 addr = t_scratch + 0x40, value = 0xcafebabedeadbeefULL;
	u64 out = 0;
	struct t_watch_rec rec;
	int rc;

	/* Put the value there while the block is mapped, and push it out of
	 * this core's cache: the replay reads the address itself.
	 */
	writeq(value, addr);
	hyp_clean_range((const void *)addr, 8);

	t_watch_reset();
	hyp_mmio_watch(addr, addr + 8, t_watch_hook);

	rc = t_replay(pcpu, T_INSN_LDR_X, addr, 0, &out);

	t_check(rc == 0, "hyp_s2_emulate() returned %d for a watched load", rc);
	t_check(t_watch_calls == 1, "the watch hook ran %u times for one load",
		t_watch_calls);

	rec = t_watch_read(0);
	t_check(rec.store == 0, "the hook saw a store (%u), not a load", rec.store);
	t_check(rec.size == 8, "the hook saw a %u-byte access, not 8", rec.size);
	t_check(rec.far == addr, "FAR was %#llx, not the watched address %#llx",
		rec.far, addr);
	t_check(out == value, "x5 came back as %#llx, not the value at %#llx (%#llx)",
		out, addr, value);

	hyp_mmio_unwatch(addr);

	return 0;
}

/*
 * Watch, fault, unwatch, fault again.  The first replay goes through the
 * hook with stage-2 dropped for the physical access; the second one, with the
 * block back in the map, must not reach the hook at all and must still land.
 */
static int __hyp t_mmio_spy_toggle(void)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(false);
	u64 addr = t_scratch + 0x40, mapped = 0, watched = 0;
	u64 hcr_mapped, hcr_unmapped, value = 0x1234567890abcdefULL;
	int rc;

	if (hyp_s2_block(addr, &mapped))
		return t_fail_ret("no block for %#llx", addr);

	t_watch_reset();
	hyp_mmio_watch(addr, addr + 8, t_watch_hook);
	if (hyp_s2_block(addr, &watched)) {
		hyp_mmio_unwatch(addr);
		return t_fail_ret("the block vanished while it was watched");
	}
	if (watched & S2_VALID) {
		hyp_mmio_unwatch(addr);
		return t_fail_ret("watching left %#llx in the map", addr);
	}

	hcr_unmapped = mrs(hcr_el2);
	rc = t_replay(pcpu, T_INSN_STR_X, addr, value, NULL);
	hcr_unmapped = mrs(hcr_el2) ^ hcr_unmapped;
	t_check(rc == 0, "the watched replay returned %d", rc);
	t_check(hcr_unmapped == 0, "HCR_EL2 changed across the watched replay: %#llx",
		hcr_unmapped);
	t_check(t_watch_calls == 1, "the watched replay called the hook %u times",
		t_watch_calls);

	hyp_mmio_unwatch(addr);
	if (hyp_s2_block(addr, &mapped) || !(mapped & S2_VALID))
		return t_fail_ret("unwatch left %#llx out of the map (desc %#llx)",
				  addr, mapped);

	/* Unwatched, the block is an ordinary mapped one: a guest access to it
	 * never traps, so a trap that reaches the emulator is not a missing
	 * mapping and is not a spy access either.  It is refused, never
	 * replayed: no hook call, no store, HCR_EL2 untouched. */
	hcr_mapped = mrs(hcr_el2);
	rc = t_replay(pcpu, T_INSN_STR_X, addr, value ^ 0xffffULL, NULL);
	hcr_mapped = mrs(hcr_el2) ^ hcr_mapped;
	t_check(rc != 0, "the unwatched access was replayed (%d)", rc);
	t_check(t_watch_calls == 1, "an unwatched access reached the hook (%u calls)",
		t_watch_calls);
	t_check(hcr_mapped == 0, "HCR_EL2 changed across the unwatched fault: %#llx",
		hcr_mapped);

	t_check(readq(addr) == value,
		"memory at %#llx holds %#llx, not the first replay's %#llx", addr,
		readq(addr), value);

	return 0;
}

/*
 * A denied range (hyp_mmio_deny(), the USB controller after the hand-over) is
 * not the guest's any more: a load reads zero without touching the device, a
 * store is dropped, and in both cases the guest steps over the instruction.
 */
static int __hyp t_mmio_deny_guest(void)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(false);
	u64 addr = t_scratch + 0x40, value = 0x1122334455667788ULL;
	u64 out = ~0ULL;
	int rc;

	writeq(value, addr);
	hyp_clean_range((const void *)addr, 8);

	hyp_mmio_deny(addr, addr + 8);

	rc = t_replay(pcpu, T_INSN_LDR_X, addr, 0, &out);
	t_check(rc == 0, "hyp_s2_emulate() returned %d for a load from a denied range", rc);
	t_check(out == 0, "a denied load returned %#llx, not zero", out);
	t_check(t_regs.elr == (u64)&t_insn + 4, "ELR went to %#llx after the denied load, not four past the insn",
		t_regs.elr);

	rc = t_replay(pcpu, T_INSN_STR_X, addr, ~value, NULL);
	t_check(rc == 0, "hyp_s2_emulate() returned %d for a store to a denied range", rc);
	t_check(t_regs.elr == (u64)&t_insn + 4, "ELR went to %#llx after the denied store, not four past the insn",
		t_regs.elr);

	hyp_mmio_unwatch(addr);
	t_check(readq(addr) == value, "a denied store reached memory: %#llx, expected %#llx",
		readq(addr), value);

	return 0;
}

/*
 * Does a denied (stage-2-invalid) block stop EL2's own accesses?  Architecturally
 * it must not: HCR_EL2.VM only enables stage 2 for the EL1&0 regime, and EL2 here
 * runs its own stage-1 regime (HCR_EL2.E2H clear).  The USB driver depends on it:
 * the controller window is denied to the guest and EL2 programs it directly.  The
 * old comments in mmio.c claim the opposite, so this settles it on the board: an
 * EL2 abort here would take the EL2 data-abort path and halt the suite.
 */
static int __hyp t_mmio_el2_unaffected(void)
{
	u64 addr = t_scratch + 0x40, value = 0x0badc0de1badc0deULL, got, desc = S2_VALID;

	writeq(value, addr);
	hyp_clean_range((const void *)addr, 8);

	hyp_mmio_deny(addr, addr + 8);

	/* The experiment only means something with stage 2 on and the block out of it */
	t_check(mrs(hcr_el2) & HCR_VM, "HCR_EL2.VM is clear, stage 2 is off: %#llx",
		(u64)mrs(hcr_el2));
	t_check(hyp_s2_block(addr, &desc) == 0 && !(desc & S2_VALID),
		"the denied block is still valid in stage 2 (%#llx)", desc);

	got = readq(addr);
	writeq(~value, addr);
	hyp_mmio_unwatch(addr);

	t_check(got == value, "an EL2 read of a denied block returned %#llx, not %#llx",
		got, value);
	t_check(readq(addr) == ~value, "an EL2 write to a denied block did not land (%#llx)",
		readq(addr));

	return 0;
}

/*
 * ICC_SGI1R_EL1 is the one CPU-interface access the hardware cannot virtualize,
 * so the hypervisor turns the guest's write into a virtual interrupt itself
 * (hyp/gic.c).  Aimed at this core it has to land pending, unbacked, in group 1;
 * aimed at an affinity we do not have it has to be dropped rather than parked
 * somewhere on our interface.
 */
static int __hyp t_vgic_sgi1r_self(void)
{
	int nlrs = hyp_gic_nr_lrs(), i, seen;
	u64 lr;

	t_drain_lrs(-1);

	hyp_gic_emulate_sgi1r(FIELD_PREP(ICC_SGI1R_TARGET, 1) |
			      FIELD_PREP(ICC_SGI1R_IRQ, HYPTEST_SGI_VINTID));

	seen = -1;
	for (i = 0; i < nlrs; i++) {
		lr = hyp_gic_read_lr(i);
		if (lr & LR_STATE_MASK) {
			seen = i;
			break;
		}
	}

	t_check(seen >= 0, "an SGI for our own target list left no list register pending");
	if (seen >= 0) {
		lr = hyp_gic_read_lr(seen);
		t_check(lr & LR_PENDING, "ICH_LR%d_EL2 is not pending (%#llx)", seen, lr);
		t_check(LR_GET_VINTID(lr) == HYPTEST_SGI_VINTID,
			"ICH_LR%d_EL2 vINTID is %d, not the SGI's %d", seen,
			LR_GET_VINTID(lr), HYPTEST_SGI_VINTID);
		t_check(!(lr & LR_HW),
			"an emulated SGI claims a physical interrupt (%#llx)", lr);
		t_check(lr & LR_GRP1, "the emulated SGI is not Group 1 (%#llx)", lr);
		t_check(LR_GET_PRIORITY(lr) == HYP_IRQ_PRIO,
			"the emulated SGI priority is %llu, not %d",
			LR_GET_PRIORITY(lr), HYP_IRQ_PRIO);
	}
	t_drain_lrs(-1);

	hyp_gic_emulate_sgi1r(FIELD_PREP(ICC_SGI1R_TARGET, BIT(15)) |
			      FIELD_PREP(ICC_SGI1R_AFF1, 0xff) |
			      FIELD_PREP(ICC_SGI1R_IRQ, HYPTEST_SGI_VINTID));
	for (i = 0; i < nlrs; i++) {
		lr = hyp_gic_read_lr(i);
		t_check(!(lr & LR_STATE_MASK),
			"lr%d holds %#llx after an SGI aimed at affinity3 0xff", i, lr);
	}
	t_drain_lrs(-1);

	return 0;
}

/*
 * The SMCCC conduit the hypervisor uses to reach EL3 answers from EL2, which is
 * where hyp_smp_start() issues its CPU_ON calls.  A version reply is the cheap
 * proof that the conduit works at all: a negative answer is EL3 refusing to
 * talk, and every SMP case after this one depends on it.
 */
static int __hyp t_smp_smccc_version(void)
{
	struct arm_smccc_res res;

	arm_smccc_smc(ARM_SMCCC_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);
	t_check((long)res.a0 >= ARM_SMCCC_VERSION_1_0,
		"ARM_SMCCC_VERSION returned %ld, not a 1.x convention version",
		(long)res.a0);

	arm_smccc_smc(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &res);
	t_check((long)res.a0 >= 0, "PSCI_VERSION returned an error (%ld)",
		(long)res.a0);
	t_check(FIELD_GET(GENMASK(31, 16), res.a0) ||
		FIELD_GET(GENMASK(15, 0), res.a0),
		"PSCI_VERSION returned %#llx, which encodes no major.minor", res.a0);

	return 0;
}

/*
 * PSCI_FEATURES is what hyp_smp_start() relies on, and its reply has to agree
 * with the revision the firmware claims: CPU_ON - the exact function id smp.c
 * calls - has to be advertised as implemented under a 1.x PSCI, and a firmware
 * older than 1.0 must not answer FEATURES at all.  Either way the harness needs
 * to know which one it is driving before it asks EL3 to wake a core.
 */
static int __hyp t_smp_psci_features(void)
{
	struct arm_smccc_res ver, on, off;
	u64 major;

	arm_smccc_smc(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &ver);
	arm_smccc_smc(PSCI_1_0_FN_PSCI_FEATURES, PSCI_0_2_FN64_CPU_ON, 0, 0, 0, 0,
		      0, 0, &on);
	arm_smccc_smc(PSCI_1_0_FN_PSCI_FEATURES, PSCI_0_2_FN_CPU_OFF, 0, 0, 0, 0,
		      0, 0, &off);
	major = FIELD_GET(GENMASK(31, 16), ver.a0);

	if (major >= 1) {
		t_check((long)on.a0 == PSCI_RET_SUCCESS,
			"PSCI %llu.x reports CPU_ON as %ld, so no core can be started",
			major, (long)on.a0);
		t_check((long)off.a0 == PSCI_RET_SUCCESS,
			"PSCI %llu.x reports CPU_OFF as %ld", major, (long)off.a0);
	} else {
		t_check((long)on.a0 == PSCI_RET_NOT_SUPPORTED,
			"PSCI %llu.x answers FEATURES for %#llx with %ld, but FEATURES is a 1.0 function",
			major, (u64)PSCI_0_2_FN64_CPU_ON, (long)on.a0);
	}

	return 0;
}

/*
 * The cores the hypervisor may start are the DT's cpu nodes, in DT order, and
 * hyp_smp_start() takes an affinity rather than a slot number: every affinity
 * in the table has to map back to its own slot, no two cores may share one, and
 * an affinity the DT does not describe must not answer as a slot at all.
 * (cpu@0's reg is <0>, so a zero affinity is a real core, not an empty slot.)
 */
static int __hyp t_smp_affinity_table(void)
{
	int i, j, n = hyp_cpu_count();

	t_check(n > 0 && n <= HYP_MAX_CPUS, "hyp_cpu_count() is %d, outside [1, %d]",
		n, HYP_MAX_CPUS);

	for (i = 0; i < n; i++) {
		u64 mpidr = hyp_cpu_mpidr(i);

		t_check(hyp_cpu_id(mpidr) == i,
			"cpu%d affinity %#llx maps back to slot %d, not %d", i, mpidr,
			hyp_cpu_id(mpidr), i);
		for (j = i + 1; j < n; j++)
			t_check(mpidr != hyp_cpu_mpidr(j),
				"cpu%d and cpu%d share affinity %#llx", i, j, mpidr);
	}

	t_check(hyp_cpu_id(hyp_cpu_mpidr(n - 1) | (0xffULL << 8)) == -1,
		"affinity %#llx is no DT core, yet hyp_cpu_id() answers slot %d",
		hyp_cpu_mpidr(n - 1) | (0xffULL << 8),
		hyp_cpu_id(hyp_cpu_mpidr(n - 1) | (0xffULL << 8)));

	return 0;
}

/*
 * Each core the harness can start gets its own EL2 stack out of the hyp_mem
 * region: distinct, 16-byte aligned (the trampoline sets its stack top that
 * way) and inside the region the hypervisor owns.  Two cores sharing a stack is
 * the corruption the SMP cases would never see coming.
 */
static int __hyp t_smp_table_geometry(void)
{
	int i, j, n = hyp_cpu_count();

	for (i = 0; i < n; i++) {
		u64 stack = hyp_mem_stack(i);

		t_check(stack, "cpu%d has no stack slot", i);
		t_check((stack & 0xf) == 0, "cpu%d stack top %#llx is not 16-byte aligned",
			i, stack);
		for (j = i + 1; j < n; j++)
			t_check(stack != hyp_mem_stack(j),
				"cpu%d and cpu%d share stack top %#llx", i, j, stack);
	}

	return 0;
}

/*
 * Two-core cases.  CPU0 publishes a job number in a shared word, starts a core
 * the DT describes, and that core runs the job at EL2 in hyp_test_secondary()
 * - hyp/smp.c calls it with the console lock released, on the way to that
 * core's EL1 handoff.  The shared words below are the only channel between
 * them: every read cleans the line first, so what is compared is what memory
 * holds and not this core's copy of it, and every assertion is on a value.
 */
#define HYPTEST_JOB_NONE		0
#define HYPTEST_JOB_SPIN		1
#define HYPTEST_JOB_MUTEX		2
#define HYPTEST_JOB_CONSOLE		3
#define HYPTEST_JOB_GIC		4
#define HYPTEST_JOB_SGI		5	/* inject an SGI on this core, at EL2 */
#define HYPTEST_JOB_EL1		6	/* announce, then run the EL1 payload */
#define HYPTEST_ITERS			32	/* per core */
#define HYPTEST_LOCK_ITERS		16	/* beats while CPU0 reads the row */
#define HYPTEST_WAIT_SPINS		8000000	/* hyp/ has no timers */
#define HYPTEST_GO_SPINS		2000000	/* the EL1 payload waiting for CPU0 */
#define HYPTEST_IAR_SPINS		100000	/* the EL1 payload waiting for its irq */
#define HYPTEST_RECORD_MAGIC		0x48595052
#define HYPTEST_VINTID		0x3ff	/* no DT interrupt, so it stays put */
#define HYPTEST_EL1_VINTID		0xc	/* an SGI number, for the core at EL1 */
#define HYPTEST_EL1_VALUE		0x1122334455667788ULL

/* Read a word another core may have written; publish one for it to read. */
#define t_shared(var) (hyp_clean_range(&(var), sizeof(var)), (var))
#define t_publish(var, val) do { (var) = (val);				\
	hyp_clean_range(&(var), sizeof(var)); } while (0)

struct hyp_test_record {
	u32 magic;
	u32 cpu;
	u32 seq;
	u32 sum;				/* magic ^ cpu ^ seq */
};

/*
 * One cache line each.  hyp_clean_range() works a whole 64-byte line at a
 * time, and cleaning a line writes back everything in it: two shared words in
 * one line let a core write its stale copy of its neighbour straight over the
 * update the other core just made.  That is how lock.mutex-contention first
 * lost the increment it was there to catch.
 */
static __hyp_data u32 test_job __aligned(64);
static __hyp_data u32 test_done __aligned(64);
static __hyp_data u32 test_counter __aligned(64);
static __hyp_data u32 test_seq __aligned(64);
static __hyp_data u32 test_trylock_failed __aligned(64);
static __hyp_data u32 test_lr_readback __aligned(64);
static __hyp_data u32 test_record_next __aligned(64);
static __hyp_data u32 test_sgi_slot __aligned(64);
static __hyp_data u32 test_sgi_lr_lo __aligned(64);
static __hyp_data u32 test_sgi_lr_hi __aligned(64);
static __hyp_data u32 test_cpu_id __aligned(64);
static __hyp_data u64 test_mpidr __aligned(64);
static __hyp_data struct hyp_test_record test_records[HYPTEST_ITERS * 2] __aligned(64);
static __aligned(64) DEFINE_SPINLOCK(two_core_spin);
static __aligned(64) DEFINE_MUTEX(test_handover);

/**
 * hyp_test_secondary() - run the job CPU0 left for this core, at EL2
 * @pcpu: this core's hypervisor context
 *
 * Called from hv_secondary_start() after the console lock has been put back and
 * immediately before the core's EL1 handoff, so this runs on the core's own
 * EL2 context with stage-1, stage-2 and its own CPU interface live.  test_job
 * is the only input and the shared words CPU0 polls are the only output;
 * nothing here prints, so a job cannot deadlock on a console lock another core
 * is holding.
 */
void __hyp hyp_test_secondary(struct hyp_percpu *pcpu)
{
	u32 job, i, idx, slot = 0, lo = 0, hi = 0;
	u64 lr, self;
	struct hyp_test_record *rec;

	hyp_clean_range(&test_job, sizeof(test_job));
	job = test_job;

	switch (job) {
	case HYPTEST_JOB_SPIN:
		for (i = 0; i < HYPTEST_ITERS; i++) {
			spin_lock(&two_core_spin);
			test_counter = t_shared(test_counter) + 1;
			hyp_clean_range(&test_counter, sizeof(test_counter));
			spin_unlock(&two_core_spin);
		}
		break;
	case HYPTEST_JOB_MUTEX:
		/* CPU0 holds it: the trylock has to fail, and the blocking lock
		 * has to wait until CPU0 lets go.
		 */
		t_publish(test_trylock_failed, mutex_trylock(&test_handover) ? 0 : 1);
		mutex_lock(&test_handover);
		if (t_shared(test_seq) == 1)
			test_seq = 2;	/* only a core that came after CPU0 sees 1 */
		test_counter = t_shared(test_counter) + 1;
		hyp_clean_range(&test_seq, sizeof(test_seq));
		hyp_clean_range(&test_counter, sizeof(test_counter));
		mutex_unlock(&test_handover);
		break;
	case HYPTEST_JOB_CONSOLE:
		for (i = 0; i < HYPTEST_ITERS; i++) {
			hyp_smp_console_lock();
			idx = t_shared(test_record_next);
			if (idx < ARRAY_SIZE(test_records)) {
				rec = &test_records[idx];
				rec->magic = HYPTEST_RECORD_MAGIC;
				rec->cpu = pcpu->id;
				rec->seq = i;
				rec->sum = rec->magic ^ rec->cpu ^ rec->seq;
				hyp_clean_range(rec, sizeof(*rec));
			}
			t_publish(test_record_next, idx + 1);
			hyp_smp_console_unlock();
		}
		break;
	case HYPTEST_JOB_GIC:
		for (i = 0; i < HYPTEST_ITERS; i++) {
			hyp_gic_write_lr(0, LR_PENDING | LR_PRIORITY(HYP_IRQ_PRIO) |
					 LR_VINTID(HYPTEST_VINTID));
			hyp_gic_sync();
			spin_lock(&two_core_spin);
			test_counter = t_shared(test_counter) + 1;
			hyp_clean_range(&test_counter, sizeof(test_counter));
			spin_unlock(&two_core_spin);
		}
		t_publish(test_lr_readback, (u32)hyp_gic_read_lr(0));
		hyp_gic_write_lr(0, 0);
		hyp_gic_sync();
		break;
	case HYPTEST_JOB_SGI:
		/* An SGI aimed at this core, through the same emulation a guest
		 * HVC would take.  TargetList is per-Aff1, so the affinity
		 * fields have to name *this* core - hyp/gic.c drops a routing
		 * that does not include the core running the call.  The list
		 * register it lands in is published for CPU0 to read, then
		 * drained: a pending interrupt must not ride into the EL1
		 * handoff this core continues into.
		 */
		self = mrs(mpidr_el1);
		t_drain_lrs(-1);
		hyp_gic_emulate_sgi1r(FIELD_PREP(ICC_SGI1R_TARGET, BIT(self & 0xff)) |
				      FIELD_PREP(ICC_SGI1R_AFF1, (self >> 8) & 0xff) |
				      FIELD_PREP(ICC_SGI1R_IRQ, HYPTEST_SGI_VINTID));
		for (i = 0; i < (u32)hyp_gic_nr_lrs(); i++) {
			lr = hyp_gic_read_lr(i);
			if ((lr & LR_STATE_MASK) != LR_PENDING)
				continue;
			slot = i;
			lo = (u32)lr;
			hi = (u32)(lr >> 32);
			break;
		}
		t_publish(test_sgi_slot, slot);
		t_publish(test_sgi_lr_lo, lo);
		t_publish(test_sgi_lr_hi, hi);
		t_drain_lrs(-1);
		break;
	case HYPTEST_JOB_EL1:
		/* This core is about to run an EL1 payload; say who it is while
		 * it is still here at EL2, and start from an empty interface so
		 * the payload's own interrupt is the only one it can take.
		 */
		t_publish(test_cpu_id, (u32)pcpu->id);
		t_publish(test_mpidr, pcpu->mpidr);
		t_drain_lrs(-1);
		break;
	default:
		return;
	}

	t_publish(test_done, job);
}

/*
 * t_start_core_payload() - publish a job for a DT core and wake it with a
 * chosen EL1 payload
 * @slot: DT slot to start, never the one this core is running on
 * @job: which hyp_test_secondary() payload that core has to run
 * @entry: its EL1 payload, as a link-time .text address (hyp/smp.c adds the
 * relocation offset), or NULL for the default one
 *
 * The done flag is cleared before the job is published so a stale one cannot
 * answer for the new job, and the job is published before CPU_ON so the core
 * cannot miss it.
 *
 * Return: hyp_smp_start_payload()'s result.
 */
static int __hyp t_start_core_payload(int slot, u32 job, void *entry)
{
	if (slot >= hyp_cpu_count()) {
		t_fail("the DT describes %d cores, there is no cpu%d",
		       hyp_cpu_count(), slot);
		return -EINVAL;
	}

	t_publish(test_done, HYPTEST_JOB_NONE);
	t_publish(test_job, job);

	if (entry)
		return hyp_smp_start_payload(hyp_cpu_mpidr(slot), entry);

	return hyp_smp_start(hyp_cpu_mpidr(slot));
}

static int __hyp t_start_core(int slot, u32 job)
{
	return t_start_core_payload(slot, job, NULL);
}

/*
 * t_wait_for() - bounded wait for another core to publish a value
 * @p: the word to watch
 * @want: the value that means done
 *
 * hyp/ has no timers, so the bound is a spin count and every look cleans the
 * line first.  Returns the spins it took, or 0 if the value never arrived.
 */
static u32 __hyp t_wait_for(const u32 *p, u32 want)
{
	u32 i;

	for (i = 1; i <= HYPTEST_WAIT_SPINS; i++) {
		if ((i & 0xfff) == 0)
			hyp_clean_range(p, sizeof(*p));
		if (*p == want)
			return i;
	}

	return 0;
}

/*
 * EL1 cases: one core runs a payload at EL1 under the live hypervisor while
 * CPU0 stays at EL2 and scores what it reports.
 *
 * t_el1_payload() is ordinary .text and t_el1_addr / t_el1_go are ordinary .bss,
 * for the same reason hv_secondary_guest_entry() is: relocate_code() moves the
 * guest's .text and .bss and deliberately leaves .hyp alone, and this payload
 * runs as a guest.  CPU0 reaches them at their relocated address.  It is called
 * t_el1_payload and not hyp_test_el1 because -ffunction-sections names a
 * function's section after it, and the linker script pulls every .text.hyp* into
 * the resident .hyp span - a payload named hyp_* would never be relocated.  The
 * payload prints nothing, calls no hypervisor code and decides nothing itself -
 * every assertion lives in the cases that read its reports.
 */
/* Each one gets its own cache line: CPU0 writes them, the payload reads them. */
static volatile u64 t_el1_addr __aligned(64);
static volatile u64 t_el1_go __aligned(64);
static volatile u64 t_el1_fire __aligned(64);

/* Write one of the payload's own variables where the payload will read it. */
static void __hyp t_el1_publish(void *var, u64 val)
{
	u64 *p = (u64 *)((u64)(unsigned long)var + hyp_reloc_off());

	*p = val;
	hyp_clean_range(p, sizeof(*p));
}

/*
 * t_el1_gate() - the payload waiting for CPU0 to release one of its stages
 * @step: the stage number CPU0 has reached
 *
 * Bounded, because a hypervisor test must not hang a core if CPU0 gave up on
 * it.  Returns whether the gate opened.
 */
static u64 t_el1_gate(u64 step)
{
	u64 i;

	for (i = 0; i < HYPTEST_GO_SPINS; i++) {
		if (t_el1_fire >= step)
			break;
	}

	return t_el1_fire >= step;
}

/*
 * t_wait_stage() - bounded wait for the payload to report one of its stages
 * @stage: which report to wait for
 *
 * Returns the spins it took, or 0 if the report never arrived.
 */
static u32 __hyp t_wait_stage(int stage)
{
	int i;

	for (i = 1; i <= HYPTEST_WAIT_SPINS; i++) {
		if ((i & 0xfff) == 0)
			hyp_clean_range(&el1_report[stage], sizeof(el1_report[stage]));
		if (el1_report[stage].done)
			return i;
	}

	return 0;
}

/* One stage report, as memory holds it rather than as this core last saw it. */
static struct hyp_test_el1_report __hyp t_stage(int stage)
{
	hyp_clean_range(&el1_report[stage], sizeof(el1_report[stage]));

	return el1_report[stage];
}

/*
 * t_el1_payload() - the payload a core runs at EL1
 *
 * hyp_smp_start_payload() puts it there through the handoff hyp/smp.c sets:
 * EL1h, DAIF masked (SPSR_EL2 0x3c5), MMU off, its own stack, stage-1 and
 * stage-2 live, its own virtual CPU interface.  Every stage reports itself with
 * `hvc #HV_TEST_RESULT` (x1 = stage, x2 = what it measured, x3 = what it
 * expected); the EL2 side scores it.
 *
 * Stages 4 and 5 name x4 and x5 on purpose: the EL2 entry/exit frame saves
 * x0..x28 and not x29/x30, and hyp_s2_emulate() takes the value out of the
 * frame slot the trapping instruction names - so a trapped access has to name a
 * register the frame actually saves.
 */
static void t_el1_payload(void)
{
	struct arm_smccc_res res;
	register u64 rt asm("x5");
	register u64 ra asm("x4");
	u64 mpidr;
	u32 i, intid;

	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_MPIDR, mpidr, 0, 0, 0, 0, 0,
		      &res);

	arm_smccc_hvc(HV_GET_ID, 0, 0, 0, 0, 0, 0, 0, &res);
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_HVC, res.a0, HV_ID_MAGIC, 0, 0,
		      0, 0, &res);

	/* Ask for an interrupt the way a guest does, with the one CPU-interface
	 * access no hardware can virtualize: hyp/hv.c traps the write and
	 * injects it here.  TargetList is per-Aff1, so the affinity fields have
	 * to name this core - as the hypervisor presented it, which is the
	 * affinity hyp/gic.c matches the call against.  The answer then comes
	 * back through the virtual interface, which the hardware serves without
	 * a trap.
	 */
	msr(icc_sgi1r_el1, FIELD_PREP(ICC_SGI1R_TARGET, BIT(mpidr & 0xff)) |
			   FIELD_PREP(ICC_SGI1R_AFF1, (mpidr >> 8) & 0xff) |
			   FIELD_PREP(ICC_SGI1R_IRQ, HYPTEST_EL1_VINTID));
	asm volatile("isb");

	intid = ICC_IAR_SPURIOUS;
	for (i = 0; i < HYPTEST_IAR_SPINS; i++) {
		intid = (u32)mrs(icc_iar1_el1);
		if (intid < ICC_IAR_SPURIOUS)
			break;
	}
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_IAR, intid, HYPTEST_EL1_VINTID,
		      0, 0, 0, 0, &res);

	msr(icc_eoir1_el1, (u64)intid);
	asm volatile("isb");
	intid = (u32)mrs(icc_iar1_el1);
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_EOIR, intid, ICC_IAR_SPURIOUS,
		      0, 0, 0, 0, &res);

	/* The genuine guest fault: a store into, and a load back out of, a range
	 * the hypervisor has taken out of the stage-2 map.  Nothing here knows.
	 *
	 * CPU0 releases these stages one at a time and reads what each leaves
	 * behind before releasing the next; the address stage is the safety
	 * catch, so a core that would fault on the wrong address never gets to
	 * the access.  ra and rt are set again for every access - arm_smccc_hvc()
	 * reports through the very caller-saved registers the trapping
	 * instruction names, so an ra carried over a report is whatever that
	 * call left in x4.
	 */
	t_el1_gate(1);
	ra = t_el1_addr;
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_ADDR, ra, 0, 0, 0, 0, 0, &res);

	t_el1_gate(2);
	rt = HYPTEST_EL1_VALUE;
	ra = t_el1_addr;
	asm volatile("str %0, [%1]" :: "r"(rt), "r"(ra) : "memory");
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_STORE, rt, HYPTEST_EL1_VALUE,
		      0, 0, 0, 0, &res);

	t_el1_gate(3);
	rt = 0;
	ra = t_el1_addr;
	asm volatile("ldr %0, [%1]" : "=r"(rt) : "r"(ra) : "memory");
	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_LOAD, rt, HYPTEST_EL1_VALUE, 0,
		      0, 0, 0, &res);

	/* Wait for CPU0 to start reading this core's vCPU row, then beat on it
	 * from EL1 while it does: the two cores meet on hyp/smp.c's smp_lock
	 * from opposite sides.
	 */
	for (i = 0; i < HYPTEST_GO_SPINS; i++) {
		if (t_el1_go)
			break;
	}

	for (i = 0; i < HYPTEST_LOCK_ITERS; i++)
		arm_smccc_hvc(HV_SMP_DONE, 0, 0, 0, 0, 0, 0, 0, &res);

	arm_smccc_hvc(HV_TEST_RESULT, HYPTEST_EL1_BEATS, i, HYPTEST_LOCK_ITERS,
		      0, 0, 0, 0, &res);

	arm_smccc_hvc(HV_SMP_DONE, 1, 0, 0, 0, 0, 0, 0, &res);

	for (;;)
		asm volatile("wfe");
}

/*
 * An SGI injected on another core stays on that core.  cpu6 emulates an
 * ICC_SGI1R_EL1 on itself at EL2 and publishes the list register it landed in;
 * the register contents are per-PE, so cpu0's own interface has to be empty
 * afterwards.  (The slot bookkeeping in hyp/gic.c is shared, which is why the
 * assertion is on the registers and not on which slot was handed out.)
 */
static int __hyp t_vgic_sgi_cross_core(void)
{
	int i, n = hyp_gic_nr_lrs(), rc, seen = -1;
	u32 spins, slot, lo, hi;
	u64 lr;

	rc = t_start_core(6, HYPTEST_JOB_SGI);
	t_check(rc == 0, "cpu6 would not start (%d)", rc);
	if (rc)
		return 0;

	spins = t_wait_for(&test_done, HYPTEST_JOB_SGI);
	t_check(spins, "cpu6 never finished its SGI job");

	slot = t_shared(test_sgi_slot);
	lo = t_shared(test_sgi_lr_lo);
	hi = t_shared(test_sgi_lr_hi);
	lr = ((u64)hi << 32) | lo;

	t_check(slot < (u32)n, "cpu6 published list register %u of %d", slot, n);
	if (slot < (u32)n)
		seen = (int)slot;
	t_check((lr & LR_STATE_MASK) == LR_PENDING,
		"cpu6's SGI is not pending (%#llx)", lr);
	t_check(LR_GET_VINTID(lr) == HYPTEST_SGI_VINTID,
		"cpu6's SGI vINTID is %d, not %#x", LR_GET_VINTID(lr),
		HYPTEST_SGI_VINTID);
	t_check(!(lr & LR_HW), "cpu6's emulated SGI claims a physical interrupt (%#llx)",
		lr);
	t_check(lr & LR_GRP1, "cpu6's SGI is not Group 1 (%#llx)", lr);
	t_check(LR_GET_PRIORITY(lr) == HYP_IRQ_PRIO,
		"cpu6's SGI priority is %#llx, not %#x", LR_GET_PRIORITY(lr),
		HYP_IRQ_PRIO);
	t_check(seen >= 0, "cpu6 found no pending list register at all");

	/* The SGI belongs to cpu6's list registers.  cpu0's own may hold other
	 * things (the live console interrupt, a physical one with HW set); what
	 * must not be there is cpu6's SGI. */
	for (i = 0; i < n; i++) {
		lr = hyp_gic_read_lr(i);
		t_check(!(lr & LR_STATE_MASK) || LR_GET_VINTID(lr) != HYPTEST_SGI_VINTID,
			"cpu0 list register %d holds cpu6's SGI after it (%#llx)", i, lr);
	}

	return 0;
}

/*
 * The stage-2 spy serving a real guest access: cpu7 stores to and loads from a
 * block the hypervisor has taken out of the map, with no idea it is being
 * emulated.  The watch hook saw both accesses, the value reached the hardware,
 * and the load that came back is the value the store put there - all of it
 * through the trap path a guest takes, not a synthesized frame.
 */
static int __hyp t_mmio_fault_from_el1(void)
{
	struct hyp_test_el1_report st, ld;
	struct t_watch_rec rec;
	u64 addr = t_scratch + 0x800, line = (t_scratch + 0x800) & ~63UL;
	u32 spins, calls;
	int rc;

	if (t_mmio_prepare())
		return -ENOMEM;

	t_watch_reset();
	t_el1_publish((void *)&t_el1_addr, addr);
	t_el1_publish((void *)&t_el1_go, 0);
	t_el1_publish((void *)&t_el1_fire, 0);
	hyp_mmio_watch(t_scratch, t_scratch + SZ_2M, t_watch_hook);

	rc = t_start_core_payload(7, HYPTEST_JOB_EL1, t_el1_payload);
	if (rc) {
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("cpu7 would not start (%d)", rc);
	}

	/* Hand the payload one stage at a time: what it names before it faults,
	 * then the store, then the load.  Nothing dangerous runs before CPU0 has
	 * read the stage that precedes it, and every early exit releases the
	 * gates so the core still reaches the stages the later cases need.
	 */
	spins = t_wait_stage(HYPTEST_EL1_EOIR);
	if (!spins) {
		t_el1_publish((void *)&t_el1_fire, 3);
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("cpu7 never finished its virtual-irq stages");
	}
	t_el1_publish((void *)&t_el1_fire, 1);

	spins = t_wait_stage(HYPTEST_EL1_ADDR);
	st = t_stage(HYPTEST_EL1_ADDR);
	if (!spins || st.value != addr) {
		t_el1_publish((void *)&t_el1_fire, 3);
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("cpu7 faults on %#llx, not the published %#llx",
				  st.value, addr);
	}

	t_el1_publish((void *)&t_el1_fire, 2);
	spins = t_wait_stage(HYPTEST_EL1_STORE);
	if (!spins) {
		t_el1_publish((void *)&t_el1_fire, 3);
		hyp_mmio_unwatch(t_scratch);
		return t_fail_ret("cpu7 never reported the store fault (%u hook calls)",
				  t_shared(t_watch_calls));
	}

	rec = t_watch_read(0);
	t_check(rec.cpu == 7, "the store was recorded on cpu%u, not cpu7", rec.cpu);
	t_check(rec.store == 1, "the first recorded access was not a store");
	t_check(rec.size == 8, "the trapped store is %u bytes, not 8", rec.size);
	t_check(rec.far == addr, "FAR_EL2 for the store is %#llx, not %#llx",
		rec.far, addr);
	t_check(rec.val == HYPTEST_EL1_VALUE,
		"the hook saw %#llx stored, not %#llx", rec.val, HYPTEST_EL1_VALUE);

	t_el1_publish((void *)&t_el1_fire, 3);
	spins = t_wait_stage(HYPTEST_EL1_LOAD);
	t_check(spins, "cpu7 never reported the load fault (%u hook calls)",
		t_shared(t_watch_calls));

	hyp_mmio_unwatch(t_scratch);

	calls = t_shared(t_watch_calls);
	t_check(calls == 2, "%u accesses reached the watch hook, not 2", calls);

	rec = t_watch_read(1);
	t_check(rec.store == 0, "the second recorded access was not a load");
	t_check(rec.far == addr, "FAR_EL2 for the load is %#llx, not %#llx",
		rec.far, addr);
	t_check(rec.val == HYPTEST_EL1_VALUE,
		"the hook saw %#llx loaded, not %#llx", rec.val, HYPTEST_EL1_VALUE);

	/* Out of any cache that could still be holding it, then read the
	 * hardware the way the next guest access would.
	 */
	asm volatile("dc ivac, %0" :: "r"(line));
	asm volatile("dsb sy");
	t_check(readq(addr) == HYPTEST_EL1_VALUE,
		"%#llx holds %#llx after the emulated store, not %#llx", addr,
		readq(addr), HYPTEST_EL1_VALUE);

	st = t_stage(HYPTEST_EL1_STORE);
	ld = t_stage(HYPTEST_EL1_LOAD);
	t_check(st.cpu == 7 && ld.cpu == 7, "the fault reports came from cpu%u and cpu%u",
		st.cpu, ld.cpu);
	t_check(ld.value == HYPTEST_EL1_VALUE,
		"cpu7 loaded %#llx, not %#llx", ld.value, HYPTEST_EL1_VALUE);
	t_check(ld.value == st.value, "cpu7 stored %#llx and loaded %#llx",
		st.value, ld.value);

	return 0;
}

/*
 * Two cores on one lock, live: CPU0 reads cpu7's vCPU row over and over while
 * cpu7 beats on the same row from EL1.  A heartbeat that never went backwards
 * and a read that caught one land is what "the lock held" looks like from the
 * outside; a lost update shows up as a count below HYPTEST_LOCK_ITERS.
 */
static int __hyp t_smp_two_core_lock(void)
{
	struct hyp_vcpu_info info;
	u32 spins, prev = 0, increases = 0, regressions = 0, reads = 0;
	int i, rc, parked = 0;

	t_el1_publish((void *)&t_el1_go, 1);

	for (i = 0; i < HYPTEST_WAIT_SPINS / 16; i++) {
		rc = hyp_smp_info(7, &info);
		if (rc) {
			t_fail("cpu7 has no vCPU row (%d)", rc);
			return 0;
		}
		if (info.heartbeats < prev)
			regressions++;
		if (info.heartbeats > prev)
			increases++;
		prev = info.heartbeats;
		reads++;
		if (info.parked) {
			parked = 1;
			break;
		}
	}

	t_check(parked, "cpu7 never parked after %u reads on cpu0", reads);
	t_check(!regressions, "%u reads saw cpu7's heartbeat go backwards", regressions);
	t_check(increases > 0,
		"none of the %u reads on cpu0 caught a heartbeat land: the two cores never met on the lock",
		reads);
	t_check(info.started && !info.failed, "cpu7's row says started=%d failed=%d",
		info.started, info.failed);
	t_check(info.mpidr == hyp_cpu_mpidr(7), "cpu7's row holds affinity %#llx, not %#llx",
		info.mpidr, hyp_cpu_mpidr(7));

	spins = t_wait_stage(HYPTEST_EL1_BEATS);
	t_check(spins, "cpu7 never reported its heartbeat burst");
	if (spins) {
		struct hyp_test_el1_report r = t_stage(HYPTEST_EL1_BEATS);

		t_check(r.value == HYPTEST_LOCK_ITERS,
			"cpu7 counted %llu beats, not %d", r.value, HYPTEST_LOCK_ITERS);
		t_check(prev >= r.value, "cpu7 beat %llu times but its row reached %u",
			r.value, prev);
	}

	rc = hyp_smp_info(7, &info);
	t_check(rc == 0 && info.parked, "cpu7 is not parked at the end (%d, parked=%d)",
		rc, info.parked);
	t_check(info.heartbeats == prev, "cpu7's final row reads %u, not %u",
		info.heartbeats, prev);

	return 0;
}

/* An HVC from a guest at EL1, answered by the hypervisor, as the guest saw it. */
static int __hyp t_el1_hvc(void)
{
	struct hyp_test_el1_report r = t_stage(HYPTEST_EL1_HVC);

	t_check(r.done, "cpu7 never reported its HVC stage");
	t_check(r.cpu == 7, "the HVC report came from cpu%u, not cpu7", r.cpu);
	t_check(r.value == HV_ID_MAGIC, "HV_GET_ID answered %#llx, not %#llx",
		r.value, HV_ID_MAGIC);
	t_check(r.value == r.expect, "cpu7 got %#llx for an expect of %#llx",
		r.value, r.expect);

	return 0;
}

/*
 * A virtual interrupt all the way through the guest's own interface: cpu7 asked
 * for one with ICC_SGI1R_EL1 (trapped and emulated), acknowledged it with
 * ICC_IAR1_EL1 and ended it with ICC_EOIR1_EL1 - the last two served by the
 * hardware virtual interface, with no trap at all.  Its own EOI has to leave
 * nothing behind, so the next acknowledge is the spurious INTID.
 */
static int __hyp t_gic_irq_to_el1(void)
{
	struct hyp_test_el1_report ack = t_stage(HYPTEST_EL1_IAR);
	struct hyp_test_el1_report eoi = t_stage(HYPTEST_EL1_EOIR);

	t_check(ack.done, "cpu7 never reported its acknowledge");
	t_check(ack.cpu == 7, "the acknowledge came from cpu%u, not cpu7", ack.cpu);
	t_check(ack.value == HYPTEST_EL1_VINTID,
		"ICC_IAR1_EL1 at EL1 answered %llu, not the virtual interrupt %#x",
		ack.value, HYPTEST_EL1_VINTID);

	t_check(eoi.done, "cpu7 never reported what its EOI left behind");
	t_check(eoi.cpu == 7, "the EOI report came from cpu%u, not cpu7", eoi.cpu);
	t_check(eoi.value >= ICC_IAR_SPURIOUS,
		"the acknowledge after cpu7's EOI returned %llu, not a spurious INTID",
		eoi.value);

	return 0;
}

/*
 * The core CPU0 woke is the core the DT described, and it agrees: the affinity
 * it reads for itself at EL1 is the virtual one the hypervisor programmed, and
 * it maps back to the slot CPU0 started.
 */
static int __hyp t_smp_cpu_on_identity(void)
{
	struct hyp_test_el1_report r = t_stage(HYPTEST_EL1_MPIDR);
	u64 want = MPIDR_UPT | hyp_cpu_mpidr(7), mpidr;
	u32 cpu_id;

	cpu_id = t_shared(test_cpu_id);
	mpidr = t_shared(test_mpidr);
	t_check(cpu_id == 7, "the payload core published its id as %u, not 7", cpu_id);
	t_check(mpidr == hyp_cpu_mpidr(7), "cpu7 published affinity %#llx, not %#llx",
		mpidr, hyp_cpu_mpidr(7));

	t_check(r.done, "cpu7 never reported its MPIDR stage");
	t_check(r.cpu == 7, "the MPIDR report came from cpu%u, not cpu7", r.cpu);
	t_check(r.value == want, "MRS MPIDR_EL1 at EL1 read %#llx, not %#llx",
		r.value, want);
	t_check(hyp_cpu_id(r.value & ~MPIDR_UPT) == 7,
		"the affinity cpu7 read maps to slot %d, not 7",
		hyp_cpu_id(r.value & ~MPIDR_UPT));

	return 0;
}

/*
 * What EL3's PSCI says about the cores this hypervisor starts cores through:
 * the revision, whether AFFINITY_INFO is there at all, and - while every other
 * DT core is still off, which is exactly the state this case runs in - that the
 * answer distinguishes this core from them.
 */
static int __hyp t_smp_psci_version(void)
{
	struct arm_smccc_res ver, feat;
	u32 v;
	int i, n = hyp_cpu_count();

	arm_smccc_smc(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0, 0, 0, 0, 0, &ver);
	t_check((long)ver.a0 >= 0, "PSCI_VERSION returned an error (%ld)",
		(long)ver.a0);
	if ((long)ver.a0 < 0)
		return 0;

	v = (u32)ver.a0;
	t_check(PSCI_VERSION_MAJOR(v) <= 1 &&
		(PSCI_VERSION_MAJOR(v) || PSCI_VERSION_MINOR(v) <= 2),
		"PSCI_VERSION %#x decodes to %u.%u, not a revision this harness drives",
		v, PSCI_VERSION_MAJOR(v), PSCI_VERSION_MINOR(v));
	if (PSCI_VERSION_MAJOR(v) < 1) {
		hprintf("psci: %u.x offers no FEATURES, skipping the per-core check\n",
			PSCI_VERSION_MAJOR(v));
		return 0;
	}

	arm_smccc_smc(PSCI_1_0_FN_PSCI_FEATURES, PSCI_0_2_FN64_AFFINITY_INFO, 0, 0,
		      0, 0, 0, 0, &feat);
	if ((long)feat.a0 != PSCI_RET_SUCCESS) {
		hprintf("psci: AFFINITY_INFO is not offered (%ld), skipping the per-core check\n",
			(long)feat.a0);
		return 0;
	}

	for (i = 0; i < n; i++) {
		struct arm_smccc_res res;

		arm_smccc_smc(PSCI_0_2_FN64_AFFINITY_INFO, hyp_cpu_mpidr(i), 0, 0, 0,
			      0, 0, 0, &res);
		if (i == 0) {
			t_check((long)res.a0 == PSCI_0_2_AFFINITY_LEVEL_ON,
				"cpu0 (%#llx) reports affinity state %ld, not ON",
				hyp_cpu_mpidr(i), (long)res.a0);
		} else {
			t_check((long)res.a0 == PSCI_0_2_AFFINITY_LEVEL_OFF,
				"cpu%d (%#llx) reports %ld before anything started it",
				i, hyp_cpu_mpidr(i), (long)res.a0);
		}
	}

	return 0;
}

/*
 * hyp_smp_start() takes a DT affinity and leaves a row behind that says the
 * core is started with exactly that affinity - a result a test can assert on,
 * not just a "CPU_ON accepted" line on the console.
 */
static int __hyp t_smp_cpu_on_accept(void)
{
	struct hyp_vcpu_info info;
	u64 mpidr = hyp_cpu_mpidr(1);
	int rc;

	t_check(hyp_cpu_count() > 1, "the DT describes %d core, none to start",
		hyp_cpu_count());
	if (hyp_cpu_count() <= 1)
		return 0;

	rc = hyp_smp_start(mpidr);
	t_check(rc == 0, "CPU_ON for cpu1 (%#llx) failed (%d)", mpidr, rc);
	t_check(hyp_smp_info(1, &info) == 0, "cpu1 has no vCPU row");
	t_check(info.started, "cpu1's row does not say started");
	t_check(!info.failed, "cpu1's bring-up failed");
	t_check(info.mpidr == mpidr, "cpu1's row holds affinity %#llx, not %#llx",
		info.mpidr, mpidr);

	return 0;
}

/*
 * The heartbeat a core bumps under smp_lock is the counter CPU0 reads back out
 * of memory: it has to be non-zero, and once the core has parked it is final -
 * two reads have to agree, which is what a torn or lost update would break.
 */
static int __hyp t_smp_shared_counter(void)
{
	struct hyp_vcpu_info info, again;
	u32 i;
	int rc;

	rc = hyp_smp_info(1, &info);
	t_check(rc == 0, "cpu1 has no vCPU row");
	if (rc)
		return 0;

	for (i = 0; i < HYPTEST_WAIT_SPINS; i++) {
		hyp_smp_info(1, &info);
		if (info.parked || info.failed)
			break;
	}

	t_check(!info.failed, "cpu1's bring-up failed");
	t_check(info.parked, "cpu1 never parked, %u heartbeats", info.heartbeats);
	t_check(info.heartbeats > 0, "cpu1 parked with %u heartbeats",
		info.heartbeats);
	hyp_smp_info(1, &again);
	t_check(again.heartbeats == info.heartbeats,
		"cpu1's counter reads %u and then %u", info.heartbeats,
		again.heartbeats);

	return 0;
}

/*
 * The started core reaches EL1 and announces itself through its HVCs, which
 * hyp_smp_hvc() records in the same row CPU0 reads: running set, no failure.
 */
static int __hyp t_smp_secondary_runs(void)
{
	struct hyp_vcpu_info info;
	u32 i;

	for (i = 0; i < HYPTEST_WAIT_SPINS; i++) {
		if (hyp_smp_info(1, &info))
			break;
		if (info.running || info.failed)
			break;
	}

	t_check(hyp_smp_info(1, &info) == 0, "cpu1 has no vCPU row");
	t_check(!info.failed, "cpu1's bring-up failed");
	t_check(info.running, "cpu1 never reported in from EL1");

	return 0;
}

/*
 * Both cores increment one counter under one spinlock.  A lost update shows up
 * as a count below 2N: the lock is the only thing between them, and the count
 * is read from memory rather than from either core's cache.
 */
static int __hyp t_lock_spin_two_core(void)
{
	struct hyp_vcpu_info info;
	u32 spins, counter;
	int i, rc;

	t_publish(test_counter, 0);
	rc = t_start_core(2, HYPTEST_JOB_SPIN);
	t_check(rc == 0, "cpu2 would not start (%d)", rc);
	if (rc)
		return 0;

	for (i = 0; i < HYPTEST_ITERS; i++) {
		spin_lock(&two_core_spin);
		test_counter = t_shared(test_counter) + 1;
		hyp_clean_range(&test_counter, sizeof(test_counter));
		spin_unlock(&two_core_spin);
	}

	spins = t_wait_for(&test_done, HYPTEST_JOB_SPIN);
	t_check(spins, "cpu2 never finished its %d increments", HYPTEST_ITERS);
	counter = t_shared(test_counter);
	t_check(counter == 2 * HYPTEST_ITERS,
		"the shared counter is %u, not 2 x %d", counter, HYPTEST_ITERS);
	t_check(hyp_smp_info(2, &info) == 0 && info.started,
		"cpu2 is not a started vCPU");

	return 0;
}

/*
 * CPU0 holds the mutex, cpu3 tries it and must fail, then blocks in
 * mutex_lock() until CPU0 lets go.  test_seq is the witness: CPU0 writes 1
 * before unlocking, and only a core that acquired after that can read 1 and
 * publish 2 - so a 2 in the word proves the handover, not merely that both
 * cores touched the mutex.
 */
static int __hyp t_lock_mutex_contention(void)
{
	u32 spins, counter;
	int rc;

	t_publish(test_seq, 0);
	t_publish(test_counter, 0);
	t_publish(test_trylock_failed, 0);

	mutex_lock(&test_handover);
	rc = t_start_core(3, HYPTEST_JOB_MUTEX);
	if (rc) {
		t_check(rc == 0, "cpu3 would not start (%d)", rc);
		mutex_unlock(&test_handover);
		return 0;
	}

	/* Keep holding it until the other core has tried it and failed. */
	spins = t_wait_for(&test_trylock_failed, 1);
	t_check(spins, "cpu3 never tried the mutex cpu0 was holding");
	t_publish(test_seq, 1);
	mutex_unlock(&test_handover);

	spins = t_wait_for(&test_seq, 2);
	t_check(spins, "cpu3 never took the mutex after cpu0 released it");
	counter = t_shared(test_counter);
	t_check(counter == 1, "the mutex let %u cores into the section, not 1",
		counter);

	return 0;
}

/*
 * Two cores through the locked hyp_gic_*() wrappers at once.  Each core's own
 * list register still holds exactly what it wrote through the wrapper, the
 * shared counter they both bumped under the spinlock has no lost update, and
 * the console interrupt is still armed afterwards - the distributor state they
 * both reached through gic.lock survived.
 */
static int __hyp t_lock_gic_wrappers(void)
{
	u64 lr = LR_PENDING | LR_PRIORITY(HYP_IRQ_PRIO) | LR_VINTID(HYPTEST_VINTID);
	u32 spins, counter, readback;
	int i, rc;

	t_publish(test_counter, 0);
	t_publish(test_lr_readback, 0);
	rc = t_start_core(4, HYPTEST_JOB_GIC);
	t_check(rc == 0, "cpu4 would not start (%d)", rc);
	if (rc)
		return 0;

	for (i = 0; i < HYPTEST_ITERS; i++) {
		hyp_gic_write_lr(0, lr);
		hyp_gic_sync();
		spin_lock(&two_core_spin);
		test_counter = t_shared(test_counter) + 1;
		hyp_clean_range(&test_counter, sizeof(test_counter));
		spin_unlock(&two_core_spin);
	}

	spins = t_wait_for(&test_done, HYPTEST_JOB_GIC);
	t_check(spins, "cpu4 never finished its %d passes", HYPTEST_ITERS);
	counter = t_shared(test_counter);
	t_check(counter == 2 * HYPTEST_ITERS,
		"the shared counter is %u, not 2 x %d", counter, HYPTEST_ITERS);
	t_check(hyp_gic_read_lr(0) == lr,
		"cpu0's list register 0 no longer holds what it wrote");
	hyp_gic_write_lr(0, 0);
	hyp_gic_sync();
	readback = t_shared(test_lr_readback);
	t_check(readback == (u32)lr,
		"cpu4 read %#x from its list register 0, not %#x", readback, (u32)lr);
	t_check(hyp_gic_console_irq() >= 32, "the console irq is no longer known");

	return 0;
}

/*
 * Both cores append fixed-size records to one array while holding the console
 * lock.  A torn write shows up as a bad checksum or a duplicated (cpu, seq)
 * pair, not as a garbled line of console output.
 */
static int __hyp t_smp_console_serialised(void)
{
	int i, j, n = (int)ARRAY_SIZE(test_records), intact = 0, rc;
	u32 idx, spins;

	t_publish(test_record_next, 0);
	rc = t_start_core(5, HYPTEST_JOB_CONSOLE);
	t_check(rc == 0, "cpu5 would not start (%d)", rc);
	if (rc)
		return 0;

	for (i = 0; i < HYPTEST_ITERS; i++) {
		hyp_smp_console_lock();
		idx = t_shared(test_record_next);
		if (idx < (u32)n) {
			test_records[idx].magic = HYPTEST_RECORD_MAGIC;
			test_records[idx].cpu = 0;
			test_records[idx].seq = i;
			test_records[idx].sum = HYPTEST_RECORD_MAGIC ^ i;
			hyp_clean_range(&test_records[idx], sizeof(test_records[idx]));
		}
		t_publish(test_record_next, idx + 1);
		hyp_smp_console_unlock();
	}

	spins = t_wait_for(&test_done, HYPTEST_JOB_CONSOLE);
	t_check(spins, "cpu5 never finished appending its records");
	idx = t_shared(test_record_next);
	t_check(idx == 2 * HYPTEST_ITERS, "%u records on the array, not 2 x %d",
		idx, HYPTEST_ITERS);

	hyp_clean_range(test_records, sizeof(test_records));
	for (i = 0; i < n; i++) {
		struct hyp_test_record *rec = &test_records[i];

		if (!rec->magic)
			continue;
		intact++;
		t_check(rec->magic == HYPTEST_RECORD_MAGIC &&
			rec->sum == (rec->magic ^ rec->cpu ^ rec->seq),
			"record %d is torn: magic %#x cpu %u seq %u sum %#x", i,
			rec->magic, rec->cpu, rec->seq, rec->sum);
		t_check(rec->cpu == 0 || rec->cpu == 5,
			"record %d claims cpu%u, which took no part", i, rec->cpu);
		for (j = i + 1; j < n; j++)
			t_check(rec->cpu != test_records[j].cpu ||
				rec->seq != test_records[j].seq,
				"records %d and %d both claim cpu%u seq %u", i, j,
				rec->cpu, rec->seq);
	}
	t_check(intact == 2 * HYPTEST_ITERS, "%d records filled, not 2 x %d",
		intact, HYPTEST_ITERS);

	return 0;
}

/*
 * hyp_guest_write()/hyp_guest_read(), the debugger's window into guest memory: a buffer
 * that straddles a page comes back as written, and what was written is in memory.  The
 * guest MMU is off here, so the "guest virtual" address is the physical one.
 */
static __hyp_data u8 t_copy_page[2 * SZ_4K] __aligned(SZ_4K);

static int __hyp t_mmio_guest_copy(void)
{
	u8 *at = t_copy_page + SZ_4K - 40;
	u8 want[100], got[100];
	int rc;

	for (int i = 0; i < sizeof(want); i++)
		want[i] = 0xa0 ^ i;

	rc = hyp_guest_write((u64)at, want, sizeof(want));
	t_check(!rc, "hyp_guest_write() across a page boundary returned %d", rc);
	t_check(!memcmp(at, want, sizeof(want)), "the written bytes are not in memory");

	memset(got, 0, sizeof(got));
	rc = hyp_guest_read((u64)at, got, sizeof(got));
	t_check(!rc, "hyp_guest_read() across a page boundary returned %d", rc);
	t_check(!memcmp(got, want, sizeof(got)), "the bytes read back differ from the ones written");

	return 0;
}

#if IS_ENABLED(CONFIG_HYP_USB)
/*
 * hyp/net.c answers frames from the host.  Frames go to a capture buffer instead of the USB
 * function; the host is 192.168.77.1 / fe80::1 at 02:68:79:70:00:01.
 */
static __hyp_data u8 t_net_out[1600];
static __hyp_data u32 t_net_out_len, t_net_out_count;
static __hyp_data bool t_busy;		/* the capture refuses frames, like a busy USB function */

static int __hyp t_net_capture(const void *frame, u32 len)
{
	if (t_busy)
		return -EBUSY;

	memcpy(t_net_out, frame, min_t(u32, len, sizeof(t_net_out)));
	t_net_out_len = len;
	t_net_out_count++;

	return 0;
}

static const u8 __hyp_rodata t_host_mac[6] = { 0x02, 0x68, 0x79, 0x70, 0x00, 0x01 };
static const u8 __hyp_rodata t_host_ip4[4] = { 192, 168, 77, 1 };
static const u8 __hyp_rodata t_host_ip6[16] = { 0xfe, 0x80, [15] = 1 };

static void __hyp t_net_begin(void)
{
	t_net_out_count = 0;
	t_net_out_len = 0;
	hyp_net_set_tx(t_net_capture);
	hyp_net_link(true);
}

static void __hyp t_net_end(void)
{
	hyp_net_link(false);
	hyp_net_set_tx(NULL);
}

/* The Internet checksum of @p over @len bytes, starting from the partial sum @sum: 0 when valid */
static u16 __hyp t_csum(u32 sum, const u8 *p, u32 len)
{
	for (; len > 1; len -= 2, p += 2)
		sum += (p[0] << 8) | p[1];
	if (len)
		sum += p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return ~sum & 0xffff;
}

/* ICMPv6 over IPv6: the pseudo header's contribution, then the message */
static u16 __hyp t_csum6(const u8 *src, const u8 *dst, const u8 *msg, u32 len)
{
	u32 sum = 0;

	for (int i = 0; i < 16; i += 2)
		sum += ((src[i] << 8) | src[i + 1]) + ((dst[i] << 8) | dst[i + 1]);
	sum += len + 58;

	return t_csum(sum, msg, len);
}

static void __hyp t_eth(u8 *f, u16 type)
{
	u8 ours[6];

	memcpy(ours, hyp_net_mac, 6);
	memcpy(f, ours, 6);
	memcpy(f + 6, t_host_mac, 6);
	f[12] = type >> 8;
	f[13] = type;
}

static int __hyp t_net_arp(void)
{
	u8 f[60] = { 0 }, ours[4];
	u8 *a = f + 14;

	hyp_net_our_ip(false, ours);
	t_eth(f, 0x0806);
	a[1] = 1;				/* Ethernet */
	a[2] = 0x08;				/* IPv4 */
	a[4] = 6;
	a[5] = 4;
	a[7] = 1;				/* request */
	memcpy(a + 8, t_host_mac, 6);
	memcpy(a + 14, t_host_ip4, 4);
	memcpy(a + 24, ours, 4);

	t_net_begin();
	hyp_net_rx(f, sizeof(f));
	t_check(t_net_out_count == 1, "a request for our address drew %u replies", t_net_out_count);
	if (t_net_out_count) {
		const u8 *r = t_net_out + 14;

		t_check(!memcmp(t_net_out, t_host_mac, 6), "reply not addressed to the asker");
		t_check(r[7] == 2, "reply carries ARP opcode %u", r[7]);
		t_check(!memcmp(r + 8, hyp_net_mac, 6), "sender hardware address is not ours");
		t_check(!memcmp(r + 14, ours, 4), "sender protocol address is not ours");
		t_check(!memcmp(r + 18, t_host_mac, 6) && !memcmp(r + 24, t_host_ip4, 4),
			"target is not the asker");
	}

	t_net_out_count = 0;
	memcpy(a + 24, t_host_ip4, 4);			/* someone else's address */
	hyp_net_rx(f, sizeof(f));
	t_check(!t_net_out_count, "a request for another address was answered");
	t_net_end();

	return 0;
}

static int __hyp t_net_icmp_echo(void)
{
	u8 f[14 + 20 + 12] = { 0 }, ours[4];
	u8 *ip = f + 14, *icmp = ip + 20;

	hyp_net_our_ip(false, ours);
	t_eth(f, 0x0800);
	ip[0] = 0x45;
	ip[2] = 0;
	ip[3] = 32;
	ip[8] = 64;
	ip[9] = 1;
	memcpy(ip + 12, t_host_ip4, 4);
	memcpy(ip + 16, ours, 4);
	ip[10] = t_csum(0, ip, 20) >> 8;
	ip[11] = t_csum(0, ip, 20);
	icmp[0] = 8;					/* echo request */
	icmp[4] = 0x12;
	icmp[5] = 0x34;
	memcpy(icmp + 8, "ping", 4);
	icmp[2] = t_csum(0, icmp, 12) >> 8;
	icmp[3] = t_csum(0, icmp, 12);

	t_net_begin();
	hyp_net_rx(f, sizeof(f));
	t_check(t_net_out_count == 1, "an echo request drew %u replies", t_net_out_count);
	if (t_net_out_count) {
		const u8 *rip = t_net_out + 14, *ricmp = rip + 20;

		t_check(t_net_out_len == sizeof(f), "reply is %u bytes, the request %u",
			t_net_out_len, (u32)sizeof(f));
		t_check(!t_csum(0, rip, 20), "reply IPv4 header checksum is wrong");
		t_check(!memcmp(rip + 12, ours, 4) && !memcmp(rip + 16, t_host_ip4, 4),
			"reply addresses are not ours -> the asker");
		t_check(ricmp[0] == 0, "reply is ICMP type %u", ricmp[0]);
		t_check(!t_csum(0, ricmp, 12), "reply ICMP checksum is wrong");
		t_check(ricmp[4] == 0x12 && ricmp[5] == 0x34 && !memcmp(ricmp + 8, "ping", 4),
			"identifier or payload not echoed");
	}

	t_net_out_count = 0;
	icmp[3] ^= 0xff;				/* corrupt the checksum */
	hyp_net_rx(f, sizeof(f));
	t_check(!t_net_out_count, "an echo request with a bad checksum was answered");
	t_net_end();

	return 0;
}

/* A frame of IPv6 + an ICMPv6 message from the host to @dst, checksum filled in; returns its length */
static u32 __hyp t_icmp6_frame(u8 *f, const u8 *dst, const u8 *msg, u32 mlen, const u8 *src)
{
	u8 *ip = f + 14;

	memset(f, 0, 14 + 40);
	t_eth(f, 0x86dd);
	ip[0] = 0x60;
	ip[4] = mlen >> 8;
	ip[5] = mlen;
	ip[6] = 58;
	ip[7] = 255;
	memcpy(ip + 8, src, 16);
	memcpy(ip + 24, dst, 16);
	memcpy(ip + 40, msg, mlen);
	ip[42] = ip[43] = 0;
	ip[42] = t_csum6(src, dst, ip + 40, mlen) >> 8;
	ip[43] = t_csum6(src, dst, ip + 40, mlen);

	return 14 + 40 + mlen;
}

static int __hyp t_net_ndp(void)
{
	u8 f[14 + 40 + 32], ours[16], msg[32] = { 135 };	/* neighbour solicitation */
	static const u8 __hyp_rodata zero[16] = { 0 };
	u32 len;

	hyp_net_our_ip(true, ours);
	memcpy(msg + 8, ours, 16);
	msg[24] = 1;					/* source link-layer address option */
	msg[25] = 1;
	memcpy(msg + 26, t_host_mac, 6);

	t_net_begin();
	len = t_icmp6_frame(f, ours, msg, sizeof(msg), t_host_ip6);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "a solicitation for our address drew %u answers", t_net_out_count);
	if (t_net_out_count) {
		const u8 *ip = t_net_out + 14, *na = ip + 40;

		t_check(na[0] == 136, "answer is ICMPv6 type %u, not a neighbour advertisement", na[0]);
		t_check(na[4] == 0x60, "advertisement flags are %#x, not solicited+override", na[4]);
		t_check(!memcmp(na + 8, ours, 16), "advertised target is not our address");
		t_check(na[24] == 2 && na[25] == 1 && !memcmp(na + 26, hyp_net_mac, 6),
			"target link-layer address option is not our MAC");
		t_check(!memcmp(ip + 24, t_host_ip6, 16), "not sent to the solicitor");
		t_check(!t_csum6(ip + 8, ip + 24, na, 32), "advertisement checksum is wrong");
	}

	/* duplicate address detection (unspecified source) is not answered */
	t_net_out_count = 0;
	len = t_icmp6_frame(f, ours, msg, sizeof(msg), zero);
	hyp_net_rx(f, len);
	t_check(!t_net_out_count, "a duplicate address detection probe was answered");
	t_net_end();

	return 0;
}

static int __hyp t_net_icmp6_echo(void)
{
	u8 f[14 + 40 + 16], ours[16], msg[16] = { 128 };	/* echo request */
	u32 len;

	hyp_net_our_ip(true, ours);
	msg[4] = 0xbe;
	msg[5] = 0xef;
	memcpy(msg + 8, "echo6!!!", 8);

	t_net_begin();
	len = t_icmp6_frame(f, ours, msg, sizeof(msg), t_host_ip6);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "an ICMPv6 echo request drew %u replies", t_net_out_count);
	if (t_net_out_count) {
		const u8 *ip = t_net_out + 14, *rep = ip + 40;

		t_check(rep[0] == 129, "reply is ICMPv6 type %u", rep[0]);
		t_check(rep[4] == 0xbe && rep[5] == 0xef && !memcmp(rep + 8, "echo6!!!", 8),
			"identifier or payload not echoed");
		t_check(!memcmp(ip + 8, ours, 16) && !memcmp(ip + 24, t_host_ip6, 16),
			"reply addresses are not ours -> the asker");
		t_check(!t_csum6(ip + 8, ip + 24, rep, 16), "reply checksum is wrong");
	}
	t_net_end();

	return 0;
}

/*
 * A DHCP client message from the host: @type (1 discover, 3 request), with the options
 * the caller asks for.  Returns the frame length.
 */
static u32 __hyp t_dhcp_frame(u8 *f, u8 type, const u8 *requested, const u8 *server, const u8 *ciaddr)
{
	u8 *ip = f + 14, *udp = ip + 20, *b = udp + 8, *o;
	u32 len;

	memset(f, 0, 14 + 20 + 8 + 300);
	memset(f, 0xff, 6);				/* Ethernet broadcast */
	memcpy(f + 6, t_host_mac, 6);
	f[12] = 0x08;
	b[0] = 1;					/* BOOTREQUEST */
	b[1] = 1;
	b[2] = 6;
	b[4] = 0xde;					/* xid */
	b[5] = 0xad;
	b[6] = 0xbe;
	b[7] = 0xef;
	b[10] = 0x80;					/* broadcast flag */
	if (ciaddr)
		memcpy(b + 12, ciaddr, 4);
	memcpy(b + 28, t_host_mac, 6);
	b[236] = 0x63;					/* magic cookie */
	b[237] = 0x82;
	b[238] = 0x53;
	b[239] = 0x63;
	o = b + 240;
	*o++ = 53;
	*o++ = 1;
	*o++ = type;
	if (requested) {
		*o++ = 50;
		*o++ = 4;
		memcpy(o, requested, 4);
		o += 4;
	}
	if (server) {
		*o++ = 54;
		*o++ = 4;
		memcpy(o, server, 4);
		o += 4;
	}
	*o++ = 255;

	len = o - udp;
	udp[0] = 0;
	udp[1] = 68;
	udp[2] = 0;
	udp[3] = 67;
	udp[4] = len >> 8;
	udp[5] = len;
	ip[0] = 0x45;
	ip[2] = (20 + len) >> 8;
	ip[3] = 20 + len;
	ip[8] = 64;
	ip[9] = 17;
	memset(ip + 16, 0xff, 4);			/* from 0.0.0.0 to 255.255.255.255 */
	ip[10] = t_csum(0, ip, 20) >> 8;
	ip[11] = t_csum(0, ip, 20);

	return 14 + 20 + len;
}

/* The option @code of the BOOTP reply in the captured frame, or NULL */
static const u8 __hyp *t_dhcp_opt(u8 code)
{
	const u8 *o = t_net_out + 14 + 20 + 8 + 240;

	while (o < t_net_out + t_net_out_len && *o != 255) {
		if (*o == code)
			return o + 2;
		o += 2 + o[1];
	}

	return NULL;
}

static int __hyp t_net_dhcp(void)
{
	u8 f[400], ours[4];
	const u8 *o, *bootp = t_net_out + 14 + 20 + 8;
	u32 len;

	hyp_net_our_ip(false, ours);

	/* discover -> offer of 192.168.77.1/24 from 192.168.77.2, no router */
	t_net_begin();
	len = t_dhcp_frame(f, 1, NULL, NULL, NULL);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "a discover drew %u replies", t_net_out_count);
	if (t_net_out_count) {
		const u8 *ip = t_net_out + 14, *udp = ip + 20;

		t_check(!memcmp(t_net_out, "\xff\xff\xff\xff\xff\xff", 6), "offer is not an Ethernet broadcast");
		t_check(!t_csum(0, ip, 20), "offer IPv4 header checksum is wrong");
		t_check(!memcmp(ip + 12, ours, 4) && !memcmp(ip + 16, "\xff\xff\xff\xff", 4),
			"offer is not from us to 255.255.255.255");
		t_check(udp[1] == 67 && udp[3] == 68, "offer ports are %u -> %u", udp[1], udp[3]);
		t_check(bootp[0] == 2 && !memcmp(bootp + 4, "\xde\xad\xbe\xef", 4), "not a reply to our xid");
		t_check(!memcmp(bootp + 16, t_host_ip4, 4), "yiaddr is not 192.168.77.1");
		t_check(!memcmp(bootp + 28, t_host_mac, 6), "chaddr not echoed");
		o = t_dhcp_opt(53);
		t_check(o && o[0] == 2, "message type is %u, not an offer", o ? o[0] : 0);
		o = t_dhcp_opt(54);
		t_check(o && !memcmp(o, ours, 4), "server identifier is not us");
		o = t_dhcp_opt(1);
		t_check(o && !memcmp(o, "\xff\xff\xff\x00", 4), "subnet mask is not /24");
		o = t_dhcp_opt(51);
		t_check(o && o[0] == 0 && o[1] == 1 && o[2] == 0x51 && o[3] == 0x80, "lease is not a day");
		t_check(!t_dhcp_opt(3), "the offer carries a router: a default route over the debug link");
	}

	/* request for the offered address, naming us -> ack */
	t_net_out_count = 0;
	len = t_dhcp_frame(f, 3, t_host_ip4, ours, NULL);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "a request drew %u replies", t_net_out_count);
	o = t_dhcp_opt(53);
	t_check(t_net_out_count && o && o[0] == 5, "message type is %u, not an ack", o ? o[0] : 0);

	/* a renewal (ciaddr set, no options) is answered to the host directly */
	t_net_out_count = 0;
	len = t_dhcp_frame(f, 3, NULL, NULL, t_host_ip4);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "a renewal drew %u replies", t_net_out_count);
	if (t_net_out_count) {
		o = t_dhcp_opt(53);
		t_check(o && o[0] == 5, "renewal message type is %u, not an ack", o ? o[0] : 0);
		t_check(!memcmp(t_net_out, t_host_mac, 6), "renewal ack is not unicast to the host");
		t_check(!memcmp(t_net_out + 14 + 16, t_host_ip4, 4), "renewal ack is not sent to ciaddr");
	}

	/* a request for another address -> nak */
	t_net_out_count = 0;
	len = t_dhcp_frame(f, 3, "\xc0\xa8\x4d\x09", NULL, NULL);
	hyp_net_rx(f, len);
	t_check(t_net_out_count == 1, "a request for a wrong address drew %u replies", t_net_out_count);
	o = t_dhcp_opt(53);
	t_check(t_net_out_count && o && o[0] == 6, "message type is %u, not a nak", o ? o[0] : 0);

	/* a request that picked another server is none of our business */
	t_net_out_count = 0;
	len = t_dhcp_frame(f, 3, t_host_ip4, "\xc0\xa8\x01\x01", NULL);
	hyp_net_rx(f, len);
	t_check(!t_net_out_count, "a request addressed to another server was answered");
	t_net_end();

	return 0;
}

/*
 * The guest's virtual devices (hyp/vdev.c), driven the way a guest driver would: through the
 * MMIO window, with virtqueues in ordinary memory.
 */
static __hyp_data u8 t_vq_mem[2][4096] __aligned(4096);	/* rx, tx: desc @0, avail @0x200, used @0x400 */
static __hyp_data u8 t_vq_buf[4][2048] __aligned(64);
static __hyp_data int t_irqs, t_irq_last;

static void __hyp t_vspi(int intid)
{
	t_irqs++;
	t_irq_last = intid;
}

static u32 __hyp t_vr(u64 base, u32 off)
{
	u64 v = 0;

	hyp_vdev_test_access(base + off, false, 4, &v);

	return v;
}

static void __hyp t_vw(u64 base, u32 off, u32 val)
{
	u64 v = val;

	hyp_vdev_test_access(base + off, true, 4, &v);
}

static int __hyp t_vdev_regs(void)
{
	u64 nb = HYP_VIRTIO_NET_BASE, v = 0;
	u8 mac[6] = HYP_GUEST_MAC;
	u64 feat;

	t_check(t_vr(nb, 0x000) == 0x74726976, "magic is %#x", t_vr(nb, 0x000));
	t_check(t_vr(nb, 0x004) == 2, "version is %u, not 2", t_vr(nb, 0x004));
	t_check(t_vr(nb, 0x008) == 1, "device id is %u, not 1 (net)", t_vr(nb, 0x008));

	t_vw(nb, 0x014, 0);
	feat = t_vr(nb, 0x010);
	t_vw(nb, 0x014, 1);
	feat |= (u64)t_vr(nb, 0x010) << 32;
	t_check(feat & BIT_ULL(32), "VIRTIO_F_VERSION_1 is not offered (%#llx)", feat);
	t_check(feat & BIT_ULL(33), "VIRTIO_F_ACCESS_PLATFORM is not offered (%#llx)", feat);
	t_check((feat & BIT_ULL(5)) && (feat & BIT_ULL(16)), "MAC and STATUS are not offered (%#llx)", feat);

	t_vw(nb, 0x030, 0);
	t_check(t_vr(nb, 0x034) >= 64, "queue 0 holds at most %u buffers", t_vr(nb, 0x034));
	t_vw(nb, 0x030, 2);
	t_check(t_vr(nb, 0x034) == 0, "a third queue exists (max %u)", t_vr(nb, 0x034));

	for (int i = 0; i < 6; i++) {
		v = 0;
		hyp_vdev_test_access(nb + 0x100 + i, false, 1, &v);
		t_check(v == mac[i], "config byte %d is %#llx, not %#x", i, v, mac[i]);
	}
	v = 0;
	hyp_vdev_test_access(nb + 0x106, false, 2, &v);
	t_check(v == 1, "the link status is %#llx, not up", v);

	t_vw(nb, 0x070, 0x0b);			/* ACKNOWLEDGE | DRIVER | FEATURES_OK */
	t_check(t_vr(nb, 0x070) == 0x0b, "status reads back %#x", t_vr(nb, 0x070));
	t_vw(nb, 0x070, 0);
	t_check(t_vr(nb, 0x070) == 0, "a reset leaves status %#x", t_vr(nb, 0x070));

	return 0;
}

static void __hyp t_vq_setup(u64 nb, u32 q, u8 *mem)
{
	t_vw(nb, 0x030, q);
	t_vw(nb, 0x038, 8);
	t_vw(nb, 0x080, (u32)(u64)mem);
	t_vw(nb, 0x084, (u64)mem >> 32);
	t_vw(nb, 0x090, (u32)((u64)mem + 0x200));
	t_vw(nb, 0x094, ((u64)mem + 0x200) >> 32);
	t_vw(nb, 0x0a0, (u32)((u64)mem + 0x400));
	t_vw(nb, 0x0a4, ((u64)mem + 0x400) >> 32);
	t_vw(nb, 0x044, 1);
}

/* descriptor @i of a ring in @mem, and make it the next avail entry */
static void __hyp t_vq_post(u8 *mem, u16 i, u64 addr, u32 len, u16 flags, u16 next, u16 slot)
{
	u8 *d = mem + 16 * i;
	u16 *avail = (u16 *)(mem + 0x200);

	*(u64 *)d = addr;
	*(u32 *)(d + 8) = len;
	*(u16 *)(d + 12) = flags;
	*(u16 *)(d + 14) = next;
	avail[2 + slot] = i;
}

static void __hyp t_vq_publish(u8 *mem, u16 idx)
{
	*(volatile u16 *)(mem + 0x202) = idx;
	asm volatile("dsb sy" ::: "memory");
}

static u16 __hyp t_vq_used(u8 *mem)
{
	asm volatile("dsb sy" ::: "memory");

	return *(volatile u16 *)(mem + 0x402);
}

/* A frame on the wire for the guest; (is) its MAC as destination */
static u32 __hyp t_gframe(u8 *f, const u8 *dst, u16 type, const char *payload)
{
	u32 n = strlen(payload);

	memcpy(f, dst, 6);
	memcpy(f + 6, t_host_mac, 6);
	f[12] = type >> 8;
	f[13] = type;
	memcpy(f + 14, payload, n);

	return 14 + n;
}

static int __hyp t_vdev_bridge(void)
{
	u64 nb = HYP_VIRTIO_NET_BASE;
	u8 gmac[6] = HYP_GUEST_MAC, f[128], *rx = t_vq_mem[0], *tx = t_vq_mem[1];
	u32 len;

	memset(t_vq_mem, 0, sizeof(t_vq_mem));
	memset(t_vq_buf, 0, sizeof(t_vq_buf));
	t_irqs = 0;
	hyp_vdev_test_irq(t_vspi);
	t_net_begin();

	/* what a driver does: reset, ACK, DRIVER, features, FEATURES_OK, queues, DRIVER_OK */
	t_vw(nb, 0x070, 0);
	t_vw(nb, 0x070, 3);
	t_vw(nb, 0x024, 1);
	t_vw(nb, 0x020, 3);
	t_vw(nb, 0x070, 0x0b);
	t_vq_setup(nb, 0, rx);
	t_vq_setup(nb, 1, tx);
	t_vw(nb, 0x070, 0x0f);

	/* host -> guest: a frame for the guest's MAC lands in its first receive buffer */
	t_vq_post(rx, 0, (u64)t_vq_buf[0], 2048, 2 /* WRITE */, 0, 0);
	t_vq_post(rx, 1, (u64)t_vq_buf[1], 2048, 2, 0, 1);
	t_vq_publish(rx, 2);
	len = t_gframe(f, gmac, 0x0800, "hello guest");
	hyp_net_rx(f, len);
	t_check(t_vq_used(rx) == 1, "the receive queue's used index is %u after one frame", t_vq_used(rx));
	t_check(*(u32 *)(rx + 0x404) == 0 && *(u32 *)(rx + 0x408) == 12 + len,
		"the used entry is id %u, %u bytes (want 0, %u)", *(u32 *)(rx + 0x404),
		*(u32 *)(rx + 0x408), 12 + len);
	t_check(!memcmp(t_vq_buf[0] + 12, f, len), "the frame is not behind the 12-byte header");
	t_check(t_vq_buf[0][10] == 1, "num_buffers is %u, not 1", t_vq_buf[0][10]);
	t_check(t_irqs == 1 && t_irq_last == HYP_VSPI_NET, "%d interrupts, last %d", t_irqs, t_irq_last);
	t_check(t_vr(nb, 0x060) == 1, "interrupt status is %#x", t_vr(nb, 0x060));
	t_vw(nb, 0x064, 1);
	t_check(t_vr(nb, 0x060) == 0, "ACK left interrupt status %#x", t_vr(nb, 0x060));
	t_check(!t_net_out_count, "a frame for the guest went out on the link");

	/* a frame for the hypervisor's MAC is not the guest's */
	t_net_out_count = 0;
	len = t_gframe(f, hyp_net_mac, 0x0800, "not for the guest");
	hyp_net_rx(f, len);
	t_check(t_vq_used(rx) == 1, "a frame for the hypervisor reached the guest");

	/* guest -> link: a broadcast ARP request for the hypervisor's address goes out, and the stack's
	 * reply comes back into the guest's second receive buffer */
	{
		u8 *a, ours[4];
		static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

		hyp_net_our_ip(false, ours);
		memset(t_vq_buf[2], 0, 12);		/* virtio_net_hdr */
		memcpy(t_vq_buf[2] + 12, bc, 6);
		memcpy(t_vq_buf[2] + 18, gmac, 6);
		t_vq_buf[2][24] = 0x08;
		t_vq_buf[2][25] = 0x06;
		a = t_vq_buf[2] + 26;
		a[1] = 1;
		a[2] = 0x08;
		a[4] = 6;
		a[5] = 4;
		a[7] = 1;				/* request */
		memcpy(a + 8, gmac, 6);
		a[14] = 192; a[15] = 168; a[16] = 77; a[17] = 3;
		memcpy(a + 24, ours, 4);

		t_vq_post(tx, 0, (u64)t_vq_buf[2], 12, 1 /* NEXT */, 1, 0);	/* header, then the frame */
		t_vq_post(tx, 1, (u64)t_vq_buf[2] + 12, 42, 0, 0, 0);
		*(u16 *)(tx + 0x204) = 0;
		t_vq_publish(tx, 1);
		t_net_out_count = 0;
		t_vw(nb, 0x050, 1);
		t_check(t_vq_used(tx) == 1, "the transmit queue's used index is %u", t_vq_used(tx));
		t_check(t_net_out_count == 1 && !memcmp(t_net_out, bc, 6),
			"the broadcast did not go out on the link (%u frames)", t_net_out_count);
		t_check(t_vq_used(rx) == 2, "the stack's ARP reply did not reach the guest (used %u)",
			t_vq_used(rx));
		t_check(t_vq_buf[1][12 + 12] == 0x08 && t_vq_buf[1][12 + 13] == 0x06 &&
			t_vq_buf[1][12 + 14 + 7] == 2, "the frame in the guest's buffer is not an ARP reply");
		t_check(!memcmp(t_vq_buf[1] + 12, gmac, 6), "the reply is not addressed to the guest");
	}

	/* a frame the link cannot take yet stays in the queue until the function is free */
	{
		static const u8 hmac[6] = { 0x02, 0x68, 0x79, 0x70, 0x00, 0x01 };

		memset(t_vq_buf[3], 0, 12);
		memcpy(t_vq_buf[3] + 12, hmac, 6);
		memcpy(t_vq_buf[3] + 18, gmac, 6);
		t_vq_buf[3][24] = 0x08;
		memcpy(t_vq_buf[3] + 26, "to the host, later", 18);
		t_vq_post(tx, 2, (u64)t_vq_buf[3], 44, 0, 0, 1);	/* header and frame in one buffer */
		t_vq_publish(tx, 2);

		t_busy = true;
		t_net_out_count = 0;
		t_vw(nb, 0x050, 1);
		t_check(t_vq_used(tx) == 1 && !t_net_out_count, "a frame was taken while the link was busy");
		t_busy = false;
		hyp_net_tx_done();
		t_check(t_vq_used(tx) == 2 && t_net_out_count == 1 && !memcmp(t_net_out, hmac, 6),
			"the held-back frame did not go out when the link was free (used %u, out %u)",
			t_vq_used(tx), t_net_out_count);
	}

	t_vw(nb, 0x070, 0);
	hyp_vdev_test_irq(NULL);
	t_net_end();

	return 0;
}

static int __hyp t_vdev_uart(void)
{
	u64 ub = HYP_PL011_BASE, v;
	u8 out[32] = { 0 };
	u32 n;

	t_irqs = 0;
	hyp_vdev_test_irq(t_vspi);
	hyp_vdev_test_console(out, sizeof(out), NULL, 0);	/* drop what the guest said before */

	t_check(t_vr(ub, 0xfe0) == 0x11 && t_vr(ub, 0xfe4) == 0x10 && (t_vr(ub, 0xfe8) & 0xf) == 4,
		"the PL011 peripheral id reads %#x %#x %#x", t_vr(ub, 0xfe0), t_vr(ub, 0xfe4), t_vr(ub, 0xfe8));
	t_check(t_vr(ub, 0xff0) == 0x0d && t_vr(ub, 0xff4) == 0xf0 && t_vr(ub, 0xff8) == 0x05 &&
		t_vr(ub, 0xffc) == 0xb1, "the PrimeCell id is wrong");
	t_check((t_vr(ub, 0x018) & 0xb0) == 0x90, "flags %#x: not (TX empty, RX empty, TX not full)",
		t_vr(ub, 0x018));

	/* output: bytes written to DR come out of the console ring, in order */
	t_vw(ub, 0x038, 0);
	for (const char *s = "boot ok\n"; *s; s++)
		t_vw(ub, 0x000, *s);
	n = hyp_vdev_test_console(out, sizeof(out), NULL, 0);
	t_check(n == 8 && !memcmp(out, "boot ok\n", 8), "the console output is %u bytes: '%s'", n, out);
	t_check(!t_irqs, "an interrupt with everything masked");

	/* the transmit interrupt: enabling it with the FIFO empty fires; clearing it does not */
	t_vw(ub, 0x038, BIT(5));
	t_check(t_irqs == 1 && t_irq_last == HYP_VSPI_UART, "enabling TXIM raised %d interrupts, last %d",
		t_irqs, t_irq_last);
	t_check((t_vr(ub, 0x040) & BIT(5)), "masked interrupt status lacks the transmit bit");
	t_vw(ub, 0x044, BIT(5));
	t_check(!(t_vr(ub, 0x040) & BIT(5)), "the transmit interrupt did not clear");
	t_vw(ub, 0x038, 0);

	/* input: what a telnet client types, past its escapes, one Enter for CR LF */
	hyp_vdev_test_console(out, sizeof(out), (const u8 *)"ab\xff\xfb\x01\r\nc", 8);
	t_check(!(t_vr(ub, 0x018) & BIT(4)), "the receive FIFO is empty after input");
	v = 0;
	for (int i = 0; i < 4; i++)
		v = (v << 8) | (t_vr(ub, 0x000) & 0xff);
	t_check(v == 0x61620d63, "the guest read %#llx, not 'ab', CR, 'c'", v);
	t_check(t_vr(ub, 0x018) & BIT(4), "the receive FIFO is not empty after reading it dry");

	hyp_vdev_test_irq(NULL);

	return 0;
}
#endif

/*
 * hyp_phys_write()/hyp_phys_read() on DRAM: odd offsets and lengths across a page boundary
 * come back as written, and the bytes really are in memory.
 */
static int __hyp t_phys_dram(void)
{
	u8 *at = t_copy_page + SZ_4K - 37;
	u8 want[101], got[101];
	int rc;

	for (int i = 0; i < sizeof(want); i++)
		want[i] = 0x5a ^ (i * 7);

	rc = hyp_phys_write((u64)at, want, sizeof(want));
	t_check(!rc, "hyp_phys_write() across a page boundary returned %d", rc);
	t_check(!memcmp(at, want, sizeof(want)), "the written bytes are not in memory");

	memset(got, 0, sizeof(got));
	rc = hyp_phys_read((u64)at, got, sizeof(got));
	t_check(!rc, "hyp_phys_read() across a page boundary returned %d", rc);
	t_check(!memcmp(got, want, sizeof(got)), "the bytes read back differ from the ones written");

	rc = hyp_phys_read(1ULL << 40, got, 4);
	t_check(rc == -EFAULT, "an address beyond EL2's map returned %d, not -EFAULT", rc);

	/* physical 0 is neither DRAM nor in the device tree: refused before the bus is asked */
	rc = hyp_phys_read(0, got, 4);
	t_check(rc == -EFAULT, "a read of physical address 0 returned %d, not -EFAULT", rc);
	rc = hyp_phys_write(0, got, 4);
	t_check(rc == -EFAULT, "a write to physical address 0 returned %d, not -EFAULT", rc);

	return 0;
}

/*
 * The guest's SCM calls for a processor EL2 loads (hyp/pas.c): the call is the OS's PAS driver's
 * ABI - the SMCCC function id in x0, the arguments from x2, the call's status in x0 and its result
 * in x1 - and anything that is not one of EL2's must go on to the secure world untouched.
 */
#define T_SCM(svc, cmd)		(0xc2000000ULL | ((u64)(svc) << 8) | (cmd))	/* fast, SMC64, SiP */

static bool __hyp t_scm(struct pt_regs *regs, u64 fid, u64 a0, u64 a1, u64 a2)
{
	memset(regs, 0, sizeof(*regs));
	regs->regs[0] = fid;
	regs->regs[2] = a0;
	regs->regs[3] = a1;
	regs->regs[4] = a2;

	return hyp_pas_smc(regs);
}

static int __hyp t_pas_smc(void)
{
	struct pt_regs regs;
	/* 32-bit ELF header, little endian, e_entry 0x9df00000 at +0x18 */
	u8 elf[64] = { 0x7f, 'E', 'L', 'F', 1, 1, 1 };
	u8 junk[64] = { 0 };

	elf[0x18] = 0x00;
	elf[0x19] = 0x00;
	elf[0x1a] = 0xf0;
	elf[0x1b] = 0x9d;

	hyp_pas_test_inject(7, 8);

	/* not ours: another service, another owner, an id nobody loads */
	t_check(!t_scm(&regs, T_SCM(3, 7), 7, 0, 0), "a call of another SCM service was taken");
	t_check(!t_scm(&regs, 0xc4000000ULL | (2 << 8) | 7, 7, 0, 0), "a call of another owner was taken");
	t_check(!t_scm(&regs, T_SCM(2, 7), 99, 0, 0), "PAS_IS_SUPPORTED for an id EL2 does not load was taken");

	/* PAS_IS_SUPPORTED: the call went through (x0 = 0), the answer is 1 */
	t_check(t_scm(&regs, T_SCM(2, 7), 7, 0, 0), "PAS_IS_SUPPORTED for EL2's processor was not taken");
	t_check(regs.regs[0] == 0 && regs.regs[1] == 1, "PAS_IS_SUPPORTED answered x0=%#lx x1=%#lx, not 0 and 1",
		regs.regs[0], regs.regs[1]);
	t_check(t_scm(&regs, T_SCM(2, 7), 8, 0, 0) && regs.regs[1] == 1, "its DTB image's id is not supported");

	/* PAS_INIT_IMAGE: the entry point comes from the ELF header the metadata starts with */
	t_check(t_scm(&regs, T_SCM(2, 1), 7, (u64)elf, 0) && regs.regs[0] == 0 && regs.regs[1] == 0,
		"PAS_INIT_IMAGE on an ELF header failed (x0=%#lx x1=%#lx)", regs.regs[0], regs.regs[1]);
	t_check(hyp_pas_test_entry(7) == 0x9df00000ULL, "the entry point is %#llx, not 0x9df00000",
		hyp_pas_test_entry(7));
	t_check(t_scm(&regs, T_SCM(2, 1), 7, (u64)junk, 0) && (long)regs.regs[1] == -EINVAL,
		"PAS_INIT_IMAGE on something that is not an ELF header answered %ld, not -EINVAL", (long)regs.regs[1]);

	/* PAS_MEM_SETUP and PAS_SET_REMOTE_STATE have nothing to do and succeed */
	t_check(t_scm(&regs, T_SCM(2, 2), 7, 0x9df00000ULL, 0x4080000ULL) && regs.regs[1] == 0, "PAS_MEM_SETUP failed");
	t_check(t_scm(&regs, T_SCM(2, 0xa), 1, 7, 0) && regs.regs[1] == 0, "PAS_SET_REMOTE_STATE failed");

	/* a call EL2 has no answer to is refused as not supported, not sent to the secure world */
	t_check(t_scm(&regs, T_SCM(2, 0x21), 7, 0, 0) && regs.regs[0] == (u64)-1,
		"PAS_GET_RSCTABLE answered %#lx, not -1", regs.regs[0]);

	/* MEM_PROT_ASSIGN succeeds for any region, with or without a readable one, and its sibling
	 * calls of the same service are not ours */
	{
		u64 region[2] = { 0x86b00000ULL, 0x4900000ULL };

		t_check(t_scm(&regs, T_SCM(0x0c, 0x16), (u64)region, 16, 0) && regs.regs[0] == 0 && regs.regs[1] == 0,
			"MEM_PROT_ASSIGN did not succeed (x0=%#lx x1=%#lx)", regs.regs[0], regs.regs[1]);
		t_check(t_scm(&regs, T_SCM(0x0c, 0x16), 0, 16, 0) && regs.regs[0] == 0 && regs.regs[1] == 0,
			"MEM_PROT_ASSIGN with an unreadable region did not succeed");
		t_check(!t_scm(&regs, T_SCM(0x0c, 0x18), (u64)region, 16, 0), "another MP call was taken");
	}

	hyp_pas_test_clear();
	t_check(!t_scm(&regs, T_SCM(2, 7), 7, 0, 0), "a call for a processor that is gone was taken");

	return 0;
}

/*
 * A device access the bus refuses must come back as an error and leave the core as it was.  EL2
 * has no stage-1 mapping at 1 << 40, so the load takes a synchronous data abort at EL2 - what a
 * load from a dead register window does - and the probe has to skip it; a word that is there
 * has to read fine right after, which shows the probe did not stay armed.
 */
static int __hyp t_phys_fault(void)
{
	u32 v = 0xdeadbeef, ok = 0;
	int rc;

	t_copy_page[0] = 0x11;
	t_copy_page[1] = 0x22;
	t_copy_page[2] = 0x33;
	t_copy_page[3] = 0x44;

	rc = hyp_phys_probe32(1ULL << 40, &v);
	t_check(rc == -EFAULT, "a load from an unmapped address returned %d, not -EFAULT", rc);
	t_check(v == 0xdeadbeef, "a faulted load stored %#x", v);

	rc = hyp_phys_probe32((u64)t_copy_page, &ok);
	t_check(!rc && ok == 0x44332211, "a load from mapped memory after the fault returned %d, %#x", rc, ok);

	rc = hyp_phys_probe32(1ULL << 41, &v);
	t_check(rc == -EFAULT, "a second faulting load returned %d, not -EFAULT", rc);

	return 0;
}

static const __hyp_rodata struct hyp_test_case cases[] = {
	{ "hyp.resident",		t_hyp_resident },
	{ "hyp.handoff-offset",		t_handoff_offset },
	{ "hyp.hcr-guest-config",	t_hcr_guest_config },
	{ "lock.spin-trylock",		t_lock_spin_trylock },
	{ "lock.irqsave-daif",		t_lock_irqsave_daif },
	{ "lock.mutex-owner",		t_lock_mutex_owner },
	{ "irq.el2-return",		t_irq_el2_return },
	{ "gic.dist-geometry",		t_gic_dist_geometry },
	{ "gic.rdist-affinity",		t_gic_rdist_affinity },
	{ "gic.cpuif-sre",		t_gic_cpuif_sre },
	{ "gic.vgic-cr0",		t_gic_vgic_cr0 },
	{ "vgic.lr-fields",		t_vgic_lr_fields },
	{ "vgic.lr-retire",		t_vgic_lr_retire },
	{ "vgic.sgi1r-self",		t_vgic_sgi1r_self },
	{ "mmio.watch-carve",		t_mmio_watch_carve },
	{ "mmio.replay-store",		t_mmio_replay_store },
	{ "mmio.replay-load",		t_mmio_replay_load },
	{ "mmio.spy-toggle",		t_mmio_spy_toggle },
	{ "mmio.deny-guest",		t_mmio_deny_guest },
	{ "mmio.el2-unaffected",	t_mmio_el2_unaffected },
	{ "smp.smccc-version",		t_smp_smccc_version },
	{ "smp.psci-version",		t_smp_psci_version },
	{ "smp.psci-features",		t_smp_psci_features },
	{ "smp.affinity-table",		t_smp_affinity_table },
	{ "smp.table-geometry",		t_smp_table_geometry },
	{ "smp.cpu-on-accept",		t_smp_cpu_on_accept },
	{ "smp.shared-counter",		t_smp_shared_counter },
	{ "smp.secondary-runs",		t_smp_secondary_runs },
	{ "lock.spin-two-core",		t_lock_spin_two_core },
	{ "lock.mutex-contention",	t_lock_mutex_contention },
	{ "lock.gic-wrappers",		t_lock_gic_wrappers },
	{ "smp.console-serialised",	t_smp_console_serialised },
	{ "gic.irq-to-el2",		t_gic_irq_to_el2 },
	{ "gic.irq-owned",		t_gic_irq_owned },
	{ "vgic.sgi-cross-core",	t_vgic_sgi_cross_core },
	{ "mmio.fault-from-el1",	t_mmio_fault_from_el1 },
	{ "smp.two-core-lock",		t_smp_two_core_lock },
	{ "el1.hvc",			t_el1_hvc },
	{ "gic.irq-to-el1",		t_gic_irq_to_el1 },
	{ "smp.cpu-on-identity",	t_smp_cpu_on_identity },
	{ "mmio.guest-copy",		t_mmio_guest_copy },
	{ "phys.dram-copy",		t_phys_dram },
	{ "phys.fault-survives",	t_phys_fault },
	{ "pas.scm-abi",			t_pas_smc },
#if IS_ENABLED(CONFIG_HYP_USB)
	{ "net.arp-reply",		t_net_arp },
	{ "net.icmp-echo",		t_net_icmp_echo },
	{ "net.ndp-advert",		t_net_ndp },
	{ "net.icmp6-echo",		t_net_icmp6_echo },
	{ "net.dhcp-server",		t_net_dhcp },
	{ "vdev.virtio-regs",		t_vdev_regs },
	{ "vdev.virtio-bridge",		t_vdev_bridge },
	{ "vdev.pl011",			t_vdev_uart },
#endif
};

static void __hyp t_run_case(const struct hyp_test_case *c, int idx)
{
	int rc;

	cur_case = c->id;
	cur_fails = 0;

	rc = c->run();
	if (rc != 0)
		t_fail("case returned %d", rc);

	if (cur_fails) {
		hyp_test_result.failed++;
		if (hyp_test_result.first_failed < 0)
			hyp_test_result.first_failed = idx;
		return;
	}

	hyp_test_result.passed++;
	hprintf("PASS %s\n", c->id);
}

/*
 * hyp_test_run() - run the whole suite, at EL2, before the guest is handed off
 *
 * Called from hv_start_vm() with stage-2, the GIC and EL2 stage-1 all live.
 * Reports one line per case plus a SUMMARY, fills hyp_test_result for gdb, and
 * halts the board on any failure.
 *
 * @pcpu: this core's hypervisor context
 */
void __hyp hyp_test_run(struct hyp_percpu *pcpu)
{
	int i, n = (int)ARRAY_SIZE(cases);

	hyp_test_result.magic = HYPTEST_MAGIC;
	hyp_test_result.cases = n;
	hyp_test_result.passed = 0;
	hyp_test_result.failed = 0;
	hyp_test_result.first_failed = -1;

	hprintf("%d cases at EL2\n", n);
	for (i = 0; i < n; i++)
		t_run_case(&cases[i], i);

	hprintf("SUMMARY passed=%d failed=%d\n",
		hyp_test_result.passed, hyp_test_result.failed);

	if (hyp_test_result.failed) {
		hprintf("halting: first failure is case %d (%s)\n",
			hyp_test_result.first_failed,
			cases[hyp_test_result.first_failed].id);
		hang();
	}
}

/*
 * hyp_test_hvc() - a stage result from the EL1 test core
 *
 * The payload t_el1_payload() runs on a core this harness started and reports each
 * stage with `hvc #HV_TEST_RESULT`: x1 = stage index, x2 = the value it
 * measured, x3 = the value it expected.  The EL2 side scores the reports; the
 * stub never decides anything itself.  Called with the console lock held.
 *
 * @pcpu: the reporting core's context
 * @regs: its trapped frame
 */
void __hyp hyp_test_hvc(struct hyp_percpu *pcpu, struct pt_regs *regs)
{
	u64 stage = regs->regs[1];

	if (stage >= HYPTEST_EL1_STAGES) {
		cur_case = "el1.stub";
		t_fail("stage %#llx out of range", stage);
		regs->regs[0] = (u64)HV_ENOTSUPPORTED;
		return;
	}

	el1_report[stage].cpu = pcpu->id;
	el1_report[stage].value = regs->regs[2];
	el1_report[stage].expect = regs->regs[3];
	el1_report[stage].done = 1;
	hyp_clean_range(&el1_report[stage], sizeof(el1_report[stage]));
	regs->regs[0] = 0;
}
