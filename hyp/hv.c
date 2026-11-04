// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2024 Caleb Connolly <caleb.connolly@linaro.org>
 *
 * Loosely based on m1n1.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] "fmt

#include <asm/io.h>
#include <asm/global_data.h>
#include <asm/ptrace.h>
#include <errno.h>
#include <dm/ofnode.h>
#include <dm/root.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <asm/system.h>
#include <log.h>
#include <hypervisor.h>
#include <hang.h>
#include <hyp/spinlock.h>
#include <stdarg.h>

/* Uncomment to enable per-access trap traces (hdebug()): */
/* #define LOG_DEBUG */
#include "hv.h"

DECLARE_GLOBAL_DATA_PTR;

/* Sysreg-trap ISS fields (EC=0x18) and the sync EC selector */
#define ISS_SYSREG_OP2		GENMASK(19, 17)
#define ISS_SYSREG_OP1		GENMASK(16, 14)
#define ISS_SYSREG_CRN		GENMASK(13, 10)
#define ISS_SYSREG_RT		GENMASK(9, 5)
#define ISS_SYSREG_CRM		GENMASK(4, 1)
#define ISS_SYSREG_DIR		BIT(0)
#define ISS_SYSREG_OP0		GENMASK(21, 20)
#define ESR_EC			GENMASK(31, 26)
#define MDCR_EL2_TDE		BIT(8)

/* HVC #imm16 encoding (./scripts/isa.py op hvc): imm16 is bits 20:5 */
#define HVC_OPCODE_MASK		0xffe0001fU
#define HVC_OPCODE		0xd4000002U
#define HVC_IMM			GENMASK(20, 5)

static __hyp_data struct hyp_percpu percpu[8] = { {} };

/* U-Boot's relocation offset (new link addr - CONFIG_TEXT_BASE), stashed by
 * hv_start_vm() so trap prints can report ELR as a link-time address. */
static __hyp_data u64 reloc_off;

u64 __hyp hyp_reloc_off(void)
{
	return reloc_off;
}

/* HCR_EL2 as hv_start_vm() settled on it: every secondary core runs its
 * guest under exactly this configuration. */
static __hyp_data u64 hcr_final;

u64 __hyp hyp_hcr_value(void)
{
	return hcr_final;
}

/* The hypervisor's own gd: a core PSCI has just woken has nothing useful in
 * x18, and every hyp print, allocation and stack lookup runs off gd. */
static __hyp_data gd_t *root_gd;

gd_t *__hyp hyp_root_gd(void)
{
	return root_gd;
}

/*
 * Per-cpu context of the core that called this, keyed by the affinity the DT
 * gives it.
 *
 * The affinity - not MPIDR_EL1 - has to be the key here.  Every PE on this
 * board reads MPIDR_EL1 = 0x81000000: MT (bit 24) is set, which says the
 * affinity levels are not valid, and Aff2:Aff1:Aff0 are all zero.  Keying
 * percpu[] on that - or on `mpidr_el1 & 0xff`, as this function used to -
 * maps all eight cores onto slot 0.  The DT's reg cells (0x0, 0x100 ... 0x700)
 * and the GIC redistributors' GICR_TYPER affinity do agree with each other and
 * with what PSCI's CPU_ON takes, so they are the identity: CPU0, the only core
 * that was already up, is the one whose masked MPIDR is 0x0; every other core
 * is identified by the affinity we asked PSCI to start (hyp/smp.c).
 */
void __hyp hyp_percpu_init_aff(u64 mpidr)
{
	struct hyp_percpu *pcpu;
	u64 aff = mpidr & MPIDR_AFFINITY;
	int id = hyp_cpu_id(aff);

	if (id < 0) {
		hprintf("percpu: affinity %#llx is not a CPU in the DT\n", aff);
		hang();
	}

	pcpu = &percpu[id];

	pcpu->global_data = gd;
	pcpu->id = id;
	pcpu->mpidr = aff;
	pcpu->vcpu.id = id;
	pcpu->sp = hyp_mem_stack(id);
	hyp_clean_range(pcpu, sizeof(*pcpu));

	/* Stash per-cpu context */
	msr(tpidr_el2, (u64)pcpu);
	/* VMPIDR_EL2 is what the guest's MRS MPIDR_EL1 reads back: the affinity
	 * the DT describes, with UPT set so the guest sees a PE that is up and
	 * running - its CPU driver, its PSCI calls and the GIC's affinity
	 * routing all key off it. */
	msr(vmpidr_el2, MPIDR_UPT | aff);
}

void __hyp hyp_percpu_init(void)
{
	hyp_percpu_init_aff(mrs(mpidr_el1));
}

void __hyp hyp_set_gd_ptr(gd_t *g)
{
	asm volatile("mov x18, %0" : : "r" (g) : "memory");
}

static void __hyp hyp_set_gd(struct hyp_percpu *pcpu)
{
	hyp_set_gd_ptr(pcpu->global_data);
}

struct hyp_percpu *__hyp hyp_restore_percpu(bool restore_gd)
{
	struct hyp_percpu *pcpu = (void *)mrs(tpidr_el2);

	if (restore_gd)
		hyp_set_gd(pcpu);

	return pcpu;
}

/*
 * The console is one UART shared by every core, so a line is buffered here and
 * written in one go under a lock.  The lock is per-core recursive: hyp code
 * that already owns the console for a sequence - hyp/smp.c's bring-up, the HVC
 * handlers - prints through here without deadlocking on itself.
 */
static DEFINE_SPINLOCK(console_io);
static __hyp_data u32 console_depth[HYP_MAX_CPUS];

/**
 * hyp_cpu_num() - slot of the calling core, or -1 if it has no context yet
 *
 * TPIDR_EL2 is what says which core this is: every PE on this board reads the
 * same MPIDR_EL1, so the affinity cannot be used (see hyp_percpu_init_aff()).
 *
 * Return: the per-cpu id, or -1 before this core has a hyp_percpu.
 */
int __hyp hyp_cpu_num(void)
{
	struct hyp_percpu *pcpu = (void *)mrs(tpidr_el2);

	/*
	 * TPIDR_EL2 is only ours once hyp_percpu_init_aff() has written it;
	 * before that it holds whatever the firmware left there (measured on
	 * this board at the first hprintf() in hv_init(): 0xffffffb2c2039ff0).
	 * EL2's stage-1 MMU is still off that early, so reading `pcpu->id`
	 * through such a value faults at EL2 on a vector table that is not
	 * ours yet - which is how a print in hv_init() rebooted the board.
	 * Only a pointer inside percpu[] is a context.
	 */
	if (pcpu < percpu || pcpu >= percpu + ARRAY_SIZE(percpu) ||
	    pcpu->id < 0 || pcpu->id >= HYP_MAX_CPUS)
		return -1;

	return pcpu->id;
}

/**
 * hyp_printf() - write one whole hypervisor console line, from one core
 * @tag: the calling file's console tag, e.g. "[HYP] "
 * @fmt: printf format for the message
 *
 * hprintf() is the caller; hyp/hv.h explains why the line is buffered and
 * locked instead of being printed straight out.
 */
