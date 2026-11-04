// SPDX-License-Identifier: GPL-2.0+
/*
 * PSCI for the guest.
 *
 * HCR_EL2.TSC is set, so every SMC the guest executes lands here (EC=0x17).
 * Most of them are none of our business - Qualcomm SCM calls, SMCCC
 * discovery, SYSTEM_OFF - and go on to EL3 unchanged.  The few that are about
 * cores are ours:
 *
 * - CPU_ON.  PSCI starts the target in the exception level of its caller.  A
 *   guest CPU_ON would hand the new core over at EL1 with no EL2 state at all
 *   (no vectors, no stage-1/stage-2, no GIC virtual interface), so EL2 issues
 *   the real CPU_ON itself (hyp/smp.c) and enters EL1 at the guest's entry
 *   point with the guest's context id in x0.
 * - CPU_OFF.  EL2 marks the vCPU off, then lets EL3 power the core down; a
 *   later CPU_ON runs the whole bring-up again.
 * - CPU_SUSPEND.  A standby wait: the core sleeps in EL2 like a WFI until
 *   something wakes it and returns SUCCESS.  Returning at once is permitted for
 *   every power state, a power-down state included: the Linux kernel treats a
 *   finisher that returns as -EOPNOTSUPP and falls back.
 * - SET_SUSPEND_MODE goes on to EL3 like the rest.  Refusing it keeps the guest
 *   out of OS-initiated mode, but then the DT's PSCI power-domain providers
 *   (/psci/power-domain-*) never register and everything that depends on them -
 *   the apps RSC, so every RPMH user - defers forever (run 144).  The guest's
 *   cpuidle is off, so nothing uses OSI mode's CPU_SUSPEND semantics.
 *
 * Register convention (SMCCC 1.2): function id in x0, arguments x1-x7 (x1-x17
 * for 1.2 calls), results in x0-x3 (up to x17).  A forwarded call round-trips
 * x0-x17 so it is indistinguishable from the guest executing SMC itself.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <asm/ptrace.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <linux/types.h>

#include "hv.h"

/*
 * Execute `smc #0` at EL2 with the guest's x0-x17 and write EL3's x0-x17 back.
 * x19 carries the frame pointer across the call - EL3 preserves x18 and up
 * (SMCCC 1.2) - and x18, the hypervisor's gd, is never touched.
 */
static void __hyp hyp_smc_forward(struct pt_regs *regs)
{
	asm volatile(
		"mov	x19, %0\n"
		"ldp	x0, x1, [x19, #0]\n"
		"ldp	x2, x3, [x19, #16]\n"
		"ldp	x4, x5, [x19, #32]\n"
		"ldp	x6, x7, [x19, #48]\n"
		"ldp	x8, x9, [x19, #64]\n"
		"ldp	x10, x11, [x19, #80]\n"
		"ldp	x12, x13, [x19, #96]\n"
		"ldp	x14, x15, [x19, #112]\n"
		"ldp	x16, x17, [x19, #128]\n"
		"smc	#0\n"
		"stp	x0, x1, [x19, #0]\n"
		"stp	x2, x3, [x19, #16]\n"
		"stp	x4, x5, [x19, #32]\n"
		"stp	x6, x7, [x19, #48]\n"
		"stp	x8, x9, [x19, #64]\n"
		"stp	x10, x11, [x19, #80]\n"
		"stp	x12, x13, [x19, #96]\n"
		"stp	x14, x15, [x19, #112]\n"
		"stp	x16, x17, [x19, #128]\n"
		:: "r"(regs->regs)
		: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
		  "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
		  "x19", "memory");
}

/* PSCI 32-bit calls take and return 32-bit values; 64-bit ones use all of x0. */
static inline bool __hyp psci_is64(u64 fid)
{
	return !!(fid & PSCI_0_2_64BIT);
}

/*
 * Handle one trapped guest SMC.  A trapped SMC is a Trap exception, not a
 * Secure Monitor Call exception, so ELR_EL2 is the SMC instruction itself
 * (unlike HVC, whose return address is the next instruction) and the handler
 * steps over it - the same thing KVM's handle_smc() does.  Left alone the
 * guest would re-execute the SMC forever (run 142: U-Boot stuck right after the
 * handoff on its first SMC).
 */
void __hyp hyp_handle_smc(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u64 fid = regs->regs[0];
	long ret;

	regs->elr += 4;

	switch ((u32)fid) {
	case PSCI_0_2_FN_CPU_ON:
	case PSCI_0_2_FN64_CPU_ON: {
		u64 mask = psci_is64(fid) ? ~0ULL : 0xffffffffULL;

		ret = hyp_smp_guest_cpu_on(regs->regs[1] & mask, regs->regs[2] & mask,
					   regs->regs[3] & mask);
		regs->regs[0] = (u64)(s64)ret;
		break;
	}
	case PSCI_0_2_FN_CPU_OFF:
		/* Only returns when EL3 refused (or this core is not ours to stop) */
		regs->regs[0] = (u64)(s64)hyp_smp_guest_cpu_off();
		break;
	case PSCI_0_2_FN_CPU_SUSPEND:
	case PSCI_0_2_FN64_CPU_SUSPEND:
		hyp_guest_wait(false, 0);
		regs->regs[0] = PSCI_RET_SUCCESS;
		break;
	case PSCI_0_2_FN_SYSTEM_OFF:
	case PSCI_0_2_FN_SYSTEM_RESET:
	case PSCI_1_1_FN_SYSTEM_RESET2:
	case PSCI_1_1_FN64_SYSTEM_RESET2:
		/* Say so first: it is the guest, not the board, that stops here */
		hprintf("psci: guest SYSTEM_%s (fid %#x, x1 %#llx, x2 %#llx, ELR %#llx)\n",
			fid == PSCI_0_2_FN_SYSTEM_OFF ? "OFF" : "RESET", (u32)fid,
			regs->regs[1], regs->regs[2], regs->elr - 4);
		hyp_smc_forward(regs);
		break;
	default:
		/* SCM calls for a processor EL2 loads itself are answered here */
		if (!hyp_pas_smc(regs))
			hyp_smc_forward(regs);
		break;
	}
}
