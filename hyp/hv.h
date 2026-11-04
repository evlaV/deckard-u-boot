// SPDX-License-Identifier: GPL-2.0+


#ifndef __HV_H__
#define __HV_H__

#include <asm/global_data.h>
#include <asm/ptrace.h>
#include <hyp/vdev.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <stdbool.h>
#include <hypervisor.h>

/*
 * EL2 print helpers.  Every core on this board writes the same GENI debug
 * UART, and a plain printf is a stream of putc() calls: two cores printing at
 * once interleave byte ranges into lines nobody recognises.  hprintf()
 * therefore goes through hyp_printf(), which formats the whole line, prefixes
 * it with this file's console tag and the id of the core writing it, and puts
 * it out under a console lock.  A file picks its tag with HYP_CONSOLE_TAG
 * before including this header.  hdebug() is the per-access trace which is
 * compiled out unless the .c file does `#define LOG_DEBUG` before this
 * include (same convention as U-Boot's debug()).
 */
#ifndef HYP_CONSOLE_TAG
#define HYP_CONSOLE_TAG		"[HYP] "
#endif

/* hyp/log.c: the ring hyp_printf() fills for the :2222 console, and the switch for the UART */
void hyp_log_put(const char *line, u32 len);
u32 hyp_log_take(char *buf, u32 size);
u32 hyp_log_lost(void);
bool hyp_log_uart(void);
bool hyp_log_uart_set(bool on);
int hyp_cpu_num(void);

#define hprintf(fmt, ...) hyp_printf(HYP_CONSOLE_TAG, fmt, ##__VA_ARGS__)

#ifdef LOG_DEBUG
#define hdebug(fmt, ...) hprintf(fmt, ##__VA_ARGS__)
#else
#define hdebug(fmt, ...) do { } while (0)
#endif

/* Slots in hyp/hv.c's percpu[] and hyp/mem.c's stack array */
#define HYP_MAX_CPUS		8

/*
 * The affinity the DT's reg cells and PSCI's CPU_ON use: Aff2:Aff1:Aff0.
 * This board's PEs report MPIDR_EL1 = 0x81000000 - MT (bit 24) set, "affinity
 * levels not valid", Aff2:Aff1:Aff0 all zero - so the raw MPIDR is the same on
 * every core and is not an identity; mask it to the DT's affinity before
 * comparing.  See hyp/hv.c's hyp_percpu_init_aff().
 */
#define MPIDR_AFFINITY		GENMASK(23, 0)
#define MPIDR_UPT		BIT(31)		/* PE is up and running */

/*
 * The identity a lock's owner field stores: MPIDR_EL1 Aff1:Aff0, +1 so that 0
 * stays "unowned".  This board is a single cluster with Aff0 < 256, so
 * Aff1:Aff0 identifies the calling PE (CPU0 -> 1, CPU1 -> 0x101).
 */
#define MUTEX_AFFINITY		GENMASK(15, 0)

struct vcpu {
	struct pt_regs *regs;
	int id;
};

struct hyp_percpu {
	phys_addr_t sp;
	struct vcpu vcpu;
	int id;
	u64 mpidr;					/* affinity as the DT gives it */
	u32 exits;					/* guest->EL2 entries, for hv_trace() */
	/*
	 * What the last IRQ entry at this core interrupted.  The exception
	 * frame itself is popped by elx_return() and the stack slot is reused
	 * immediately, so pcpu->vcpu.regs is a dangling pointer the moment the
	 * handler has returned; these three words are what survives, and they
	 * are how a caller tells "the interrupt returned into our EL2 code"
	 * from "it returned into the guest".
	 */
	u32 irq_entries;				/* IRQ entries served by this core */
	u64 irq_entry_spsr;				/* SPSR_EL2 of the last one */
	u64 irq_entry_elr;				/* ELR_EL2 of the last one */
	/*
	 * Faults and SErrors handed back to the guest (hyp/hv.c): synchronous
	 * aborts the maps cannot resolve and virtual SErrors.  Counted so a
	 * guest that keeps faulting on the same access cannot loop forever.
	 */
	u32 injected;
	gd_t *global_data;
};

