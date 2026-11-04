// SPDX-License-Identifier: GPL-2.0+
/*
 * Secondary cores as vCPUs.
 *
 * hyp/hv.c brings up one core - CPU0, the one U-Boot started on.  This file
 * turns the other seven into vCPUs the hypervisor owns (scripts/SMP.md).
 *
 * The route is PSCI CPU_ON, issued by EL2.  PSCI starts the target in the
 * exception level of its caller, so a CPU_ON from the guest would hand the
 * secondary over at EL1 with no EL2 stack, no VBAR_EL2, no stage-1 or stage-2
 * map and no struct hyp_percpu - its first exception would vector into
 * garbage.  Issued from EL2 the core wakes on hv_secondary_entry, and
 * hv_secondary_start() reproduces - per core - what hv_start_vm() did for
 * CPU0.  The SMC itself is handled by EL3: HCR_EL2.TSC only traps lower-EL
 * SMCs, an EL2 SMC goes to EL3 anyway, and it is the firmware (PSCI 1.1,
 * CPU_ON implemented, the seven secondaries reported off) that powers the
 * core.
 *
 * Two cores now share state, so the shared state is guarded with the locks
 * from hyp/lock.c:
 *
 * - the vCPU table (who is started, who is running, heartbeats) behind
 *   smp_lock, taken by CPU0's dump and by every heartbeat a core reports;
 * - the console behind console_lock, held once by whoever enters EL2 from the
 *   guest - hyp/hv.c's handle_hvc() takes it for its whole body - so a line
 *   from one core cannot interleave with a line from another.  Everything in
 *   this file that prints is called from inside that scope and prints with
 *   hprintf() directly; a nested hyp_smp_console_lock() would deadlock, the
 *   mutex is not recursive.
 *
 * Lock order is console_lock -> smp_lock, on both cores.
 *
 * Cache coherency: CPU0's writes to these tables sit in its caches and a core
 * PSCI has just woken runs with its MMU and caches off, so every table write
 * is cleaned (dc civac) before it can be seen from another core, and every
 * cross-core read invalidates first.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <dm/ofnode.h>
#include <hang.h>
#include <hypervisor.h>
#include <log.h>
#include <asm/ptrace.h>
#include <asm/system.h>
#include <hyp/mutex.h>
#include <hyp/spinlock.h>
#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/psci.h>
#include <linux/types.h>
#include <stddef.h>

#include "hv.h"

/*
 * PSCI return codes (PSCI 1.0, section 7.1); U-Boot's <linux/psci.h> has the
 * function IDs but not these.
 */
#define PSCI_SUCCESS		PSCI_RET_SUCCESS
#define PSCI_NOT_SUPPORTED	PSCI_RET_NOT_SUPPORTED
#define PSCI_INVALID_PARAMS	PSCI_RET_INVALID_PARAMS
#define PSCI_DENIED		PSCI_RET_DENIED

/*
 * What the secondary's EL1 payload does before it parks: locked-console
 * prints, then heartbeat bumps (scripts/SMP.md 3).
 */
#define HV_SMP_LINES		4
#define HV_SMP_BEATS		8

/*
 * The stack a woken core stands on: hyp_mem_stack(slot), the same 8 KiB EL2
 * stack CPU0 uses (hyp/mem.c).  hyp_percpu_init() has not run yet, so the
 * entry stub has to be handed the address explicitly - but it must NOT be a
 * stack embedded in the trampoline: hv_secondary_start() prints through
 * hyp_printf(), whose vsnprintf() chain alone wants >1 KiB of frame, and that
 * overflows any small inline array and walks over the trampoline's own
 * fields.
 */
struct hyp_vcpu {
	u64 mpidr;
	int slot;
	u8 power;				/* VCPU_OFF / VCPU_ON_PENDING / VCPU_ON */
	bool started;				/* EL3 accepted the CPU_ON */
	bool running;				/* announced itself from EL1 */
	bool parked;				/* finished its payload */
	bool failed;				/* bring-up failed, core parked */
	u32 heartbeats;				/* bumped by the core, at EL2 */
};