void __hyp hyp_printf(const char *tag, const char *fmt, ...)
{
	char line[256];
	va_list args;
	int id, n, len;

	id = hyp_cpu_num();
	if (id >= 0)
		n = snprintf(line, sizeof(line), "%scpu%d: ", tag, id);
	else
		n = snprintf(line, sizeof(line), "%s: ", tag);
	if (n < 0 || n >= (int)sizeof(line))
		n = 0;

	va_start(args, fmt);
	len = vsnprintf(line + n, sizeof(line) - n, fmt, args);
	va_end(args);
	len = len < 0 ? n : min_t(int, n + len, sizeof(line) - 1);

	/* the ring first: it has its own lock, and it is what the :2222 console serves */
	hyp_log_put(line, len);

	if (!hyp_log_uart())
		return;

	if (id < 0) {
		puts(line);
		return;
	}

	if (!console_depth[id])
		spin_lock(&console_io);
	console_depth[id]++;
	puts(line);
	if (!--console_depth[id])
		spin_unlock(&console_io);
}

extern void relocation_return(void);

int __hyp hv_init(void)
{
	puts("hv_init!\n");
	printf("current_el: %d\n", current_el());
	if (current_el() != 2)
		return 0;

	hprintf("Initialising U-Boot hypervisor!\n");

	root_gd = gd;
	hyp_clean_range(&root_gd, sizeof(root_gd));

	hyp_mem_init();
	hyp_cpu_init();
	hyp_percpu_init();
	hyp_smp_init();

	/* Clear VTTBR_EL2 so the stage-2 tables Gunyah left behind
	 * cannot interfere until hv_start_vm() installs our own. */
	msr(vttbr_el2, 0);

	return 0;
}

/*
 * The EL2 controls that decide what the guest touches natively, written on
 * every core before it runs a guest.  XBL/Gunyah left all of them aimed at
 * their own guests; none is a hypervisor service of ours, so each one is set to
 * "native" explicitly rather than inherited.
 *
 * CPTR_EL2 (E2H == 0 layout per ./scripts/isa.py reg CPTR_EL2): FPSIMD and SVE
 * without traps (TFP=0, TZ=0, TCPAC=0) while keeping every RES1 bit set: [13],
 * [12] (TSM reads as RES1 when FEAT_SME is not implemented - no SME on this
 * core), [9], [7:0].  The first Linux boot (2026-10-19) trapped an FP/SVE
 * access (EC=0x19 ESR=0x66000000) that the guest had no EL2 handler for.
 *
 * MDCR_EL2: Gunyah left TDRA, TDOSA, TDA, TPM and TPMCR set and HPMN = 20, past
 * PMCR_EL0.N.  Every guest debug and PMU register access then trapped here and
 * was answered RAZ/WI (run 113: OSDLR_EL1/MDSCR_EL1/OSLSR_EL1 reads).  The
 * guest owns both natively: no trap bits, HPMN = all implemented counters.
 *
 * CNTHCTL_EL2 (E2H == 0): exactly EL1PCTEN and EL1PCEN - the guest reads the
 * physical counter and uses the physical timer - and nothing else.  The other
 * bits include the FEAT_ECV traps on the guest's virtual counter and timer
 * (EL1TVT/EL1TVCT/EL1NVPCT/EL1NVVCT) and a secondary core comes out of reset
 * with them as EL3 left them: cpu1's CNTVCTSS_EL0 reads trapped and were
 * answered 0, so its clock never advanced (run 143).  CNTVOFF_EL2 = 0 keeps its virtual count
 * equal to the physical one on every core.  VPIDR_EL2 is this core's MIDR so the
 * guest sees the real part (cores differ on this board); VMPIDR_EL2 is the
 * per-vCPU affinity hyp_percpu_init_aff() programs.  HSTR_EL2 and HCRX_EL2 get
 * no traps (HCRX_EL2 exists only with FEAT_HCX).
 */
void __hyp hyp_guest_ctl_init(void)
{
	msr(cptr_el2, 0x32ffULL);
	msr(mdcr_el2, FIELD_GET(GENMASK(15, 11), mrs(pmcr_el0)) |
		      (IS_ENABLED(CONFIG_HYP_USB) ? MDCR_EL2_TDE : 0));
	msr(cnthctl_el2, 3ULL);
	msr(cntvoff_el2, 0);
	msr(vpidr_el2, mrs(midr_el1));
	msr(hstr_el2, 0);
	if (FIELD_GET(GENMASK_ULL(43, 40), mrs(id_aa64mmfr1_el1)))	/* ID_AA64MMFR1_EL1.HCX */
		msr(S3_4_C1_C2_2, 0);					/* HCRX_EL2 */
	asm volatile("isb");
}