/* hv.c: U-Boot relocation offset, stashed by hv_start_vm() so EL2 trap
 * prints can show ELR as a link-time address (./scripts/elrdecode input). */
u64 hyp_reloc_off(void);

/* hyp/hv.c: the HCR_EL2 hv_start_vm() settled on, for a secondary core */
u64 hyp_hcr_value(void);
/* hyp/hv.c: per-core EL2 controls the guest relies on (CPTR/MDCR/CNTHCTL/VPIDR...) */
void hyp_guest_ctl_init(void);
bool hyp_gic_wake_pending(void);	/* hyp/gic.c: a WFI would end: a virtual IRQ sits in a list register or another vCPU queued an SGI */
bool hyp_gic_irq_pending(void);	/* hyp/gic.c: a physical interrupt (an LPI too) waits at the CPU interface */
void hyp_guest_wait(bool timed, u64 deadline);	/* hyp/hv.c: emulate WFI/WFIT/CPU_SUSPEND for the guest */
void hyp_inject_sync(struct pt_regs *regs, u64 esr, u64 far);	/* hyp/hv.c: reflect a synchronous exception into the guest's EL1 */
void hyp_handle_smc(struct hyp_percpu *pcpu);	/* hyp/psci.c: guest SMC (EC=0x17) */

/* hyp/hv.c: the hypervisor's own gd, so a core PSCI has just woken - which
 * has nothing useful in x18 - can run hyp code */
gd_t *hyp_root_gd(void);

/* hyp/hv.c: per-cpu context of the calling core (hyp_cpu_init() first).
 * hyp_percpu_init() identifies this core by its own MPIDR; hyp_percpu_init_aff()
 * takes the affinity PSCI named it by, for a core we woke ourselves. */
void hyp_percpu_init(void);
void hyp_percpu_init_aff(u64 mpidr);

void hyp_set_gd_ptr(gd_t *g);

/* hyp/hv.c: this core's context, as stashed in TPIDR_EL2 */
struct hyp_percpu *hyp_restore_percpu(bool restore_gd);

/* hyp/hv.c: hand the guest its first instruction - park the synthesized
 * frame on the per-cpu EL2 stack and eret.  guest_x0 is the guest's x0 at the
 * first instruction: the PSCI context id for a Linux secondary, 0 otherwise. */
void hyp_enter_guest(struct hyp_percpu *pcpu, u64 guest_x0);

/* hyp/hv.c: our EL2 vectors, and U-Boot's own exception vectors
 * (arch/arm/cpu/armv8/exceptions.S).  The guest's VBAR_EL1 is `vectors` plus
 * the relocation offset; exceptions.S picks the exception level at runtime,
 * so the guest keeps native crash reporting instead of faulting into EL2. */
void __hyp_vectors(void);
extern char vectors[];

/*
 * Clean (and invalidate) a range so a core that starts with its caches off -
 * PSCI hands a secondary over with MMU and caches disabled - sees it in
 * memory, and so a core whose caches may hold a stale copy re-reads it.
 */
static inline void hyp_clean_range(const void *p, u64 len)
{
	const u64 line = 64;
	u64 addr = (u64)p & ~(line - 1);
	u64 end = ((u64)p + len + line - 1) & ~(line - 1);

	for (; addr < end; addr += line)
		asm volatile("dc civac, %0" : : "r"(addr) : "memory");

	asm volatile("dsb ish" : : : "memory");
}


/* hyp/cpu.c: the DT's /cpus affinity -> slot map */
void hyp_cpu_init(void);
int hyp_cpu_id(u64 mpidr);
u64 hyp_cpu_mpidr(int slot);
int hyp_cpu_count(void);