enum { VCPU_OFF, VCPU_ON_PENDING, VCPU_ON };

/*
 * The CPU_ON context id.  A cold core has no stack, no gd in x18 and no slot
 * yet, so it gets a private trampoline: the entry stub takes its SP_EL2 from
 * stack_top (the core's hyp_mem stack) and hands the pointer to the C half
 * as x0.
 */
struct hv_trampoline {
	u64 slot;
	u64 mpidr;
	u64 stack_top;
	/*
	 * Link-time address of the .text function this vCPU runs at EL1;
	 * 0 = hyp_secondary_guest_entry().  A vCPU's payload is part of what
	 * that vCPU is, so it travels with the CPU_ON context.
	 */
	u64 guest_entry;
	/*
	 * A guest-requested CPU_ON (hyp/psci.c): guest_entry is then the physical
	 * address the guest asked for - entered as is, no relocation offset - and
	 * guest_ctx its context id, which arrives in the guest's x0.
	 */
	u64 guest_ctx;
	u64 raw;
};

static __hyp_data struct hyp_vcpu vcpus[HYP_MAX_CPUS] = { {} };
static __hyp_data struct hv_trampoline tramps[HYP_MAX_CPUS] = { {} };
/*
 * Heartbeats of every core in one word, so a debugger can watch the secondary
 * alive over the AXI window without knowing this struct's layout.
 */
static __hyp_data u32 smp_beats;

static DEFINE_SPINLOCK(smp_lock);
static DEFINE_MUTEX(console_lock);

/* The C half, below; the stub reaches it with a plain b, resolved at link time */
static void __hyp hyp_smp_vcpu_fail(int slot);

/*
 * PSCI's entry point for a secondary core.  x0 is the CPU_ON context id we
 * passed - this core's struct hv_trampoline.  Nothing else about a core PSCI
 * has just woken can be trusted: not the SP EL3 left behind, not SPSel, not
 * the caches.  So the stub gives the core a stack of its own before it runs
 * any C, and keeps the trampoline pointer in x0 for hv_secondary_start().
 *
 * MSR SP_EL2 is UNDEFINED at EL2 (DDI0487M C5.2: writable at EL3 only), so
 * select SP_EL2 with SPSel and write it with a plain mov - the same dance
 * hyp_enter_guest() does for CPU0.
 */
__hyp void hv_secondary_entry(struct hv_trampoline *tramp)
{
	asm volatile("msr	spsel, #1\n"
		"ldr	x1, [x0, %[top]]\n"
		"mov	sp, x1\n"
		"b	hv_secondary_start\n"
		:
		: [top] "i" (offsetof(struct hv_trampoline, stack_top))
		: "memory");
}

/*
 * The vCPU payload, run at EL1.  This is an ordinary (relocatable) .text
 * function - the same trick relocation_return uses: the EL2 half takes its
 * link-time address and adds the relocation offset to reach the copy the
 * guest actually runs.
 *
 * It prints nothing itself: every announcement is an HVC, so it is served at
 * EL2 on this core's own context and reaches the console under console_lock.
 */
static void hv_secondary_guest_entry(void)
{
	struct arm_smccc_res res;
	int i;

	arm_smccc_hvc(HV_SMP_HELLO, 0, 0, 0, 0, 0, 0, 0, &res);

	for (i = 0; i < HV_SMP_LINES; i++)
		arm_smccc_hvc(HV_SMP_PRINT, (u64)"secondary core line", 0,
			      0, 0, 0, 0, 0, &res);

	for (i = 0; i < HV_SMP_BEATS; i++)
		arm_smccc_hvc(HV_SMP_DONE, 0, 0, 0, 0, 0, 0, 0, &res);

	arm_smccc_hvc(HV_SMP_DONE, 1, 0, 0, 0, 0, 0, 0, &res);

	for (;;)
		asm volatile("wfe");
}