noinline void __hyp hv_start_vm(u64 _entrypoint, u64 offset)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(true);
	unsigned long sctlr;

	printf("%s: relocation_return: %#llx (with offset: %#llx)\n", __func__,
		(phys_addr_t)relocation_return, offset + (phys_addr_t)relocation_return);

	/* Install vectors: EL2 gets ours, the guest keeps U-Boot's own (the
	 * relocated copy - VBAR is a physical address and we are still
	 * identity-mapped here, so link + offset is where it landed). */
	msr(vbar_el2, (u64)__hyp_vectors);
	msr(vbar_el1, (u64)vectors + offset);
	/* Disable MMU in EL1 */
	sctlr = mrs(sctlr_el1) & ~(CR_C | CR_M | CR_I);
	msr(sctlr_el1, sctlr);

	/* HCR_EL2: XBL/Gunyah left a configuration aimed at its own guest
	 * (it traps ID reads, SMCs, barriers...).  Run the guest as close
	 * to native as possible and keep only the traps we actually
	 * serve; the resulting mask is documented in scripts/HYPERVISOR.md. */
	u64 hcr = mrs(hcr_el2);

	hprintf("hcr_el2: %#llx ->\n", hcr);
	hcr |= HCR_VM | HCR_PTW;		/* VM: guest is a VM; PTW: stage-2 walks read memory */
	hcr = (hcr & ~HCR_BSU_MASK) | HCR_BSU_IS;
	hcr &= ~HCR_TID_MASK;		/* TID0..3: guest reads real ID registers */
	/* TSC: guest SMCs trap to EL2 (hyp/psci.c).  The hypervisor has to be
	 * the one to start a core, because PSCI enters a CPU_ON target at the
	 * caller's exception level and a core that wakes at EL1 has no EL2 at
	 * all; everything that is not core bring-up goes on to EL3 unchanged. */
	hcr |= HCR_TSC;
	hcr &= ~(HCR_TACR | HCR_TSW | HCR_TPCP);	/* guest runs them natively */
	/* Gunyah's remaining traps and modes, none of which serve anything
	 * we emulate: FGT is what made its debug-register reads flood the
	 * EC=0x18 path already, NV=1 makes the guest's EL1 behave as a
	 * nested hypervisor, and bit 1 (SWIO in ./scripts/isa.py's
	 * HCR_EL2 layout) is another leftover.  Clear all three; RW (31)
	 * is the one inherited CONDITIONALFIELD the guest needs - it runs
	 * AArch64.
	 *
	 * API/APK stay set (Gunyah had them set too, and clearing them
	 * here is what killed our first Linux boots): with API == 0, once
	 * the guest enables PAuth itself - Linux sets
	 * SCTLR_EL1.{EnIA,EnIB,EnDA,EnDB} for in-kernel PAC via its
	 * cpufeature enable (arch/arm64/include/asm/pointer_auth.h) -
	 * every paciasp/autiasp the compiler sprinkled through the kernel
	 * traps here as EC=0x9 "Trapped use of a Pointer authentication
	 * instruction because HCR_EL2.API == 0" (scripts/esr_el3.json;
	 * its ISS is all RES0, so our fatal exit ESR=0x26000000
	 * ELR=0xffff800081fdb0c4 was a PAC trap on perf_event_init+0x0's
	 * paciasp, and FAR=0x17a20d08 was stale FAR_EL2 left by the last
	 * real data abort, not an RPMH fault).  With APK == 0 the
	 * kernel's MSR APIAKeyLo/Hi_EL1 writes trap as EC=0x18 and their
	 * values are dropped, so the guest would sign with whatever keys
	 * XBL left behind.  The kernel detected "Address authentication"
	 * from ID registers it reads natively: let its whole PAC stack
	 * run natively at EL1.
	 *
	 * ATA stays 0: with ATA == 0 every GCR_EL1/RGSR_EL1/TFSR_EL1/
	 * TFSRE0_EL1 access at EL1 traps here as EC=0x18 (DDI0487M
	 * D24.2.104 "ATA, bit [56]": "accesses to the following
	 * registers at EL1 are trapped to EL2 and reported using EC
	 * syndrome value 0x18") - Linux writes GCR_EL1 on every
	 * mte_thread_switch, which buried the boot console under 584
	 * "unhandled sysreg" lines in run-21.  Setting ATA = 1 made
	 * those accesses native (run-22: spam gone) - but the board
	 * then warm-resetted silently, no panic line, no HYP signpost,
	 * right after the guest printed "CPU features: detected:
	 * Memory Tagging Extension / Asymmetric MTE Tag Check Fault",
	 * i.e. the moment tagging goes live at EL1&0 the kernel's
	 * tag-storage accesses hit something this board has not
	 * enabled for our VM ([INFERENCE]: our stage-2 DRAM blocks are
	 * plain Normal-WB, not Normal-Tagged) and the interconnect
	 * answers with a PSC-driven reset (XBL's next banner logs
	 * "PM: Reset by PSHOLD" + "Warm Reset").  So: ATA 0, and
	 * handle_sysreg_trap answers the four MTE registers from a
	 * shadow - consistent guest values, no spam, no live tagging. */
	hcr &= ~(HCR_SWIO | HCR_FGT | HCR_NV);
	hcr |= HCR_API | HCR_APK;
	/* TWI: trap WFI/WFIT so handle_wfx() can wait for the guest.  A PE that
	 * sleeps in a real WFI is never woken by the physical interrupt this
	 * hypervisor routes to EL2 (IMO=1): with the guest's virtual timer
	 * expired and PPI 27 pending in the redistributor, the guest idled in
	 * WFI for good (runs 111-116), while an awake EL2 polling
	 * ICC_HPPIR1_EL1 sees the same interrupt within a tick (run 128). */
	hcr |= HCR_TWI;
	/* HCR_TIDCP stays set: it is what makes the guest's accesses to
	 * unallocated group-3 sysregs trap here instead of being undefined.
	 * The GIC CPU interface no longer needs this - hyp/gic.c enables the
	 * hardware virtual interface, which serves those accesses itself. */
	hprintf("hcr_el2: %#llx\n", hcr);
	/* Stash the offset before anything can trap: EL2 prints turn raw
	 * ELR values back into link-time addresses. */
	reloc_off = offset;
	hyp_clean_range(&reloc_off, sizeof(reloc_off));
	/* Replace the inherited stage-2 map: the one Gunyah left has
	 * holes (the apps SMMU at 0x15000000 is unmapped there, which
	 * faulted a guest LDR).  hyp/mmio.c builds our own identity
	 * map (DRAM Normal WB, below-DRAM Device) and serves the
	 * faults its spy regions produce. */
	hyp_s2_init();
	hyp_s2_install();
	/* Hyp DM pass: bind + probe the `hypervisor`-tagged DT nodes here, at
	 * EL2, once our stage-2 map exists - a driver probe registers its MMIO
	 * watches with hyp_mmio_watch(), which needs those tables.  gd is the
	 * hypervisor's own (pre-reloc) copy, so gd->dm_root, the DTB and the
	 * pre-reloc malloc pool are all live, and every non-__hyp function this
	 * reaches is the link-time copy: relocation fixups were applied to the
	 * relocated U-Boot, not to the resident one. */
	if (CONFIG_IS_ENABLED(OF_REAL)) {
		int tagged = 0, bound = 0;

		dm_hyp_scan(gd->dm_root, ofnode_root(), &tagged, &bound);
	}

	/* Generic-counter delays first: the EL2 voter and the USB bring-up wait on
	 * them.  Then the controller EL2 will take over from the guest later
	 * (DT-driven, hyp/usb.c); here it is only found and remembered. */
	hyp_timer_init();
	hyp_rpmh_init();
	hyp_smmu_init();
	hyp_pas_init();
	hyp_res_init();
	hyp_usb_init();
	hyp_phys_init();
	hyp_spmi_init();

	/* GICv3 and the guest's virtual CPU interface, armed before the guest
	 * runs a single instruction: the distributor base and the console
	 * UART's interrupt both come from the DT (hyp/gic.c), and HCR_EL2.IMO
	 * is inherited as 1, so every physical IRQ the guest could take is
	 * ours to serve. */
	hyp_gic_init();
	/* Guest MMU state at the handoff (it enables its own later). */
	hprintf("guest mmu: sctlr_el1=%#lx ttbr1_el1=%#lx\n",
		(unsigned long)mrs(sctlr_el1), (unsigned long)mrs(ttbr1_el1));
	/* EnSCXT (bit 53): without it the guest's SCXTNUM_EL0 writes - one per
	 * context switch when ID_AA64PFR0_EL1.CSV2 >= 2 - each trap to EL2. */
	if (FIELD_GET(GENMASK_ULL(59, 56), mrs(id_aa64pfr0_el1)) >= 2)
		hcr |= BIT_ULL(53);
	msr(hcr_el2, hcr);
	hcr_final = hcr;
	hyp_clean_range(&hcr_final, sizeof(hcr_final));

	hyp_guest_ctl_init();

	/* Run EL2 with its own stage-1 identity MMU on (hyp/s1.c): the
	 * hypervisor code/data, the pools/stacks and the relocated U-Boot
	 * it calls into are all mapped; anything else faults visibly. */
	hyp_s1_enable();

	/* Run on the hypervisor's own gd copy (the one every trap handler
	 * re-selects via hyp_restore_percpu(true)) and mark it relocated so
	 * show_regs() symbolizes guest addresses.  This must NOT touch the
	 * guest's relocated gd: an old `gd->flags &= GD_FLG_RELOC` here
	 * wiped GD_FLG_HAVE_CONSOLE from it and silently dropped every EL1
	 * console write. */
	gd->flags |= GD_FLG_RELOC;

#if IS_ENABLED(CONFIG_HYP_TEST)
	/* The self-test suite (scripts/TESTING.md) runs here, at EL2, with
	 * stage-2, the GIC and EL2 stage-1 all live.  It prints a SUMMARY line
	 * and halts the board on any failure, so only a passing suite reaches
	 * the handoff below. */
	hyp_test_run(pcpu);