/* hyp/mem.c: bump allocator over the DTB hyp_mem reservation */
void hyp_mem_init(void);
void *hyp_mem_alloc(u64 size);
u64 hyp_mem_stack(int id);

/* hyp/s1.c: EL2 stage-1 identity map, built and enabled before eret */
void hyp_s1_enable(void);
int hyp_s1_apply(void);			/* publish the built tables on this core */
bool hyp_s1_covers(u64 pa);		/* is @pa inside EL2's stage-1 map? */
int hyp_s1_map_pa(u64 pa);		/* map the 2 MiB block holding @pa at EL2 */
/* hyp/mmio.c: stage-2 identity map, data-abort emulation, MMIO spy */
void hyp_s2_init(void);
void hyp_s2_install(void);
int hyp_s2_apply(void);			/* point this core at the built tables */

void hyp_mmio_spy(u64 start, u64 end);
void hyp_mmio_unspy(u64 start, u64 end);
int hyp_s2_emulate(struct hyp_percpu *pcpu);
bool hyp_guest_is_uboot(const struct pt_regs *regs);	/* hyp/mmio.c: is this guest frame U-Boot's own code, not the OS's */
int hyp_s2_block(u64 pa, u64 *desc);	/* the block descriptor covering pa */
int hyp_s2_map_fault(struct hyp_percpu *pcpu);	/* map a stage-2 fault away */
/*
 * The guest's own stage-1 walk.  Once the guest turns its MMU on, ELR_EL2
 * and FAR_EL2 are guest virtual addresses: nothing in the trap path may be
 * read at face value, because EL2's stage-1 map covers only the hypervisor's
 * own layout and reading a guest VA through it faults at EL2 (run 3,
 * scripts/EXCEPTION_DEBUGGING.md).
 */
int hyp_guest_va_to_pa(u64 va, u64 *pa, u64 *pte);
int hyp_fetch_guest_insn(u64 elr, u32 *insn);
int hyp_guest_read(u64 va, void *buf, u32 len);		/* hyp/mmio.c: copy guest memory out */
int hyp_guest_write(u64 va, const void *buf, u32 len);	/* and in, with the cache maintenance code needs */

/* hyp/phys.c: physical memory (DRAM and device registers) from EL2.  A device access that the
 * bus refuses fails with -EFAULT instead of taking the hypervisor down. */
void hyp_phys_init(void);					/* collect the register windows the DT describes */
int hyp_phys_read(u64 pa, void *buf, u32 len);
int hyp_phys_write(u64 pa, const void *buf, u32 len);
int hyp_phys_probe32(u64 addr, u32 *val);			/* one probed word read, for the self-tests */
bool hyp_phys_fault(struct pt_regs *regs, bool precise);	/* hv.c: an EL2 abort, is it a probe's? */

/* hyp/spmi.c: the PMIC arbiter's debug bus, a channel to the PMICs of EL2's own (scripts/SPMI.md).
 * Addresses are bus, slave id, peripheral and register; errors are negative errnos below 100. */
void hyp_spmi_init(void);					/* find the bus in the DT */
bool hyp_spmi_present(void);
void hyp_spmi_connect(void);					/* a client connected: vote the QDSS clock on again */
int hyp_spmi_read(u64 addr, void *buf, u32 len);
int hyp_spmi_write(u64 addr, const void *buf, u32 len);

/*
 * The MMIO watch API (hyp_mmio_watch_fn, hyp_mmio_watch(),
 * hyp_mmio_unwatch()) is public and declared in <hypervisor.h>: watchers live
 * with the driver that owns the device, e.g. the RPMH spy in
 * drivers/soc/qcom/rpmh-rsc.c.
 */

/*
 * Stage-2 block-descriptor bits (DDI0487M Table D8-53 / D8-96).  Public
 * because hyp_s2_block() hands a descriptor back: reading what watching did
 * to the map - a block taken out and put back with its attributes intact -
 * means decoding one, and the map's owner is not the only reader.
 */