/*
 * SCTLR_EL1 for a core entering EL1 the way PSCI promises: MMU off, caches
 * off, the RES1 bits set (0x30d00980 is what this core reports for them) and
 * everything else - EE, E0E, WXN - clear.
 */
#define SCTLR_EL1_RESET		0x30d00980ULL

static void __hyp hyp_smp_set_power(int slot, u8 power)
{
	unsigned long flags;

	spin_lock_irqsave(&smp_lock, flags);
	vcpus[slot].power = power;
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	spin_unlock_irqrestore(&smp_lock, flags);
}

/*
 * The C half of a secondary core's bring-up, at EL2, on the trampoline stack.
 * x18 holds whatever EL3 left here, so the hypervisor's own gd has to be
 * re-selected before anything prints or allocates.
 */
void __hyp hv_secondary_start(struct hv_trampoline *tramp)
{
	struct hyp_percpu *pcpu;
	unsigned long sctlr;
	u64 mpidr = tramp->mpidr;
	u64 entry;			/* where this vCPU enters EL1, set below */
	u64 guest_x0;			/* its x0 there */
	int slot;

	hyp_set_gd_ptr(hyp_root_gd());

	/* A core that enters EL2 through PSCI, not through a guest HVC, has no
	 * console lock taken for it yet - and it prints while CPU0 is still
	 * inside its own locked CPU_ON handling.  Take it for the whole bring-up
	 * and put it back before the eret: the payload's first HVC re-enters
	 * EL2 on this core, and handle_hvc() takes the same (non-recursive)
	 * mutex.
	 */
	hyp_smp_console_lock();

	/*
	 * The trampoline is this core's identity.  MPIDR_EL1 cannot be one on
	 * this board - every PE reads 0x81000000, MT set and every affinity
	 * level zero (hyp/hv.c's hyp_percpu_init_aff()) - so the affinity we
	 * handed PSCI with the CPU_ON is what says which core just woke, and
	 * the slot we built the trampoline for is what says where it belongs in
	 * our tables.  A core that woke on the wrong trampoline is a firmware
	 * bug we cannot recover from here, so it hangs loudly.
	 */
	if (hyp_cpu_id(mpidr) != (int)tramp->slot) {
		hprintf("smp: trampoline slot %llu, affinity %#llx is slot %d\n",
			tramp->slot, mpidr, hyp_cpu_id(mpidr));
		hang();
	}

	hyp_percpu_init_aff(mpidr);
	pcpu = hyp_restore_percpu(true);
	slot = pcpu->id;

	hprintf("smp: cpu%d woke at EL2 (affinity %#llx, sp_el2 %#llx)\n",
		slot, mpidr, (u64)tramp->stack_top);
	hprintf("smp: cpu%d EL2 regime it inherits: sctlr %#lx ttbr %#llx\n",
		slot, (unsigned long)mrs(sctlr_el2), (u64)mrs(ttbr0_el2));

	/*
	 * Everything hv_start_vm() set up for CPU0 that a core needs before it
	 * can run anything: the same stage-1 map (hyp_s1_apply() re-publishes
	 * the tables CPU0 built, without rebuilding them) and stage-2, which
	 * *is* required - hcr_final has HCR_EL2.VM set, so a core that skipped
	 * it would translate its guest accesses through VTTBR_EL2 = 0.  The
	 * guest half still enters EL1 with the MMU off, exactly like CPU0's
	 * first EL1 entry; U-Boot installs its own TTBR0_EL1 later.
	 *
	 * A core we cannot make safe to run is parked, not fatal: it puts the
	 * console lock back and stops on its own, so CPU0, the shell and every
	 * other core keep working - and the dump says which core failed.
	 */
	msr(vbar_el2, (u64)__hyp_vectors);
	/* The guest regime CPU0 settled on, programmed before this core turns
	 * on its own stage-1 MMU: hyp_s1_apply() sets SCTLR_EL2.M, and every
	 * walk after that runs under HCR_EL2 as it is then.  A core that flips
	 * M while still carrying whatever the firmware left in its HCR_EL2
	 * walks under a different configuration than the one its tables were
	 * built for.
	 */
	msr(hcr_el2, hyp_hcr_value());
	hyp_guest_ctl_init();
	/* Stage-2 before stage-1, the order hv_start_vm() uses on CPU0:
	 * hyp_s1_apply() turns on SCTLR_EL2.M, and with HCR_EL2.VM set every
	 * stage-1 walk that follows is itself stage-2 translated.  A core that
	 * flips M first walks its tables through whatever the firmware left in
	 * that core's VTTBR_EL2.
	 */
	if (hyp_s2_apply())
		goto failed;
	hprintf("smp: cpu%d stage-2 ok\n", slot);
	if (hyp_s1_apply())
		goto failed;
	hprintf("smp: cpu%d stage-1 ok\n", slot);
	if (hyp_gic_cpuif(mpidr))
		goto failed;
	hprintf("smp: cpu%d cpuif ok\n", slot);

	if (tramp->raw) {
		/* A guest CPU_ON: the physical entry point as asked, x0 = context id */
		entry = tramp->guest_entry;
		guest_x0 = tramp->guest_ctx;
		hprintf("smp: cpu%d -> EL1 at %#llx ctx %#llx\n", slot, entry, guest_x0);
	} else {
		entry = (tramp->guest_entry ? tramp->guest_entry
					    : (u64)hv_secondary_guest_entry) +
			hyp_reloc_off();
		guest_x0 = 0;
		hprintf("smp: cpu%d -> EL1 at %#llx\n", slot, entry);
	}
	hyp_smp_console_unlock();
	hyp_smp_set_power(slot, VCPU_ON);

#if IS_ENABLED(CONFIG_HYP_TEST)
	/* The self-test suite can hand this core a two-core job to run here: at
	 * EL2, on its own context, with stage-1, stage-2 and its own CPU
	 * interface live and the console lock back so that it can take it.
	 */
	hyp_test_secondary(pcpu);
#endif

	/* The guest half.  The handoff registers are written here and nowhere
	 * earlier - a job that takes an IRQ rewrites ELR_EL2/SPSR_EL2 with the EL2
	 * context it interrupted, and hyp_enter_guest() builds the guest frame
	 * out of them (see hv_start_vm()).
	 */
	if (tramp->raw) {
		/* PSCI's entry state for a core the guest started: EL1h, DAIF
		 * masked, MMU off, caches off (SCTLR_EL1's RES1 bits only), x0 =
		 * context id.  The kernel sets up everything else, VBAR_EL1
		 * included, before its first exception. */
		msr(sctlr_el1, SCTLR_EL1_RESET);
	} else {
		/* U-Boot's own vectors, MMU off, its own stack. */
		msr(vbar_el1, (u64)vectors + hyp_reloc_off());
		sctlr = mrs(sctlr_el1) & ~(CR_C | CR_M | CR_I);
		msr(sctlr_el1, sctlr);
		msr(sp_el1, pcpu->sp);
	}
	asm volatile("isb");

	msr(elr_el2, entry);
	/* SPSR_EL2 = 0x3C5: EL1h, SP_EL1, DAIF masked - as for CPU0. */
	msr(spsr_el2, 0x3C5);

	hyp_enter_guest(pcpu, guest_x0);
	return;

failed:
	hyp_smp_vcpu_fail(slot);
	hyp_smp_console_unlock();
	hprintf("smp: cpu%d bring-up failed, parking it\n", slot);
	hang();
}