#endif

	hprintf("Switching to EL1 -> %#lx\n",
		(unsigned long)(offset + (phys_addr_t)relocation_return));
	/* relocation_return resumes EL1 without reloading x18: hand the
	 * guest back its relocated gd before the eret. */
	hyp_set_gd_ptr(gd->new_gd);
	/*
	 * Park the synthesized guest frame, unmask EL2 IRQs and eret -
	 * hyp_enter_guest() is the same handoff a secondary core gets.
	 *
	 * ELR_EL2 and SPSR_EL2 are set here and nowhere earlier:
	 * hyp_enter_guest() builds the guest frame out of them, and every
	 * exception taken on the way here - the GIC arming, the stage-1 enable,
	 * the self-test suite and the IRQ it takes - rewrites both with the
	 * context it interrupted.  Set them any sooner and the handoff resumes
	 * that instead of U-Boot.
	 */
	msr(elr_el2, offset + (phys_addr_t)relocation_return);
	/* SPSR_EL2 = 0x3C5: M[4:0]=00101 (EL1h, SP_EL1), DAIF=1111 (all masked). */
	msr(spsr_el2, 0x3C5);
	hyp_enter_guest(pcpu, 0);
}

/*
 * Hand the guest its first instruction.
 *
 * Exceptions from lower EL enter on SP_EL2, not the SP_EL0 our C code runs
 * on, and MSR SP_EL2 is UNDEFINED at EL2 (DDI0487M C5.2: writable at EL3
 * only) - select SP_EL2 with SPSel and write it with a plain mov.  From here
 * EL2 runs on SP_EL2 = the per-cpu hyp stack, which is also what lower-EL
 * exception entry uses.
 *
 * EL2 keeps DAIF.I masked from here on, which every exception entry sets:
 * ERET replaces PSTATE with the guest's SPSR, and a physical IRQ routed to EL2
 * (HCR_EL2.IMO=1) is taken from EL0/EL1 whatever the guest's own PSTATE.I says
 * - only an interrupt targeting the *current* Exception level is masked by
 * PSTATE.I (DDI0487M D1.4.6.4).  Unmasking EL2 just before the eret would
 * serve nothing and open a window in which an interrupt lands on the eret
 * itself, overwrites ELR_EL2/SPSR_EL2 with our own EL2 context and frees the
 * frame the handoff was about to pop (run 112).
 *
 * The synthesized guest frame is still parked on the stack and stashed in
 * pcpu->vcpu.regs, so the vCPU always has a pt_regs describing the handoff
 * state (and the stack layout is the one every later exit builds on).
 *
 * Every core enters its guest through here: hv_start_vm() for CPU0,
 * hv_secondary_start() for a core PSCI woke for us.
 */
void __hyp hyp_enter_guest(struct hyp_percpu *pcpu, u64 guest_x0)
{
	asm volatile(
		"msr	spsel, #1\n"
		"mov	sp, %0\n"
		"stp	x27, x28, [sp, #-16]!\n"
		"stp	x25, x26, [sp, #-16]!\n"
		"stp	x23, x24, [sp, #-16]!\n"
		"stp	x21, x22, [sp, #-16]!\n"
		"stp	x19, x20, [sp, #-16]!\n"
		"stp	x17, x18, [sp, #-16]!\n"
		"stp	x15, x16, [sp, #-16]!\n"
		"stp	x13, x14, [sp, #-16]!\n"
		"stp	x11, x12, [sp, #-16]!\n"
		"stp	x9, x10, [sp, #-16]!\n"
		"stp	x7, x8, [sp, #-16]!\n"
		"stp	x5, x6, [sp, #-16]!\n"
		"stp	x3, x4, [sp, #-16]!\n"
		"stp	x1, x2, [sp, #-16]!\n"
		"mrs	x1, esr_el2\n"
		"mrs	x2, elr_el2\n"
		"mrs	x3, spsr_el2\n"
		"mov	x0, %3\n"			/* the guest's x0: PSCI context id, 0 for U-Boot */
		"stp	x1, x0, [sp, #-16]!\n"
		"stp	x3, x2, [sp, #-16]!\n"
		"mov	x1, sp\n"
		"str	x1, [%1, %c2]\n"		/* pcpu->vcpu.regs = the frame */
		"eret"
		:: "r"(pcpu->sp), "r"(pcpu),
		   "i"(offsetof(struct hyp_percpu, vcpu.regs)),
		   "r"(guest_x0)
		: "x0", "x1", "x2", "x3", "memory");
}

void __hyp _hv_panic(struct pt_regs *regs)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(true);

	hprintf("Panic! CPU=%d ESR=%#lx\n", pcpu->id, regs->esr);
	show_regs(regs);
	hang();
}

/*
 * Trace every guest -> EL2 entry (hdebug: compiled out unless this file
 * defines LOG_DEBUG): what the CPU reported, the control state that
 * decides how the guest is seen, both vector tables, and the guest
 * instruction at ELR (informational: EL2's stage-1 map covers only the
 * hypervisor's own layout, so this load only works over mapped guest
 * code).
 */
static void __hyp hv_trace(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u32 ec = FIELD_GET(ESR_EC, regs->esr);
	u64 __maybe_unused link = regs->elr - reloc_off;

	hdebug("exit#%u cpu%u EC=%#x ESR=%#lx ELR=%#llx (raw %#lx) SPSR=%#lx HCR=%#lx VBAR2=%#lx VBAR1=%#lx\n",
		(unsigned)++pcpu->exits, (unsigned)pcpu->id, (unsigned)ec,
		regs->esr, link, regs->elr, regs->spsr, mrs(hcr_el2),
		(u64)mrs(vbar_el2), (u64)mrs(vbar_el1));
	if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25)
		hdebug(" far=%#lx\n", mrs(far_el2));
#ifdef LOG_DEBUG
	/* ELR is a guest VA once the guest's MMU is on: hv_trace() must
	 * not be the thing that faults on the way to reporting a fault. */
	u32 insn;

	if (hyp_fetch_guest_insn(regs->elr, &insn) == 0)
		hdebug(" op=%#08x\n", insn);
#endif
}

/*
 * Guest -> EL2 HVC service path (EC=0x16: "HVC instruction execution in
 * AArch64 state", see scripts/esr_el3.json).  Speaks the same calling convention
 * U-Boot already uses for firmware calls - function code in x0, results in
 * x0..x3 (arch/arm/cpu/armv8/fwcall.c, linux/arm-smccc.h) - so the guest-side
 * `hvc` command and hv_demo_hvc() need no glue of their own.
 *
 * The service numbers are project-local (include/hypervisor.h): claiming an
 * SMMI function ID would also require GetHypervisor/GetLimits, which we do
 * not implement (see scripts/HYPERVISOR.md).
 */