#define S2_VALID	(1ULL << 0)		/* block: [1:0] = 0b01 */
#define S2_TABLE	(3ULL << 0)		/* table: [1:0] = 0b11 */
#define S2_MEMATTR(m)	(((u64)(m) & 0xfULL) << 2)
#define S2_DEV		S2_MEMATTR(0x2)	/* Device-nGRE */
#define S2_WB		S2_MEMATTR(0xf)	/* Normal, Inner+Outer WB (HCR_EL2.FWB=0 encoding, D8-96) */
#define S2_RW		(3ULL << 6)		/* S2AP[1:0] = RW */
#define S2_AF		(1ULL << 10)
#define S2_SH_INNER	(3ULL << 8)	/* SH[1:0] -> [9:8] */
#define S2_XN		(1ULL << 54)
#define S2_OA_MASK	0x0000fffffffff000ULL	/* table descriptor OA [47:12] */

/*
 * ICC_SGI1R_EL1 (DDI0487M C5.2.105): TargetList[15:0] is a bitmask of Aff0
 * values (16 * RS + bit) within the Aff3:Aff2:Aff1 level the other fields
 * name, IRQ[27:24], IRM 40.  Part of the API because hyp_gic_emulate_sgi1r()
 * takes this value: a caller that aims an SGI has to build it.
 */
#define ICC_SGI1R_TARGET	GENMASK(15, 0)
#define ICC_SGI1R_AFF1		GENMASK(23, 16)
#define ICC_SGI1R_IRQ		GENMASK(27, 24)
#define ICC_SGI1R_AFF2		GENMASK_ULL(39, 32)
#define ICC_SGI1R_IRQMODE	BIT_ULL(40)
#define ICC_SGI1R_RS		GENMASK_ULL(47, 44)
#define ICC_SGI1R_AFF3		GENMASK_ULL(55, 48)

/* hyp/gic.c: GICv3 bring-up, the guest's virtual CPU interface, the EL2 IRQ
 * path and the ICC_SGI1R_EL1 emulation (see scripts/VGIC.md) */
void hyp_gic_init(void);
void hyp_gic_irq(struct hyp_percpu *pcpu);
void hyp_gic_sync(void);
int hyp_gic_emulate_sgi1r(u64 val);
void hyp_gic_doorbell(int slot);	/* make the core in @slot exit to EL2 (an SGI nothing else receives) */
void hyp_gic_vspi_raise(int intid);	/* raise a virtual SPI (hyp/vdev.h) on the calling core's guest */
int hyp_gic_cpuif(u64 mpidr);		/* CPU interface of a secondary core */
u64 hyp_gic_read_lr(int n);			/* one list register's contents */
void hyp_gic_write_lr(int n, u64 val);
int hyp_gic_console_irq(void);		/* console UART INTID this board has */
void hyp_gic_arm_console_irq(void);	/* re-arm it after a catch disarms it */
int hyp_gic_nr_lrs(void);			/* list registers this GIC has */
u64 hyp_gic_dist_base(void);		/* distributor base, from the DT */
void hyp_gic_rdist_region(u64 *span, u64 *stride);	/* the DT's GICR region */
u64 hyp_gic_rdist_of(u64 mpidr);	/* bank a core's affinity owns, 0 = none */
u64 hyp_gic_rdist_self(void);		/* CPU0's bank, as bring-up found it */
int hyp_gic_nr_intids(void);		/* highest INTID the distributor has */

/*
 * HCR_EL2 as hv_start_vm() settles it (DDI0487M C5.2.12; every position
 * checked with ./scripts/isa.py reg/fields, never by hand).  Shared because the
 * finished value is part of the hypervisor's contract with its guests:
 * hyp/smp.c replays hyp_hcr_value() onto every core it wakes, and hyp/test.c
 * asserts the field set.
 */