/* Record a core whose bring-up failed, so CPU0's dump shows it. */
static void __hyp hyp_smp_vcpu_fail(int slot)
{
	unsigned long flags;

	spin_lock_irqsave(&smp_lock, flags);
	vcpus[slot].failed = true;
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	spin_unlock_irqrestore(&smp_lock, flags);
}

/*
 * One row of the table, printed under smp_lock with the caches invalidated so
 * what shows up is what memory holds, not what this core wrote.
 */
static void __hyp hyp_smp_show(int slot)
{
	struct hyp_vcpu *vcpu = &vcpus[slot];

	hyp_clean_range(vcpu, sizeof(*vcpu));
	hprintf("smp: vcpu slot %d mpidr %#llx started %d running %d parked %d "
		"failed %d heartbeats %u sp %#llx\n",
		slot, vcpu->mpidr, vcpu->started, vcpu->running, vcpu->parked,
		vcpu->failed, vcpu->heartbeats, hyp_mem_stack(slot));
}

static void __hyp hyp_smp_dump(void)
{
	int count = hyp_cpu_count();

	spin_lock(&smp_lock);
	hprintf("smp: %d vCPUs, beat counter %#llx = %u\n", count,
		(u64)&smp_beats, smp_beats);
	for (int i = 0; i < count; i++)
		hyp_smp_show(i);
	spin_unlock(&smp_lock);
}