static void __hyp handle_hvc(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u64 fn = regs->regs[0];
	/* ELR_EL2 of an HVC exit is the preferred return address: the
	 * instruction *after* the hvc (as for SVC and SMC), so the guest resumes
	 * exactly there and ELR is never advanced.  The call itself sits at
	 * ELR-4; it is read only to recover the immediate for the trace. */
	u32 insn = 0;
	bool at_hvc = hyp_fetch_guest_insn(regs->elr - 4, &insn) == 0 &&
		      (insn & HVC_OPCODE_MASK) == HVC_OPCODE;
	/* HVC #imm16: imm is bits 5..20 */
	u32 imm = at_hvc ? FIELD_GET(HVC_IMM, insn) : 0;

	/* One console, every core: hyp/smp.c owns the lock, and its SMP
	 * handlers run under it too. */
	hyp_smp_console_lock();
	hprintf("hvc cpu%u imm=%#x fn=%#llx x1=%#lx\n",
		(unsigned)pcpu->id, (unsigned)imm, fn, (unsigned long)regs->regs[1]);

	switch (fn) {
	case HV_GET_ID:		/* proof that EL2 handled the call */
		regs->regs[0] = HV_ID_MAGIC;
		regs->regs[1] = HV_ABI_PSA_X0;
		break;
	case HV_ECHO:		/* round-trip an argument: proves x0 is restored */
		regs->regs[0] = regs->regs[1] ^ HV_ECHO_KEY;
		break;
	case HV_ECHO_STR: {	/* print the guest's NUL-terminated string (x1) */
		char buf[65];
		const char *s = (const char *)regs->regs[1];
		unsigned int i;

		for (i = 0; i < 64 && s[i]; i++)
			buf[i] = s[i];
		buf[i] = '\0';
		hprintf("hvc echo: %s\n", buf);
		regs->regs[0] = i;
		break;
	}

	case HV_SMP_ON:		/* start a secondary core, as a vCPU */
	case HV_SMP_PRINT:		/* print a string one of our cores passes */
	case HV_SMP_HELLO:	/* a secondary core announcing itself */
	case HV_SMP_DONE:
	case HV_SMP_DUMP:	/* print the vCPU table */
		hyp_smp_hvc(pcpu, fn, regs);
		break;

	case HV_OS_HANDOFF:	/* the guest is done with its devices and starts the OS */
		regs->regs[0] = (u64)(long)hyp_smmu_handoff();
		hyp_net_init();
		break;

	case HV_USB_TAKEOVER:	/* the guest has released the USB controller */
		regs->regs[0] = (u64)hyp_usb_takeover(pcpu);
		break;

#if IS_ENABLED(CONFIG_HYP_TEST)
	case HV_TEST_RESULT:	/* an EL1 test core reporting a stage result */
		hyp_test_hvc(pcpu, regs);
		break;
#endif
	default:
		regs->regs[0] = (u64)HV_ENOTSUPPORTED;
		break;
	}

	/* What the guest will see on resume, printed before the frame is
	 * restored - elx_return() reads these back out of memory. */
	hdebug("hvc resume: elr=%#llx spsr=%#llx x0=%#llx\n",
		regs->elr, regs->spsr, regs->regs[0]);
	hyp_smp_console_unlock();
}

/*
 * Trapped MSR/MRS from the guest (EC=0x18).
 *
 * hyp/gic.c turns on the hardware virtual CPU interface (ICC_SRE_EL2.SRE +
 * ICH_HCR_EL2.En), so the guest's ICC_IAR1_EL1, ICC_EOIR1_EL1, ICC_PMR_EL1,
 * ICC_CTLR_EL1 and ICC_RPR_EL1 accesses are served by the CPU against
 * ICH_VMCR_EL2 and never reach this function.  The hand-written PMR/CTLR/RPR
 * mirroring an earlier revision did here is gone with it: it only existed
 * because ICH_HCR_EL2.TC was trapping those registers (scripts/HYP_MMU.md), and TC
 * is deliberately never set.
 *
 * What does trap is SGI generation - ICC_SGI1R_EL1 / ICC_ASGI1R_EL1 /
 * ICC_SGI0R_EL1 - which no hardware virtualizes: the hypervisor must turn the
 * write into a virtual interrupt (hyp_gic_emulate_sgi1r()).
 *
 * What else traps, and is answered quietly from a shadow below: the
 * four MTE state registers (ATA == 0, see hv_start_vm) and
 * ICC_IGRPEN1_EL1 - the hardware virtual interface serves IGRPEN0
 * against ICH_VMCR_EL2 natively on this core, but IGRPEN1 still
 * lands here (run-21/22: one trap each), so both are mapped onto the
 * VMCR group-enable bits hyp/gic.c programs.
 * ISS layout: Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10] Rt[9:5]
 * CRm[4:1] Direction[0] (0 = write).
 */
/*
 * Shadow of the four MTE state registers that trap with ATA == 0: the
 * guest's own values, kept here - writes land, reads come back out of
 * the same words, so Linux's GCR_EL1 round-trip (mte_thread_switch
 * writes it per CPU per switch) stays consistent.  Initialized data,
 * not a .bss hole: .bss inside .hyp is never zeroed (crt0_64.S clears
 * BSS after relocation, at the relocated copy's address).
 */
static __hyp_data struct {
	u64 gcr, rgsr, tfsr_el1, tfsr_el0;
} mte_sysregs = { 0 };

static void __hyp handle_sysreg_trap(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u32 iss = (u32)(regs->esr & 0x1FFFFFFU);
	u32 op0 = FIELD_GET(ISS_SYSREG_OP0, iss);
	u32 op2 = FIELD_GET(ISS_SYSREG_OP2, iss);
	u32 op1 = FIELD_GET(ISS_SYSREG_OP1, iss);
	u32 crn = FIELD_GET(ISS_SYSREG_CRN, iss);
	u32 crm = FIELD_GET(ISS_SYSREG_CRM, iss);
	u32 rt  = FIELD_GET(ISS_SYSREG_RT, iss);
	bool wr = !(iss & ISS_SYSREG_DIR);	   /* direction bit 0: 0 = write */
	u64 enc = ((u64)op0 << 21) | ((u64)op1 << 18) |
		  ((u64)crn << 8) | ((u64)crm << 4) | op2;
	u64 val = 0;

	/* ICC_SGI1R_EL1 / ICC_ASGI1R_EL1 / ICC_SGI0R_EL1 (write only). */
	if (op0 == 3U && op1 == 0U && crn == 12U && crm == 11U &&
	    (op2 == 5U || op2 == 6U || op2 == 7U)) {
		if (wr)
			hyp_gic_emulate_sgi1r(regs->regs[rt]);
		regs->elr += 4ULL;
		return;
	}

	switch (enc) {
	case (3U << 21) | (12U << 8) | (12U << 4) | 5U:	/* ICC_SRE_EL1 (S3_0_C12_C12_5): RAZ/WI.  The guest
						 * has a virtual interface already;
						 * telling it "no SRE here" is what
						 * Xen does for the same access. */
		break;

	/*
	 * MTE state registers (S3_0_...), trapped because ATA == 0:
	 * answer from the shadow, never touch the real registers, and
	 * say nothing on the console.
	 */
	case (3U << 21) | (1U << 8) | 5U:	/* RGSR_EL1, S3_0_C1_C0_5 */
		if (wr)
			mte_sysregs.rgsr = rt == 31U ? 0 : regs->regs[rt];

		else
			val = mte_sysregs.rgsr;
		break;
	case (3U << 21) | (1U << 8) | 6U:	/* GCR_EL1, S3_0_C1_C0_6 */
		if (wr)
			mte_sysregs.gcr = rt == 31U ? 0 : regs->regs[rt];

		else
			val = mte_sysregs.gcr;
		break;
	case (3U << 21) | (5U << 8) | (6U << 4):	/* TFSR_EL1, S3_0_C5_C6_0 */
		if (wr)
			mte_sysregs.tfsr_el1 = rt == 31U ? 0 : regs->regs[rt];

		else
			val = mte_sysregs.tfsr_el1;
		break;
	case (3U << 21) | (5U << 8) | (6U << 4) | 1U:	/* TFSRE0_EL1, S3_0_C5_C6_1 */
		if (wr)
			mte_sysregs.tfsr_el0 = rt == 31U ? 0 : regs->regs[rt];

		else
			val = mte_sysregs.tfsr_el0;
		break;

	/*
	 * ICC_IGRPEN0/1_EL1 (S3_0_C12_C12_6/7): the virtual CPU
	 * interface's group enables, mapped onto ICH_VMCR_EL2.  The
	 * write path reads xzr (rt == 31) as 0; the guard under the
	 * switch only ever writes reads.
	 */
	case (3U << 21) | (12U << 8) | (12U << 4) | 6U:	/* ICC_IGRPEN0_EL1 */
	case (3U << 21) | (12U << 8) | (12U << 4) | 7U: {	/* ICC_IGRPEN1_EL1 */
		u64 vmcr = mrs(ich_vmcr_el2);
		u64 bit = ICH_VMCR_VENG0;
		u64 src = rt == 31U ? 0 : regs->regs[rt];

		if (op2 == 7U)
			bit = ICH_VMCR_VENG1;
		if (wr) {
			if (src)
				vmcr |= bit;
			else
				vmcr &= ~bit;
			msr(ich_vmcr_el2, vmcr);
		} else {
			val = (vmcr & bit) ? 1ULL : 0ULL;
		}
		break;
	}

	default:
		/* Raw ESR too: this core's ISS is shifted 3 bits down from
		 * the published layout (the masks above compensate), so the
		 * decoded fields are only as good as that model — the raw
		 * word is the ground truth. */
		hprintf("EC=0x18 unhandled sysreg cpu%u S%u_%u_C%u_C%u_%u %s rt=x%u ESR=%#lx\n",
			   (unsigned)pcpu->id, (unsigned)op0, (unsigned)op1, (unsigned)crn,
			   (unsigned)crm, (unsigned)op2, wr ? "wr" : "rd",
			   (unsigned)rt, (unsigned long)regs->esr);
		break;
	}
	if (!wr && rt != 31U)
		regs->regs[rt] = val;
	regs->elr += 4ULL;
}