#define HCR_VM		BIT(0)		/* the guest is a VM: stage-2 translation on */
#define HCR_SWIO	BIT(1)		/* named SWIO in ./scripts/isa.py's layout; an XBL/Gunyah leftover */
#define HCR_PTW		BIT(2)		/* stage-2 walks may read memory */
#define HCR_FMO		BIT(3)		/* physical FIQ is taken at EL2 */
#define HCR_IMO		BIT(4)		/* physical IRQ is taken at EL2 */
#define HCR_AMO		BIT(5)		/* physical SError is taken at EL2 */
#define HCR_VF		BIT(6)		/* FIQ injected to the guest by HCR itself */
#define HCR_VI		BIT(7)
#define HCR_VSE		BIT(8)
#define HCR_FB		BIT(9)		/* force guest stage-1 execute-never */
#define HCR_BSU_MASK	GENMASK(11, 10)	/* barrier shareability */
#define HCR_BSU_IS	FIELD_PREP(HCR_BSU_MASK, 1)	/* Inner Shareable */
#define HCR_DC		BIT(12)		/* guest stage-1 mappings default cacheable */
#define HCR_TWI		BIT(13)		/* trap guest WFI */
#define HCR_TWE		BIT(14)		/* trap guest WFE */
#define HCR_TID_MASK	GENMASK(18, 15)	/* TID1..3 (18..16) plus bit 15: TID0, or
					 * HCR_TTL when the core has FEAT_TTL (this one
 * does); cleared either way, so the guest reads real ID registers */
#define HCR_TSC		BIT(19)		/* trap guest SMC at EL2 */
#define HCR_TIDCP	BIT(20)		/* trap unallocated group-3 sysreg accesses */
#define HCR_TACR	BIT(21)		/* trap guest ACTLR/CPACR accesses */
#define HCR_TSW		BIT(22)		/* trap guest cache maintenance by set */
#define HCR_TPCP	BIT(23)		/* trap guest CP15 barrier instructions */
#define HCR_TPU		BIT(24)		/* trap guest prefetch and cache ops */
#define HCR_TTLB	BIT(25)		/* trap guest TLBI */
#define HCR_TVM		BIT(26)		/* trap guest virtual-memory sysregs */
#define HCR_TGE		BIT(27)		/* EL2 is running natively, not as a VM */
#define HCR_TDZ		BIT(28)		/* trap guest DC ZVA */
#define HCR_TRVM	BIT(30)		/* trap guest reads of virtual-memory regs */
#define HCR_RW		BIT(31)		/* the guest is AArch64 */
#define HCR_CD		BIT(32)		/* trap guest debug sysregs */
#define HCR_ID		BIT(33)		/* trap guest implementation-defined sysregs */
#define HCR_FGT		BIT(35)		/* fine-grained traps, per HFG{R,W,I}TR_EL2 */
#define HCR_APK		BIT(40)		/* 0: PAuth key register accesses trap, EC=0x18 */
#define HCR_API		BIT(41)		/* 0: pac/aut PAC instructions trap, EC=0x9 */
/*
 * HCR_ATA: hv_start_vm deliberately keeps this 0.  ATA == 1 makes the
 * guest's GCR_EL1/RGSR_EL1/TFSR_EL1/TFSRE0_EL1 accesses native - but
 * run-22 warm-reset the board (silently; XBL's next banner logged
 * "PM: Reset by PSHOLD") the moment Linux enabled tagging at EL1&0.
 * With ATA == 0 those four accesses trap as EC=0x18 (DDI0487M
 * D24.2.104) and handle_sysreg_trap answers them from a shadow.
 */
#define HCR_ATA		BIT(56)		/* MTE tagging override for EL1&0 */
#define HCR_NV		BIT(58)		/* the guest's EL1 behaves as a nested hypervisor */