/**
 * hyp_smp_info() - one vCPU row as memory holds it
 * @slot: slot number below hyp_cpu_count()
 * @info: filled from the row, under smp_lock, after its cache line is cleaned
 *
 * hyp_smp_dump() prints the table for a human; this is the same row for a
 * caller that has to assert on it instead of reading a log line.
 *
 * Return: 0 if the slot exists, -EINVAL otherwise.
 */
int __hyp hyp_smp_info(int slot, struct hyp_vcpu_info *info)
{
	unsigned long flags;
	struct hyp_vcpu *vcpu;

	if (slot < 0 || slot >= hyp_cpu_count() || !info)
		return -EINVAL;

	spin_lock_irqsave(&smp_lock, flags);
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	vcpu = &vcpus[slot];
	info->mpidr = vcpu->mpidr;
	info->started = vcpu->started;
	info->running = vcpu->running;
	info->parked = vcpu->parked;
	info->failed = vcpu->failed;
	info->heartbeats = vcpu->heartbeats;
	spin_unlock_irqrestore(&smp_lock, flags);

	return 0;
}

/**
 * hyp_smp_vcpu_on() - is the vCPU in @slot running guest code (PSCI ON)?
 *
 * A core the guest has not started, or has taken down with CPU_OFF, is not at EL1 and has
 * nothing to stop.
 */
bool __hyp hyp_smp_vcpu_on(int slot)
{
	unsigned long flags;
	bool on;

	if (slot < 0 || slot >= hyp_cpu_count())
		return false;

	spin_lock_irqsave(&smp_lock, flags);
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	on = vcpus[slot].power == VCPU_ON;
	spin_unlock_irqrestore(&smp_lock, flags);

	return on;
}

/* Map a PSCI return onto a log line and a Linux errno. */
static int __hyp hyp_smp_psci_ret(long val, u64 mpidr)
{
	switch (val) {
	case PSCI_SUCCESS:
		hprintf("smp: CPU_ON mpidr %#llx accepted\n", mpidr);
		return 0;
	case PSCI_NOT_SUPPORTED:
		hprintf("smp: CPU_ON mpidr %#llx: NOT_SUPPORTED\n", mpidr);
		return -EOPNOTSUPP;
	case PSCI_INVALID_PARAMS:
		hprintf("smp: CPU_ON mpidr %#llx: INVALID_PARAMETERS\n", mpidr);
		return -EINVAL;
	case PSCI_DENIED:
		hprintf("smp: CPU_ON mpidr %#llx: DENIED\n", mpidr);
		return -EACCES;
	case PSCI_RET_ALREADY_ON:
		hprintf("smp: CPU_ON mpidr %#llx: ALREADY_ON\n", mpidr);
		return -EBUSY;
	default:
		hprintf("smp: CPU_ON mpidr %#llx: PSCI error %ld\n", mpidr, val);
		return -EIO;
	}
}