__attribute__((noreturn))
void __hyp elx_return(struct pt_regs *regs)
{
	/*
	 * The frame _hv_vector_entry built is a struct pt_regs: spsr, elr,
	 * esr, then x0..x30 (arch/arm/include/asm/proc-armv/ptrace.h).
	 * Handlers change regs->elr / regs->regs[] in C, so the values
	 * have to be read back out of memory here - that is the whole
	 * point of the frame.  Pop it with post-indexed loads starting at
	 * the low end (pre-indexed [sp, #16]! skips the first pair and
	 * shifts every guest register and ELR/SPSR by one slot).
	 */
	asm volatile(
		"mov	sp, %0\n"
		"ldp	x3, x2, [sp], #16\n"
		"ldp	x1, x0, [sp], #16\n"
		"msr	spsr_el2, x3\n"
		"msr	elr_el2, x2\n"
		"msr	esr_el2, x1\n"
		"ldp	x1, x2, [sp], #16\n"
		"ldp	x3, x4, [sp], #16\n"
		"ldp	x5, x6, [sp], #16\n"
		"ldp	x7, x8, [sp], #16\n"
		"ldp	x9, x10, [sp], #16\n"
		"ldp	x11, x12, [sp], #16\n"
		"ldp	x13, x14, [sp], #16\n"
		"ldp	x15, x16, [sp], #16\n"
		"ldp	x17, x18, [sp], #16\n"
		"ldp	x19, x20, [sp], #16\n"
		"ldp	x21, x22, [sp], #16\n"
		"ldp	x23, x24, [sp], #16\n"
		"ldp	x25, x26, [sp], #16\n"
		"ldp	x27, x28, [sp], #16\n"
		"ldp	x29, x30, [sp], #16\n"
		"eret"
		:: "r"(regs) : "memory"
	);

	/* eret does not return to this C frame */
	__builtin_unreachable();
}

/**
 * hyp_guest_wait() - emulate the guest sleeping until something wakes it
 * @timed: stop at @deadline as well (WFIT, or a CPU_SUSPEND with a timeout)
 * @deadline: CNTVCT_EL0 value at which a timed wait ends
 *
 * Used by the WFI trap and by PSCI CPU_SUSPEND (hyp/psci.c).  Wake events: a
 * physical interrupt at the CPU interface, a virtual one pending in a list
 * register or queued for this core by another one (hyp_gic_wake_pending()),
 * or the timeout.  Whatever woke it is injected into the guest on the way out.
 */
void __hyp hyp_guest_wait(bool timed, u64 deadline)
{
	while (!hyp_gic_irq_pending()) {
		if (hyp_gic_wake_pending())
			break;
		if (timed && mrs(cntvct_el0) >= deadline)
			break;
		asm volatile("isb");
	}

	hyp_gic_sync();
	hyp_dwc3_tick();
}

/*
 * WFI / WFIT from the guest (EC=0x01, HCR_EL2.TWI).  WFE and WFET are not
 * trapped (TWE stays clear).
 *
 * The guest's idle loop runs on this.  Instead of sleeping in a hardware WFI
 * (see the TWI comment in hv_start_vm()), hyp_guest_wait() polls until
 * something the guest would have woken on is pending; a WFIT also ends at the
 * timeout in the register named by ISS.Rn, compared with the virtual count.
 * The cost is a busy core while the guest is idle.
 *
 * ESR_EL2.ISS for EC=0x01 (confirmed on this core): TI[1:0] 0 WFI, 1 WFE,
 * 2 WFIT, 3 WFET; RV bit 2 (Rn valid); Rn [9:5].
 */
static void __hyp handle_wfx(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u32 ti = regs->esr & 3U;
	u32 rn = (regs->esr >> 5) & 31U;
	u64 deadline = (regs->esr & BIT(2)) && rn != 31U ? regs->regs[rn] : 0;

	regs->elr += 4ULL;
	if (ti == 1U || ti == 3U)
		return;

	hyp_guest_wait(ti == 2U, deadline);
}

/*
 * Reflect a synchronous exception into the guest's EL1, the way the CPU itself would have
 * taken it with no EL2 in the way: ESR_EL1 = @esr, FAR_EL1 = @far, and the guest's
 * ELR_EL1/SPSR_EL1 are filled in.  PSTATE is set up as an exception entry to EL1h (DAIF
 * masked, PAN per SCTLR_EL1.SPAN) and the guest resumes at its own VBAR_EL1 vector: Current
 * EL with SP_ELx +0x200, Current EL with SP_EL0 +0x000, Lower EL (EL0) +0x400.  The caller
 * must not advance ELR: the instruction is not retried, the handler decides.
 */
void __hyp hyp_inject_sync(struct pt_regs *regs, u64 esr, u64 far)
{
	u64 old = regs->spsr;
	u32 from = old & PSR_MODE_MASK;
	u64 off = from == PSR_MODE_EL0t ? 0x400 : from == PSR_MODE_EL1t ? 0x0 : 0x200;
	u64 new = PSR_MODE_EL1h | PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;

	if (!(mrs(sctlr_el1) & BIT(23)))		/* SCTLR_EL1.SPAN == 0: PAN is set on entry */
		new |= BIT(22);				/* PSTATE.PAN */

	msr(esr_el1, esr);
	msr(far_el1, far);
	msr(elr_el1, regs->elr);
	msr(spsr_el1, old);
	regs->elr = mrs(vbar_el1) + off;
	regs->spsr = new;
}

/*
 * Reflect an abort the hypervisor cannot resolve: a synchronous *external* abort
 * (IFSC/DFSC 0b010000), which is what a guest sees when it touches something the bus does
 * not answer.  @ec is the EC EL2 saw: 0x20 instruction abort, 0x24 data abort, both "from a
 * lower EL" - the guest's EC is the same-EL one (0x21/0x25) unless it was EL0.
 */