/*
 * ICH_LR<n>_EL2 (DDI0487M C5.2.112): State[63:62], HW 61, Group 60,
 * Priority[55:48], pINTID[44:32], vINTID[31:0] (we use [15:0]).  The list
 * registers are the guest's view of its interrupts, so the field layout is
 * part of the hypervisor API, not private to hyp/gic.c.
 */
#define LR_PENDING		BIT(62)
#define LR_ACTIVE		BIT(63)
#define LR_STATE_MASK		(LR_PENDING | LR_ACTIVE)
#define LR_HW			BIT(61)		/* backed by a physical interrupt */
#define LR_GRP1			BIT(60)		/* IRQ to the guest, not FIQ */
#define LR_PRIORITY(p)		(((u64)(p) & 0xffULL) << 48)
#define LR_PINTID(i)		(((u64)(i) & 0x1fffULL) << 32)
#define LR_VINTID(i)		((u64)(i) & 0xffffULL)
#define LR_GET_VINTID(lr)	((int)((lr) & 0xffffULL))
#define LR_GET_PRIORITY(lr)	(((lr) >> 48) & 0xffULL)
#define LR_GET_PINTID(lr)	(((lr) >> 32) & 0x1fffULL)

/*
 * The virtual CPU interface as the hypervisor configures it (DDI0487M
 * C5.2.115-117), decoded with ./scripts/isa.py reg/fields, never by hand.  Part of
 * the hypervisor's API because a guest's view of its interrupts depends on it.
 */
#define ICC_SRE_SRE		BIT(0)		/* EL2 uses the sysreg interface */
#define ICC_SRE_ENEL1		BIT(3)		/* EL1 gets the virtual interface */
#define ICC_CTLR_EOIMODE	BIT(1)		/* EOIR drops priority only; ICC_DIR deactivates */

#define ICH_VTR_LISTREGS	GENMASK(4, 0)	/* list regs implemented = n + 1 */
#define ICH_VTR_PRIBITS		GENMASK(31, 29)	/* priority levels = n + 1 */

#define ICH_VMCR_VPMR		GENMASK(31, 24)	/* virtual priority mask */
#define ICH_VMCR_VEOIM		BIT(9)		/* virtual EOImode */
#define ICH_VMCR_VCBPR		BIT(4)
#define ICH_VMCR_VENG1		BIT(1)		/* virtual Group 1 (IRQ) enabled */
#define ICH_VMCR_VENG0		BIT(0)

#define ICH_HCR_EN		BIT(0)		/* virtual interface on */
#define ICH_HCR_UIE		BIT(1)		/* list-register update interrupt */

/* Priority for everything we inject: 0 (highest) so the virtual interface
 * always signals it - the guest's VMCR.Group*PMR (observed 0x4c/0xf0) masks
 * any numerically higher injected priority, which left injected list
 * registers pending but never signalled (apps_rsc completion IRQ deadlock). */
#define HYP_IRQ_PRIO		0x00

/* ICC_IAR1_EL1 returns a 25-bit INTID; 1020-1023 are the spurious/interrupt
 * numbers (1023 also means "no interrupt", e.g. a maintenance wake-up). */
#define ICC_IAR_INTID		GENMASK(24, 0)
#define ICC_IAR_SPURIOUS	1020

/* hyp/smp.c: secondary cores as vCPUs (scripts/SMP.md).  hyp_smp_hvc() is called
 * with the EL2 console lock already held. */
void hyp_smp_init(void);
int hyp_smp_start(u64 mpidr);
int hyp_smp_start_payload(u64 mpidr, void *entry);	/* vCPU with a chosen .text payload */
long hyp_smp_guest_cpu_on(u64 target, u64 entry, u64 ctx);	/* PSCI CPU_ON from the guest: PSCI code */
long hyp_smp_guest_cpu_off(void);	/* PSCI CPU_OFF from the guest; returns only on refusal */
void hyp_smp_hvc(struct hyp_percpu *pcpu, u64 fn, struct pt_regs *regs);
void hyp_smp_console_lock(void);
void hyp_smp_console_unlock(void);