/*
 * Start one core, as a vCPU, and return the PSCI result (what a guest's own
 * CPU_ON would have returned).  @raw says who asked: the guest, with a
 * physical entry point and context id of its own, or the hypervisor's EL1
 * payload path (entry is a link-time address, relocated by hyp_reloc_off()).
 *
 * smp_lock covers the table, not the SMC: EL3 may take a while to power a
 * core, and nothing else needs the lock for that long.  The state is
 * ON_PENDING from the moment the row is claimed to the moment the core is on
 * its way to EL1 (hv_secondary_start()), so a second CPU_ON for the same core
 * is told ON_PENDING or ALREADY_ON instead of racing the first.
 *
 * It does not wait for the core to announce itself: for the payload path
 * that announcement is an HVC whose handler needs console_lock, which the
 * caller may be holding, so polling here would deadlock; the caller re-reads
 * the table with HV_SMP_DUMP instead.
 */
static long __hyp hyp_smp_cpu_on(u64 mpidr, u64 entry, u64 ctx, bool raw)
{
	struct arm_smccc_res res;
	struct hv_trampoline *tramp;
	unsigned long flags;
	int slot;

	slot = hyp_cpu_id(mpidr);
	if (slot < 0 || (raw && (mpidr & ~(u64)MPIDR_AFFINITY))) {
		hprintf("smp: mpidr %#llx is not a CPU in the DT\n", mpidr);
		return PSCI_INVALID_PARAMS;
	}

	if (slot == hyp_cpu_num()) {
		hprintf("smp: cpu%d is the core we are running on\n", slot);
		return PSCI_RET_ALREADY_ON;
	}

	spin_lock_irqsave(&smp_lock, flags);
	if (vcpus[slot].power != VCPU_OFF) {
		long busy = vcpus[slot].power == VCPU_ON ? PSCI_RET_ALREADY_ON
							 : PSCI_RET_ON_PENDING;

		spin_unlock_irqrestore(&smp_lock, flags);
		return busy;
	}

	tramp = &tramps[slot];
	tramp->slot = slot;
	tramp->mpidr = mpidr;
	tramp->guest_entry = entry;
	tramp->guest_ctx = ctx;
	tramp->raw = raw;
	/* The core's own EL2 stack, 16-byte aligned by construction (hyp_mem.end
	 * is 8 KiB-aligned and the slots are 8 KiB apart), which AAPCS and the
	 * SCTLR_EL2.SA that hyp_s1_apply() publishes both require.  It is the
	 * same address hyp_percpu_init() will later store in pcpu->sp, so the
	 * core never changes stacks mid-bring-up.
	 */
	tramp->stack_top = hyp_mem_stack(slot);

	vcpus[slot].slot = slot;
	vcpus[slot].mpidr = mpidr;
	vcpus[slot].power = VCPU_ON_PENDING;
	vcpus[slot].started = true;
	vcpus[slot].running = false;
	vcpus[slot].parked = false;
	vcpus[slot].failed = false;
	hyp_clean_range(tramp, sizeof(*tramp));
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	spin_unlock_irqrestore(&smp_lock, flags);

	hprintf("smp: CPU_ON mpidr %#llx entry %#llx ctx %#llx (%s)\n", mpidr,
		entry, ctx, raw ? "guest" : "payload");
	arm_smccc_smc(PSCI_0_2_FN64_CPU_ON, mpidr, (u64)hv_secondary_entry,
		      (u64)tramp, 0, 0, 0, 0, &res);

	if ((long)res.a0 != PSCI_SUCCESS) {
		spin_lock_irqsave(&smp_lock, flags);
		vcpus[slot].power = VCPU_OFF;
		vcpus[slot].started = false;
		hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
		spin_unlock_irqrestore(&smp_lock, flags);
	}

	return (long)(int)res.a0;
}

/**
 * hyp_smp_guest_cpu_on() - PSCI CPU_ON, as the guest asked for it
 * @target: target_cpu MPIDR (affinity bits only)
 * @entry: physical address the core enters at EL1, 4-byte aligned
 * @ctx: context id, delivered in the new core's x0
 *
 * Return: the PSCI return code for the guest's x0.
 */