static void __hyp hyp_inject_abort(struct pt_regs *regs, u32 ec, u64 far)
{
	u64 esr_ec = (ec == 0x20 ? 0x20 : 0x24) + ((regs->spsr & PSR_MODE_MASK) == PSR_MODE_EL0t ? 0 : 1);

	hyp_inject_sync(regs, (esr_ec << 26) | BIT(25) | 0x10, far);	/* IL=1, ISS = synchronous external abort */
}

/*
 * A debug exception (EC 0x30-0x35, 0x3c) the guest caused and the stub did not claim: MDCR_EL2.TDE
 * brought it here, the guest's own debugger (or a BUG()'s BRK) is owed it.  The guest sees the same
 * class the hardware would have raised without EL2: the lower-EL codes are for EL0, the next one up
 * for EL1; BRK has just the one.
 */
static void __hyp hyp_reflect_debug(struct pt_regs *regs)
{
	u32 ec = FIELD_GET(ESR_EC, regs->esr);

	if (ec != 0x3c && (regs->spsr & PSR_MODE_MASK) != PSR_MODE_EL0t)
		ec++;

	hyp_inject_sync(regs, ((u64)ec << 26) | (regs->esr & GENMASK(25, 0)), mrs(far_el2));
}

/*
 * Queue a virtual SError for the guest: VSESR_EL2 carries the syndrome the
 * guest reads back in ESR_EL1 (FEAT_RAS only; without it the register does not
 * exist and the guest gets a bare SError), HCR_EL2.VSE raises it.  VSESR_EL2 is
 * S3_4_C5_C2_3 - no assembler here knows the name.
 */
static void __hyp hyp_inject_serror(u64 esr)
{
	if (FIELD_GET(GENMASK(31, 28), mrs(id_aa64pfr0_el1)))	/* ID_AA64PFR0_EL1.RAS */
		msr(S3_4_C5_C2_3, esr & GENMASK(24, 0));
	msr(hcr_el2, mrs(hcr_el2) | HCR_VSE);
	asm volatile("isb");
}

__attribute__((noreturn))
void __hyp elx_sync_handler(struct pt_regs *regs)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(true);
	u32 ec = FIELD_GET(ESR_EC, regs->esr);
	u32 insn = 0;
	bool have_insn = false;

	pcpu->vcpu.regs = regs;
	hv_trace(pcpu);
	/* See what the guest did with the virtual interrupts we injected. */
	hyp_gic_sync();
	hyp_gdb_park(pcpu);
	hyp_dwc3_tick();
	switch (ec) {
	case 0x01:   /* WFI/WFIT trapped by HCR_EL2.TWI */
		handle_wfx(pcpu);
		break;
	case 0x17:   /* SMC from AArch64 EL1, trapped by HCR_EL2.TSC (hyp/psci.c) */
		hyp_handle_smc(pcpu);
		break;
	case 0x16:   /* HVC from AArch64 EL1 */
		handle_hvc(pcpu);
		break;
	case 0x18:
		/* Trapped MSR/MRS (EC=0x18). See handle_sysreg_trap. */
		handle_sysreg_trap(pcpu);
		break;
	case 0x20:   /* Instruction abort from lower EL */
	case 0x21:   /* Instruction abort at EL2: our own fetch faulted */
		/*
		 * A fetch that could not be completed: a hole in the
		 * stage-2 identity map (or, for 0x21, in EL2's own
		 * stage-1).  Map the block the fault landed in and let the
		 * fetch run again; a fault the maps do not explain is not
		 * ours to fix, and not a load/store the data-abort emulator
		 * could replay either.
		 */
		if (hyp_s2_map_fault(pcpu) == 0)
			break;   /* mapped: elx_return() re-executes the fetch */
		goto unresolved;
	case 0x24:   /* Data Abort from lower EL -> stage-2 fault emulation */
		/* Only the data-abort path reads the trapping instruction: it is
		 * a guest-table walk, so the other exits do not pay for one.
		 * Once the guest's MMU is on, ELR_EL2 is a guest virtual
		 * address and the word may only be read through
		 * hyp_fetch_guest_insn(), never through EL2's own stage-1
		 * (run 3: dereferencing it at face value panicked the board). */
		have_insn = hyp_fetch_guest_insn(regs->elr, &insn) == 0;
		/*
		 * A guest cache maintenance / barrier instruction that
		 * trapped on its own address: serve it directly at EL2
		 * (the guest issues these while its MMU is still off).
		 */
		if (have_insn && !hyp_guest_mmu_on() && insn == 0xd5087620) {	/* dc ivac, x0 */
			asm volatile("dc ivac, %0" :: "r"(regs->regs[0]) : "memory");
			regs->elr += 4;
			break;
		} else if (have_insn && insn == 0xd5033f9f) {	/* dsb sy */
			asm volatile("dsb sy");
			regs->elr += 4;
			break;
		} else if (hyp_s2_emulate(pcpu) == 0) {
			break;   /* emulated: elx_return() resumes the guest */
		}
		goto unresolved;
	case 0x30:   /* breakpoint, software step and watchpoint from a lower EL, and BRK: MDCR_EL2.TDE */
	case 0x32:
	case 0x34:
	case 0x3c:
		/* the debugger stub's, or the guest's own (a BUG()'s BRK, ptrace's single step) */
		if (!hyp_gdb_debug_exception(pcpu))
			hyp_reflect_debug(regs);
		break;
	case 0x25:   /* data abort at EL2: a probed device access the bus refused (hyp/phys.c) */
		if (hyp_phys_fault(regs, true))
			break;
		goto unresolved;
	default:
		/*
		 * This core reports some stage-2 exits with a garbled EC: 0x9
		 * is reserved and ISS == 0 carries no syndrome at all, yet FAR
		 * and the instruction at ELR still describe the access.  Give
		 * the stage-2 emulator the same chance it gets under EC=0x24.
		 * Any other EC is no abort: decoding the word at ELR as a
		 * load/store would replay an access the guest never made.
		 */
		if (ec == 0x9 && hyp_s2_emulate(pcpu) == 0)
			break;	/* emulated: elx_return() resumes the guest */
unresolved:
		if (ec == 0x20 || ec == 0x24) {
			/*
			 * A real fault the maps cannot explain (external
			 * abort on a TZ-protected window, a walk off into a
			 * stage-2 hole): it belongs to the guest.  Reflect it
			 * as a synchronous external abort at EL1 - the
			 * kernel's own fault path (exception fixups, oops with
			 * a backtrace) deals with it; parking the board here
			 * hides all of that (run 58).
			 */
			if (pcpu->injected++ < 100) {
				hprintf("cpu%u: EC=%#x ESR=%#lx ELR=%#lx FAR=%#lx -> external abort injected (%u)\n",
					(unsigned)pcpu->id, (unsigned)ec,
					regs->esr, regs->elr,
					(unsigned long)mrs(far_el2),
					pcpu->injected);
				hyp_inject_abort(regs, ec, mrs(far_el2));
				break;
			}
			hprintf("cpu%u: too many aborts injected, parking\n",
				(unsigned)pcpu->id);
		}
		hprintf("Unexpected exit CPU=%u EC=%#x ESR=%#lx ELR=%#lx FAR=%#lx SPSR=%#lx\n",
			   (unsigned)pcpu->id, (unsigned)ec, regs->esr,
			   regs->elr, (unsigned long)mrs(far_el2),
			   (unsigned long)regs->spsr);
		/* The guest's registers, from the exit frame: no guest memory is
		 * touched.  show_regs() fetches code at ELR *from EL2*, which
		 * with the guest MMU on is a guest VA no EL2 map covers, and
		 * dump_pagetable() walks the guest's TTBR0 straight out of DRAM
		 * through EL2's non-cacheable view, so both are skipped: either
		 * one aborting here re-enters this handler and buries the exit
		 * that matters (run 114). */
		if (!hyp_guest_mmu_on())
			show_regs(regs);
		for (u32 i = 0; i < 30; i += 3)
			hprintf("x%u=%#lx x%u=%#lx x%u=%#lx\n",
				i, (unsigned long)regs->regs[i],
				i + 1, (unsigned long)regs->regs[i + 1],
				i + 2, (unsigned long)regs->regs[i + 2]);
		hprintf("x30=%#lx\n", (unsigned long)regs->regs[30]);
		hang();
	}

	hyp_gdb_poll(pcpu);
	hyp_gdb_park(pcpu);
	elx_return(regs);
}