/* hyp/usb.c: the USB controller EL2 takes from the guest (scripts/USB_GADGET.md).
 * hyp_usb_init() reads the DT at boot; hyp_usb_takeover() is the HVC the guest
 * issues once it has released the controller (called with the console lock held). */
#if IS_ENABLED(CONFIG_HYP_USB)
void hyp_usb_init(void);
long hyp_usb_takeover(struct hyp_percpu *pcpu);
void hyp_dwc3_poll(void);	/* hyp/dwc3.c: drain the controller's events */
void hyp_dwc3_tick(void);	/* hyp/dwc3.c: a guest exit: timers, and the events when polled */
void hyp_dwc3_irq_work(void);	/* hyp/dwc3.c: the work its interrupt left, after hyp_gic_irq() */
bool hyp_usb_boot_waiting(void);	/* hyp/usb.c: the guest is halted until `boot` */
bool hyp_usb_boot_release(void);	/* hyp/usb.c: `boot`; false when it was not waiting */
#else
static inline void hyp_usb_init(void) { }
static inline long hyp_usb_takeover(struct hyp_percpu *pcpu) { return -ENODEV; }
static inline void hyp_dwc3_poll(void) { }
void hyp_vdev_poll(void);	/* hyp/vdev.c: the network's timers, with no controller to poll */
static inline void hyp_dwc3_tick(void) { hyp_vdev_poll(); }
static inline void hyp_dwc3_irq_work(void) { }
static inline bool hyp_usb_boot_waiting(void) { return false; }
static inline bool hyp_usb_boot_release(void) { return false; }
#endif

/* hyp/gdb.c: the GDB server for the guest.  hyp_gdb_debug_exception() takes a debug exception
 * (BRK, software step, breakpoint, watchpoint) from the guest and returns false when it is not
 * the stub's, hyp_gdb_poll() enters the stub when the client asked for a stop, and
 * hyp_gdb_park() holds a core while another one serves the client. */
bool hyp_gdb_debug_exception(struct hyp_percpu *pcpu);
void hyp_gdb_poll(struct hyp_percpu *pcpu);
void hyp_gdb_park(struct hyp_percpu *pcpu);

/* hyp/timer.c: delays and timeouts on the generic counter, for EL2 code */
void hyp_timer_init(void);
u64 hyp_ticks(void);
u64 hyp_ticks_us(void);
bool hyp_ticks_elapsed(u64 since, u32 us);
void hyp_udelay(u32 us);

/* hyp/rpmh.c: EL2's own RPMH voter, on the DRV the RSC names `hypervisor-drv-id`.
 * The command DB is resolved at boot; votes can be sent any time after. */
void hyp_rpmh_init(void);
bool hyp_rpmh_ready(void);
int hyp_rpmh_write(u32 addr, u32 data);
int hyp_rpmh_read(u32 addr, u32 *data);

/* hyp/res.c: the system resources EL2 can look at and switch (scripts/RESOURCES.md) */
void hyp_res_init(void);
u32 hyp_res_count(void);
const char *hyp_res_name(int idx);
int hyp_res_find(const char *name);
const char *hyp_res_guard(int idx);
int hyp_res_value(int idx, u32 *val, bool force);
int hyp_res_describe(int idx, char *buf, size_t len, bool force);
int hyp_res_enable(int idx, bool on, bool force);
int hyp_res_set(int idx, const char *field, u64 val, bool force);
u32 hyp_res_snapshot(bool force);
int hyp_res_diff_line(u32 *cursor, char *buf, size_t len, bool force);
int hyp_res_label(int idx, char *buf, size_t len);