long __hyp hyp_smp_guest_cpu_on(u64 target, u64 entry, u64 ctx)
{
	if (entry & 3)
		return PSCI_RET_INVALID_ADDRESS;

	return hyp_smp_cpu_on(target, entry, ctx, true);
}

/**
 * hyp_smp_guest_cpu_off() - PSCI CPU_OFF for the core that trapped
 *
 * The row goes OFF first so a CPU_ON that races the power-down is not told the
 * core is still on; EL3 powers the core down and does not return.  Only a
 * refusal comes back, and then the row is ON again.
 *
 * Return: the PSCI error EL3 refused with.
 */
long __hyp hyp_smp_guest_cpu_off(void)
{
	struct arm_smccc_res res;
	unsigned long flags;
	int slot = hyp_cpu_num();

	if (slot < 0 || slot >= hyp_cpu_count())
		return PSCI_DENIED;

	spin_lock_irqsave(&smp_lock, flags);
	vcpus[slot].power = VCPU_OFF;
	vcpus[slot].started = false;
	vcpus[slot].running = false;
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	spin_unlock_irqrestore(&smp_lock, flags);

	hprintf("smp: cpu%d: CPU_OFF\n", slot);
	arm_smccc_smc(PSCI_0_2_FN_CPU_OFF, 0, 0, 0, 0, 0, 0, 0, &res);

	spin_lock_irqsave(&smp_lock, flags);
	vcpus[slot].power = VCPU_ON;
	vcpus[slot].started = true;
	hyp_clean_range(&vcpus[slot], sizeof(vcpus[slot]));
	spin_unlock_irqrestore(&smp_lock, flags);

	return (long)(int)res.a0;
}

/**
 * hyp_smp_start() - bring up one secondary core as a vCPU
 * @mpidr: MPIDR of the core to start, as the DT gives it
 *
 * Issues the PSCI CPU_ON from EL2 so the core enters our hv_secondary_entry
 * at EL2.  Does not wait for the core to report in; see hyp_smp_hvc().
 *
 * Return: 0 on success, negative errno otherwise.
 */
int __hyp hyp_smp_start(u64 mpidr)
{
	return hyp_smp_psci_ret(hyp_smp_cpu_on(mpidr, 0, 0, false), mpidr);
}

/**
 * hyp_smp_start_payload() - start a vCPU that runs a caller-provided payload
 * @mpidr: MPIDR of the core to start, as the DT gives it
 * @entry: link-time address of the ordinary (relocatable) .text function that
 *         core has to run at EL1; it is reached the way relocation_return is,
 *         as entry + hyp_reloc_off(), so the copy U-Boot actually moved is the
 *         one that executes.  __hyp code is off-limits to a guest, so a
 *         payload must not be in .text.hyp.
 *
 * The vCPU table, the EL2 bring-up and the handoff are the same ones
 * hyp_smp_start() uses; only the EL1 payload differs.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int __hyp hyp_smp_start_payload(u64 mpidr, void *entry)
{
	return hyp_smp_psci_ret(hyp_smp_cpu_on(mpidr, (u64)(unsigned long)entry, 0, false),
				mpidr);
}

/*
 * Heartbeat of the core that just trapped, reported at EL2 so both cores
 * reach the same counter through the same lock.
 */
static void __hyp hyp_smp_beat(struct hyp_percpu *pcpu, bool park)
{
	unsigned long flags;

	spin_lock_irqsave(&smp_lock, flags);
	vcpus[pcpu->id].heartbeats++;
	vcpus[pcpu->id].running = true;
	smp_beats++;
	hyp_clean_range(&vcpus[pcpu->id], sizeof(vcpus[pcpu->id]));
	hyp_clean_range(&smp_beats, sizeof(smp_beats));
	if (park) {
		vcpus[pcpu->id].parked = true;
		hyp_clean_range(&vcpus[pcpu->id], sizeof(vcpus[pcpu->id]));
	}
	spin_unlock_irqrestore(&smp_lock, flags);

	if (park)
		hprintf("smp: cpu%d parked at EL1, %u heartbeats\n", pcpu->id,
			vcpus[pcpu->id].heartbeats);
	else
		hdebug("smp: cpu%d heartbeat %u\n", pcpu->id,
		       vcpus[pcpu->id].heartbeats);
}