/*
 * A physical interrupt was taken: HCR_EL2.IMO=1 (inherited from XBL/Gunyah)
 * routes every guest IRQ to EL2 whatever the guest's own DAIF says.  Serve it
 * through the vGIC and put whatever was interrupted back exactly where it was.
 *
 * EL2 itself keeps DAIF.I masked (every exception entry sets it and the eret
 * paths no longer clear it - see hyp_enter_guest()), so an IRQ lands here from
 * the guest, or from EL2 C code that unmasks itself on purpose (the self-test
 * suite).  _hv_vector_entry() saved every caller register and the pops in
 * elx_return() unwind exactly the pushes it made on the same stack, so popping
 * this frame resumes the interrupted code - EL1 or EL2 - exactly where it
 * stopped.
 */
void __hyp elx_irq_handler(struct pt_regs *regs)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(true);
	struct pt_regs *outer = pcpu->vcpu.regs;
	bool from_guest = (regs->spsr & PSR_MODE_MASK) <= PSR_MODE_EL1h;

	/*
	 * Record what this entry interrupted before anything else runs.  The
	 * frame below is popped by elx_return() and its stack slot is reused
	 * straight away, so these words are the only lasting answer to "did the
	 * interrupt land in the guest, or in our own EL2 code?".
	 */
	pcpu->irq_entries++;
	pcpu->irq_entry_spsr = regs->spsr;
	pcpu->irq_entry_elr = regs->elr;

	hyp_gic_sync();
	hyp_gic_irq(pcpu);
	hyp_dwc3_irq_work();

	/* An IRQ in EL2 code must not repoint vcpu.regs: the exit that EL2 code
	 * is handling still owns it. */
	pcpu->vcpu.regs = from_guest ? regs : outer;
	if (from_guest) {
		hyp_gdb_poll(pcpu);
		hyp_gdb_park(pcpu);
	}
	elx_return(regs);
}

/*
 * SError (EC 0x00, asynchronous external abort): AMO=1 routes the guest's
 * imprecise bus errors into EL2.  They are news for the guest, not for us:
 * stash them as a virtual SError (VSESR_EL2 + HCR_EL2.VSE) and resume the
 * interrupted guest.  The guest's SError handler then reports the offending
 * task and stack.  The count is capped so a device that keeps erroring cannot
 * keep the guest in its handler forever.  One taken at EL2 is our own bug and
 * is not the guest's to see.
 */
void __hyp elx_serror_handler(struct pt_regs *regs)
{
	struct hyp_percpu *pcpu = hyp_restore_percpu(true);

	if (hyp_phys_fault(regs, false)) {	/* a probed device store the bus refused */
		elx_return(regs);
		return;
	}

	if ((regs->spsr & PSR_MODE_MASK) > PSR_MODE_EL1h) {
		hprintf("cpu%u: SError at EL2 ESR=%#lx ELR=%#lx\n",
			(unsigned)pcpu->id, regs->esr, regs->elr);
		_hv_panic(regs);
	}

	pcpu->vcpu.regs = regs;
	if (pcpu->injected++ < 100) {
		hprintf("cpu%u: SError ESR=%#lx ELR=%#lx -> virtual SError (%u)\n",
			(unsigned)pcpu->id, regs->esr, regs->elr, pcpu->injected);
		hyp_inject_serror(regs->esr);
		elx_return(regs);
		return;
	}
	hprintf("cpu%u: too many SErrors, parking\n", (unsigned)pcpu->id);
	_hv_panic(regs);
}

void _hv_vector_entry(void);

__section(".text.hyp_vectors")
void __hyp_vectors(void)
{
#define VECTOR_JUMP(x) asm volatile ( \
	".align 7\n" \
	"stp	x29, x30, [sp, #-16]!\n" \
	"bl _hv_vector_entry\n" \
	"b %0" :: "i"(x) \
);
#define VECTOR_PANIC VECTOR_JUMP(_hv_panic)

	asm volatile (".align 11");
	/* Current EL with SP_EL0 */
	VECTOR_JUMP(elx_sync_handler)
	VECTOR_JUMP(elx_irq_handler)		/* IRQ */
	VECTOR_JUMP(elx_serror_handler)		/* SError */
	VECTOR_JUMP(_hv_panic)			/* FIQ */
	/* Current EL with SP_ELx */
	VECTOR_JUMP(elx_sync_handler)
	VECTOR_JUMP(elx_irq_handler)		/* IRQ */
	VECTOR_JUMP(elx_serror_handler)		/* SError at EL2 */
	VECTOR_JUMP(_hv_panic)			/* FIQ */
	/* Lower EL, AArch64 */
	VECTOR_JUMP(elx_sync_handler)
	VECTOR_JUMP(elx_irq_handler)		/* IRQ */
	VECTOR_JUMP(elx_serror_handler)		/* SError from guest */
	VECTOR_JUMP(_hv_panic)			/* FIQ */
	/* Lower EL, AArch32 */
	VECTOR_JUMP(_hv_panic)
	VECTOR_JUMP(_hv_panic)
	VECTOR_JUMP(_hv_panic)
	VECTOR_JUMP(_hv_panic)
}

__section(".text.hyp_vectors")
void _hv_vector_entry(void)
{
	// Build struct pt_regs
	asm volatile(
		"stp	x27, x28, [sp, #-16]!\n"
		"stp	x25, x26, [sp, #-16]!\n"
		"stp	x23, x24, [sp, #-16]!\n"
		"stp	x21, x22, [sp, #-16]!\n"
		"stp	x19, x20, [sp, #-16]!\n"
		"stp	x17, x18, [sp, #-16]!\n"
		"stp	x15, x16, [sp, #-16]!\n"
		"stp	x13, x14, [sp, #-16]!\n"
		"stp	x11, x12, [sp, #-16]!\n"
		"stp	x9, x10, [sp, #-16]!\n"
		"stp	x7, x8, [sp, #-16]!\n"
		"stp	x5, x6, [sp, #-16]!\n"
		"stp	x3, x4, [sp, #-16]!\n"
		"stp	x1, x2, [sp, #-16]!\n"
		"mrs	x1, esr_el2\n"
		"mrs	x2, elr_el2\n"
		"mrs	x3, spsr_el2\n"
		"stp	x1, x0, [sp, #-16]!\n"
		"stp	x3, x2, [sp, #-16]!\n"
		"mov	x0, sp\n"
	);
}