/* hyp/smmu.c: the stream mappings U-Boot made are dropped when the guest hands over to the OS
 * (HV_OS_HANDOFF), so the OS's SMMU driver finds the SMMU as the firmware left it */
void hyp_smmu_init(void);
int hyp_smmu_handoff(void);

/* One-time setup: buffers, the TCP services */
void hyp_net_init(void);

/* hyp/pas.c: the secure world's half of loading a remote processor, for the processors the DT
 * tags `hypervisor-pas`.  hyp_pas_smc() answers a guest SCM call, or returns false when the call
 * is not one of EL2's and goes on to the secure world. */
void hyp_pas_init(void);
void hyp_pas_describe(char *buf, size_t len);
bool hyp_pas_core_up(void);
bool hyp_pas_toggle_smoke(void);
bool hyp_pas_toggle_tz(void);
void hyp_pas_set_run_ms(u32 ms, u32 nmi_ms, u32 nmi_phase);
void hyp_pas_set_watch_ms(u32 ms);
void hyp_pas_set_poll_ms(u32 ms);
void hyp_pas_set_ring(u64 pa, u32 ms);
void hyp_pas_ropll_stress(void);
void hyp_smmu_watch(u32 ms, u32 sid_lo, u32 sid_hi);
void hyp_pas_set_ro_pll(u32 ms);
void hyp_pas_set_ctx_ms(u32 ms);
int hyp_smmu_ctx_enable(u64 mem, u64 size);
void hyp_smmu_ctx_service(u32 ms);
void hyp_smmu_ctx_disable(void);
void hyp_smmu_set_vmid(u32 vmid);
bool hyp_pas_toggle_leave(void);
void hyp_pas_scan(void);
#if IS_ENABLED(CONFIG_HYP_TEST)
void hyp_pas_test_inject(u32 id, u32 dtb_id);
void hyp_pas_test_clear(void);
u64 hyp_pas_test_entry(u32 id);
#endif
void hyp_smmu_describe(char *buf, size_t len);
bool hyp_smmu_toggle_fault(void);
bool hyp_pas_smc(struct pt_regs *regs);

struct hyp_vcpu_info {			/* one vCPU row, as memory holds it */
	u64 mpidr;
	bool started, running, parked, failed;
	u32 heartbeats;
};
int hyp_smp_info(int slot, struct hyp_vcpu_info *info);	/* 0, or -EINVAL */
bool hyp_smp_vcpu_on(int slot);	/* the guest has this vCPU running */

#if IS_ENABLED(CONFIG_HYP_TEST)
/* hyp/test.c: the self-test suite (scripts/TESTING.md).  hyp_test_run() runs the
 * whole table from hv_start_vm(), at EL2; hyp_test_hvc() takes a stage result
 * reported by the EL1 test core, with the console lock already held. */
void hyp_test_run(struct hyp_percpu *pcpu);
void hyp_test_hvc(struct hyp_percpu *pcpu, struct pt_regs *regs);
void hyp_test_secondary(struct hyp_percpu *pcpu);	/* a started core's job, at EL2 */
#endif

#define __str(def)	#def

#define mrs(reg)							\
({									\
	unsigned long __mrs_val;					\
	asm volatile("mrs %0, " __str(reg) : "=r" (__mrs_val));		\
	__mrs_val;							\
})

#define msr(reg, val)							\
do {									\
	unsigned long __msr_val = val;					\
	asm volatile("msr " __str(reg) ", %0" : : "r" (__msr_val));	\
} while (0)

/*
 * Is the guest running with its stage-1 MMU on?  With it off, every address
 * the guest uses - ELR, FAR, its page tables - is already a physical address
 * and our identity maps reach it directly.  With it on they are guest virtual
 * addresses and hyp_guest_va_to_pa() has to walk the guest's own tables
 * before anything is read.
 */
static inline bool hyp_guest_mmu_on(void)
{
	return mrs(sctlr_el1) & 1;
}

#endif /* __HV_H__ */