/*
 * Print a string the guest handed us (x1 = guest address), on the shared
 * console.  Same bounded copy as hyp/hv.c's HV_ECHO_STR handler.
 */
static void __hyp hyp_smp_print_guest(struct hyp_percpu *pcpu, u64 addr)
{
	char buf[65];
	const char *s = (const char *)addr;
	unsigned int i;

	for (i = 0; i < 64 && s[i]; i++)
		buf[i] = s[i];
	buf[i] = '\0';

	hprintf("smp: cpu%d says: %s\n", pcpu->id, buf);
}

/**
 * hyp_smp_hvc() - the SMP half of the guest's HVC services
 * @pcpu: context of the core that trapped
 * @fn: service code from the guest's x0
 * @regs: the guest frame EL2 parked for it
 *
 * Called by hyp/hv.c's handle_hvc() with console_lock already held; it must
 * not take that lock again.  It returns through the same switch, leaving the
 * guest's x0 as the return value.
 */
void __hyp hyp_smp_hvc(struct hyp_percpu *pcpu, u64 fn, struct pt_regs *regs)
{
	switch (fn) {
	case HV_SMP_ON:
		regs->regs[0] = (u64)hyp_smp_start(regs->regs[1]);
		break;
	case HV_SMP_PRINT:
		hyp_smp_print_guest(pcpu, regs->regs[1]);
		regs->regs[0] = 0;
		break;
	case HV_SMP_HELLO:
		hyp_smp_beat(pcpu, false);
		hprintf("smp: cpu%d is alive at EL1 (mpidr %#llx)\n", pcpu->id,
			pcpu->mpidr);
		regs->regs[0] = 0;
		break;
	case HV_SMP_DONE:
		hyp_smp_beat(pcpu, regs->regs[1] != 0);
		regs->regs[0] = vcpus[pcpu->id].heartbeats;
		break;
	case HV_SMP_DUMP:
		hyp_smp_dump();
		regs->regs[0] = hyp_cpu_count();
		break;
	default:
		regs->regs[0] = (u64)HV_ENOTSUPPORTED;
		break;
	}
}

/**
 * hyp_smp_console_lock() - take the console for this core's EL2 prints
 *
 * hyp/hv.c's trap entry points take this around the prints of one guest
 * entry, so a line from one core is never interleaved with a line from
 * another.  Nested calls deadlock: the mutex is not recursive.
 */
void __hyp hyp_smp_console_lock(void)
{
	mutex_lock(&console_lock);
}

/**
 * hyp_smp_console_unlock() - release the console
 */
void __hyp hyp_smp_console_unlock(void)
{
	mutex_unlock(&console_lock);
}

/**
 * hyp_smp_init() - describe the cores this hypervisor can take over
 *
 * Runs on CPU0 after hyp_cpu_init(), so the DT's affinity table is live.
 * Seeds the vCPU table with the cores the DT lists and reports the PSCI
 * method the DT advertises - the route CPU_ON has to take.
 */
void __hyp hyp_smp_init(void)
{
	ofnode psci = ofnode_path("/psci");
	const char *method = ofnode_read_string(psci, "method");
	int count = hyp_cpu_count();

	for (int i = 0; i < count; i++) {
		vcpus[i].slot = i;
		vcpus[i].mpidr = hyp_cpu_mpidr(i);
	}
	/* The core running this is on; every other one is off until a CPU_ON. */
	if (hyp_cpu_num() >= 0 && hyp_cpu_num() < count)
		vcpus[hyp_cpu_num()].power = VCPU_ON;
	hyp_clean_range(vcpus, sizeof(vcpus));

	hprintf("smp: %d cores from the DT, PSCI method %s, table at %#llx\n",
		count, method ? method : "none", (u64)vcpus);
}
