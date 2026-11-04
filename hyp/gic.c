// SPDX-License-Identifier: GPL-2.0+
/*
 * GICv3 bring-up and the minimal virtual CPU interface (vGIC).
 *
 * The guest's debug UART (qcom,geni-debug-uart, scripts/VGIC.md) raises a real
 * GIC interrupt that nothing on this board has ever consumed: U-Boot has no
 * GIC driver compiled, its console driver is polled, and the EL2 lower-EL IRQ
 * vector used to be a hang().  This file gives the hypervisor a working
 * interrupt path:
 *
 *   - program the physical distributor for the console UART's SPI, looked up
 *     from the DT (the arm,gic-v3 node for the GICD base, the UART node's
 *     `interrupts` property for the INTID and trigger type - never hardcoded);
 *   - enable EL2's system-register view of the GIC (ICC_SRE_EL2.SRE) and the
 *     *hardware* virtual CPU interface (ICH_HCR_EL2.En + ICH_VMCR_EL2), so the
 *     guest's own ICC_IAR1_EL1/ICC_EOIR1_EL1 are served by the CPU, not by us;
 *   - on a physical IRQ: acknowledge it at EL2, priority-drop it, and mirror it
 *     into a list register with HW=1, which is what lets the guest's EOI
 *     deactivate the physical interrupt for us;
 *   - on an LPI (INTID 8192 and up: the ITS's MSIs, so everything behind PCIe): the
 *     same, as a plain virtual interrupt (HW=0) and with the priority drop alone, since
 *     an LPI has no active state to clear;
 *   - at every guest->EL2 sync entry, retire the list registers the guest has
 *     finished with;
 *   - emulate the one CPU-interface register the hardware cannot virtualize,
 *     ICC_SGI1R_EL1, by injecting a virtual (HW=0) interrupt.
 *
 * Xen (~/git/xen, xen/arch/arm/gic-v3.c:1061-1105, gic-vgic.c:96-259) is the
 * model; this is the single-vCPU subset of it.  What is deliberately missing is
 * written up in scripts/VGIC.md.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/io.h>
#include <asm/ptrace.h>
#include <asm/system.h>
#include <hypervisor.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <hyp/spinlock.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <log.h>
#include <string.h>

#include "hv.h"

/*
 * GICD register offsets (Arm GICv3 register layout, IHI 0069).  These are the
 * architecture's offsets, not board geometry: the *base* comes from the DT.
 * A non-secure EL2 access hits the non-secure bank, which is what the DT
 * `reg` describes.
 */
#define GICD_CTLR		0x0000
#define GICD_TYPER		0x0004
#define GICD_IGROUPR(n)		(0x0080 + 4 * ((n) / 32))
#define GICD_ISENABLER(n)	(0x0100 + 4 * ((n) / 32))
#define GICD_ICENABLER(n)	(0x0180 + 4 * ((n) / 32))
#define GICD_ICACTIVER(n)	(0x0380 + 4 * ((n) / 32))

#define GICD_ISPENDR(n)		(0x0200 + 4 * ((n) / 32))
#define GICD_ICPENDR(n)		(0x0280 + 4 * ((n) / 32))
#define GICD_ISACTIVER(n)	(0x0300 + 4 * ((n) / 32))
#define GICD_IPRIORITYR(n)	(0x0400 + 4 * ((n) / 4))
#define GICD_ICFGR(n)		(0x0C00 + 4 * ((n) / 16))
#define GICD_IROUTER(n)		(0x6000 + 8 * (n))
#define GICD_PIDR2		0xFFE8

#define GICD_CTLR_ARE_NS	BIT(4)		/* Affinity routing, non-secure */
#define GICD_CTLR_EN_G1S	BIT(1)		/* Group 1 secure enables */
#define GICD_CTLR_EN_G1NS	BIT(0)		/* Group 1 non-secure enables */
#define GICD_CTLR_EN_G0NS	BIT(2)		/* Group 0 non-secure enable */
#define GICD_TYPER_ITS		GENMASK(4, 0)	/* SPIs implemented = 32 * (n + 1) */

/*
 * MPIDR_EL1 holds Aff0[7:0], Aff1[15:8], Aff2[23:16], Aff3[39:32] and keeps
 * UP/MT/MainID in [31:24].  The GIC packs that affinity one byte each
 * (Aff3[24] Aff2[16] Aff1[8] Aff0[0]), which is what GICR_TYPER reports, and
 * GICD_IROUTER wants it shifted up 8 more with bit 63 free for IRM
 * (broadcast).  Masking MPIDR with either shape copies bit 31 (UP) into an
 * affinity field; gic_affinity_of() moves the fields one by one.
 */

/*
 * The two affinity shapes, named so the move in gic_affinity_of() is checkable
 * with ./scripts/isa.py instead of by hand: Aff0-Aff2 sit in the same place in
 * both, only Aff3 differs - MPIDR keeps it at [39:32], the GIC pack puts it at
 * [31:24].
 */
#define MPIDR_AFF3	GENMASK(39, 32)
#define GIC_AFF0	GENMASK(7, 0)
#define GIC_AFF1	GENMASK(15, 8)
#define GIC_AFF2	GENMASK(23, 16)
#define GIC_AFF3	GENMASK(31, 24)

/*
 * GICR register offsets.  A redistributor is two 64 KiB pages: RD_base at
 * +0x0000 (CTLR/TYPER/WAKER) and SGI_base at +0x10000, whose INTID control
 * registers use exactly the distributor's layout.  `redistributor-stride` is
 * the distance between redistributors (0x40000 here: four banks, one per
 * security state, so the non-secure pair we want is +0x0000 and +0x10000).
 *
 * With affinity routing the distributor implements INTIDs 32-1023 (SPIs);
 * SGIs and PPIs (0-31) live in every redistributor.  GICR_TYPER[63:32] names
 * the CPU a redistributor belongs to, in GIC affinity form.
 */
#define GICR_SPI_BASE		32
#define GICR_SGI_OFF		0x10000
#define GICR_CTLR		0x0000
#define GICR_TYPER		0x0008
#define GICR_TYPER_AFF		GENMASK(63, 32)
#define GICR_WAKER		0x0014
#define GICR_WAKER_SLEEP	BIT(1)
#define GICR_WAKER_ASLEEP	BIT(2)
#define GICR_IGROUPR(n)		(GICR_SGI_OFF + 0x0080 + 4 * ((n) / 32))
#define GICR_ISENABLER(n)	(GICR_SGI_OFF + 0x0100 + 4 * ((n) / 32))
#define GICR_ICENABLER(n)	(GICR_SGI_OFF + 0x0180 + 4 * ((n) / 32))
#define GICR_ICACTIVER(n)	(GICR_SGI_OFF + 0x0380 + 4 * ((n) / 32))
#define GICR_IPRIORITYR(n)	(GICR_SGI_OFF + 0x0400 + 4 * ((n) / 4))
#define GICR_ICFGR(n)		(GICR_SGI_OFF + 0x0C00 + 4 * ((n) / 16))
#define GICR_IROUTER(n)		(GICR_SGI_OFF + 0x6000 + 8 * (n))

/* We never need more than this; ICH_VTR_EL2.ListRegs says what exists. */
#define HYP_MAX_LRS		8

/* The SGI EL2 uses to make another core exit to EL2 (gic_kick()).  Linux's own IPIs are
 * virtual (every guest write to ICC_SGI1R_EL1 is trapped), so the physical SGIs 0-7 - the
 * ones the firmware enables for the non-secure world - are free; SGI 15, which this
 * used to be, is a secure one the non-secure world cannot enable.  The INTID never
 * reaches the guest. */
#define GIC_DOORBELL_SGI	7

/* Physical interrupts and LPIs that found no list register, per core, waiting for one */
#define GIC_DEFER		16

/* INTIDs from here up are LPIs (GICv3: 8192 and up, no active state, no list register HW form) */
#define GIC_LPI_BASE		8192

struct hyp_gic {
	u64 dist;				/* GICD base, from the DT */
	u64 rdist;				/* our GICR bank, found by affinity */
	u64 rdist_base;			/* start of the GICR region, DT reg[1] */
	u64 rdist_span;			/* size of that region */
	u64 rdist_stride;		/* DT redistributor-stride between GICRs */
	int nlr;				/* list registers in use */
	u32 lr_used[HYP_MAX_CPUS];		/* per core: bitmask of list registers we own */
	u16 vsgi_pending[HYP_MAX_CPUS];	/* per core: SGIs other vCPUs queued for it */
	u32 vspi_pending[HYP_MAX_CPUS];	/* per core: virtual SPIs queued for it */
	u16 defer[HYP_MAX_CPUS][GIC_DEFER];	/* per core: physical INTIDs still waiting for a list register */
	int irq;				/* console UART INTID, -1 = unknown */
	int max_intid;			/* highest INTID the distributor implements */
	bool irq_level;			/* the console line's DT flavour */
	bool ready;
	struct spinlock lock;			/* guards lr_used, vsgi_pending and the list registers */
};

static __hyp_data struct hyp_gic gic = { .irq = -1, .lock = __SPINLOCK_UNLOCKED };

/* Remember a physical interrupt that is waiting for a list register (INTID 0 = empty slot) */
static bool __hyp gic_defer(int intid)
{
	u16 *def = gic.defer[hyp_cpu_num() < 0 ? 0 : hyp_cpu_num()];

	for (int i = 0; i < GIC_DEFER; i++)
		if (def[i] == intid)
			return true;
	for (int i = 0; i < GIC_DEFER; i++)
		if (!def[i]) {
			def[i] = intid;
			return true;
		}

	return false;
}

/* Interrupts EL2 keeps for itself (hyp_gic_own_irq()).  Written at driver
 * bring-up, read on the interrupt path. */
#define GIC_MAX_OWNED	4

struct gic_owned {
	int intid;
	hyp_irq_fn fn;
};

static __hyp_data struct gic_owned owned[GIC_MAX_OWNED] = { { 0 } };

static hyp_irq_fn __hyp gic_owner(int intid)
{
	for (int i = 0; i < GIC_MAX_OWNED; i++)
		if (owned[i].fn && owned[i].intid == intid)
			return owned[i].fn;

	return NULL;
}

/* The calling core's slot; list registers, lr_used and the queued SGIs are all
 * per core.  hyp_cpu_num() is -1 only before the core has a hyp_percpu, when
 * nothing here is called yet - slot 0 keeps that case harmless. */
static int __hyp gic_me(void)
{
	int n = hyp_cpu_num();

	return n < 0 ? 0 : n;
}

/* ICH_LR<n>_EL2 are indexed sysregs: the encoding has to be a literal, so the
 * index is a switch, not a loop over an array. */
#define LR_WRITE_CASE(n)	case n: msr(ich_lr##n##_el2, val); break;
#define LR_READ_CASE(n)	case n: return mrs(ich_lr##n##_el2);

static void __hyp gic_write_lr(int n, u64 val)
{
	switch (n) {
	LR_WRITE_CASE(0)
	LR_WRITE_CASE(1)
	LR_WRITE_CASE(2)
	LR_WRITE_CASE(3)
	LR_WRITE_CASE(4)
	LR_WRITE_CASE(5)
	LR_WRITE_CASE(6)
	LR_WRITE_CASE(7)
	default:
		hprintf("gic: list register %d out of range\n", n);
		return;
	}
	asm volatile("isb");
}

static u64 __hyp gic_read_lr(int n)
{
	switch (n) {
	LR_READ_CASE(0)
	LR_READ_CASE(1)
	LR_READ_CASE(2)
	LR_READ_CASE(3)
	LR_READ_CASE(4)
	LR_READ_CASE(5)
	LR_READ_CASE(6)
	LR_READ_CASE(7)
	default:
		return 0;
	}
}

/* First node anywhere under `parent` whose compatible list contains `compat`. */
static ofnode __hyp gic_find_compat(ofnode parent, const char *compat)
{
	ofnode node, found;
	const char *c;
	int i;

	ofnode_for_each_subnode(node, parent) {
		for (i = 0; !ofnode_read_string_index(node, "compatible", i, &c); i++)
			if (!strcmp(c, compat))
				return node;

		found = gic_find_compat(node, compat);
		if (ofnode_valid(found))
			return found;
	}

	return ofnode_null();
}

/*
 * EL2's own view of the GIC plus the guest's virtual interface.  With
 * ICH_HCR_EL2.En=1 the hardware - not us - serves the guest's
 * ICC_IAR1_EL1/ICC_EOIR1_EL1/ICC_PMR_EL1/ICC_CTLR_EL1 accesses, which is why
 * hyp/hv.c no longer emulates those.
 *
 * VEOIM is left 0 on purpose: a guest that writes ICC_EOIR1_EL1 then both
 * priority-drops and deactivates, so one EOI drains a list register.  Xen sets
 * VEOIM=1 because its guests run with EOImode=1 and issue ICC_DIR_EL1 too;
 * nothing here does.
 */
static void __hyp gic_cpuif_setup(void)
{
	u64 vtr = mrs(ich_vtr_el2);
	u64 vmcr;

	msr(icc_sre_el2, mrs(icc_sre_el2) | ICC_SRE_SRE | ICC_SRE_ENEL1);
	asm volatile("isb");

	gic.nlr = min_t(int, (int)FIELD_GET(ICH_VTR_LISTREGS, vtr) + 1, HYP_MAX_LRS);
	if (gic.nlr < 1) {
		hprintf("gic: ICH_VTR_EL2=%#llx reports no list registers\n", vtr);
		return;
	}

	for (int n = 0; n < gic.nlr; n++)
		gic_write_lr(n, 0);
	/* A core coming back after CPU_OFF starts clean: nothing in flight, no
	 * SGI queued from its previous life. */
	gic.lr_used[gic_me()] = 0;
	gic.vsgi_pending[gic_me()] = 0;
	gic.vspi_pending[gic_me()] = 0;

	/* Enable BOTH virtual groups: Linux never writes ICC_IGRPEN0_EL1, and
	 * group-0 DT interrupts (apps_rsc status IRQs are group 0) would sit
	 * pending in a list register forever otherwise. */
	vmcr = FIELD_PREP(ICH_VMCR_VPMR, 0xff) | ICH_VMCR_VENG1 | ICH_VMCR_VENG0;
	msr(ich_vmcr_el2, vmcr);
	msr(ich_hcr_el2, ICH_HCR_EN);

	/* EL2's physical CPU interface (the ICC_*_EL1 encodings reached from
	 * EL2 are the physical ones; from EL1 they are the virtual ones). */
	msr(icc_bpr1_el1, 0);		/* no binary point: full priority resolution */
	msr(icc_pmr_el1, 0xff);		/* take everything */
	msr(icc_ctlr_el1, ICC_CTLR_EOIMODE);	/* EOImode 1: EOIR only drops priority, ICC_DIR/the guest's EOI deactivates */
	msr(icc_igrpen1_el1, 1);		/* Group 1 = IRQ, no FIQs for anyone */
	asm volatile("isb");

	hprintf("gic: vGIC nlr=%d priobits=%d VTR=%#llx VMCR=%#llx HCR=%#llx SRE=%#llx\n",
		gic.nlr, (int)FIELD_GET(ICH_VTR_PRIBITS, vtr) + 1,
		(unsigned long long)vtr,
		(unsigned long long)mrs(ich_vmcr_el2),
		(unsigned long long)mrs(ich_hcr_el2),
		(unsigned long long)mrs(icc_sre_el2));
}

/*
 * Where an interrupt's control registers live.  With affinity routing the
 * distributor owns the SPI range (INTIDs 32-1023, GICD_TYPER says how far
 * they reach); SGIs and PPIs are per-CPU and live in our redistributor.
 */
struct gic_irq_regs {
	u64 igroupr, isnabler, icenabler, icactiver, ipriorityr, icfgr, irouter;
};

static void __hyp gic_irq_regs(int intid, struct gic_irq_regs *r)
{
	if (intid >= GICR_SPI_BASE) {
		r->igroupr    = gic.dist + GICD_IGROUPR(intid);
		r->isnabler   = gic.dist + GICD_ISENABLER(intid);
		r->icenabler  = gic.dist + GICD_ICENABLER(intid);
		r->icactiver  = gic.dist + GICD_ICACTIVER(intid);
		r->ipriorityr = gic.dist + GICD_IPRIORITYR(intid);
		r->icfgr      = gic.dist + GICD_ICFGR(intid);
		r->irouter    = gic.dist + GICD_IROUTER(intid);
		return;
	}

	r->igroupr    = gic.rdist + GICR_IGROUPR(intid);
	r->isnabler   = gic.rdist + GICR_ISENABLER(intid);
	r->icenabler  = gic.rdist + GICR_ICENABLER(intid);
	r->icactiver  = gic.rdist + GICR_ICACTIVER(intid);
	r->ipriorityr = gic.rdist + GICR_IPRIORITYR(intid);
	r->icfgr      = gic.rdist + GICR_ICFGR(intid);
	r->irouter    = gic.rdist + GICR_IROUTER(intid);
}

/*
 * An MPIDR as the GIC names it: Aff3:Aff2:Aff1:Aff0, one byte each, which is
 * the form GICR_TYPER reports (in [63:32]) and ICC_SGI1R_EL1 targets;
 * GICD_IROUTER wants the same pack shifted up by 8 (Aff0 at [15:8]).
 * FIELD_GET() normalizes every field down to bit 0, so each one has to be put
 * back where the GIC expects it.  Aff3 is taken from MPIDR's [39:32] rather
 * than [31:24] on purpose: [31:24] carries the UP/MT bits (this board's
 * MPIDR_EL1 reads 0x81000000), and copying them into Aff3 would route to a
 * CPU that does not exist.
 */
static u32 __hyp gic_affinity_of(u64 mpidr)
{
	u64 aff = FIELD_PREP(GIC_AFF3, FIELD_GET(MPIDR_AFF3, mpidr)) |
		  FIELD_PREP(GIC_AFF2, FIELD_GET(GIC_AFF2, mpidr)) |
		  FIELD_PREP(GIC_AFF1, FIELD_GET(GIC_AFF1, mpidr)) |
		  FIELD_GET(GIC_AFF0, mpidr);

	return (u32)aff;
}

/* This CPU, in the same form. */
static u32 __hyp gic_cpu_affinity(void)
{
	return gic_affinity_of(mrs(mpidr_el1));
}

/* Arm `intid`: Group 1 (IRQ, never FIQ), our priority, trigger type from the
 * DT, routed to this CPU alone. */
static void __hyp gic_arm_irq(int intid, bool level)
{
	struct gic_irq_regs r;
	u32 v;

	gic_irq_regs(intid, &r);

	writel(BIT(intid % 32), r.icactiver);	/* drop any stale active state */

	v = readl(r.igroupr);
	writel(v | BIT(intid % 32), r.igroupr);

	/* Byte lane per interrupt: read-modify-write, the other three
	 * interrupts in this word are not ours to change. */
	v = readl(r.ipriorityr);
	v &= ~(0xffU << ((intid % 4) * 8));
	v |= HYP_IRQ_PRIO << ((intid % 4) * 8);
	writel(v, r.ipriorityr);

	/* ICFGR: 0b00 level, 0b10 edge, two bits per interrupt. */
	v = readl(r.icfgr);
	v &= ~(3U << ((intid % 16) * 2));
	if (!level)
		v |= 2U << ((intid % 16) * 2);
	writel(v, r.icfgr);

	/* GICD_IROUTER: Aff2:Aff1:Aff0 in [23:0] - the DT's reg cell for this
	 * vCPU, which is also what GICR_TYPER[63:32] reports.  (MPIDR_EL1 is
	 * 0x81000000 on every core here, so it cannot say where "this CPU" is.) */
	writeq(hyp_cpu_mpidr(gic_me()) & MPIDR_AFFINITY, r.irouter);
	writel(BIT(intid % 32), r.isnabler);
	asm volatile("dsb sy; isb");

	hprintf("gic: armed irq %d (%s, prio %#x) at %#llx: en=%#x grp=%#x prio=%#x cfg=%#x route=%#llx\n",
		intid, level ? "level" : "edge", HYP_IRQ_PRIO,
		(unsigned long long)r.isnabler, readl(r.isnabler),
		readl(r.igroupr), readl(r.ipriorityr), readl(r.icfgr),
		(unsigned long long)readq(r.irouter));
}

/*
 * Put `vintid` in the first free list register.  `hw` mirrors it to the
 * physical interrupt pintid: the guest's ICC_EOIR1_EL1 then deactivates the
 * real device, which is the whole reason we do not have to.
 */
static int __hyp gic_inject(int vintid, int pintid, u8 prio, bool hw)
{
	u64 val = LR_PENDING | LR_GRP1 | LR_PRIORITY(prio) | LR_VINTID(vintid);
	u32 *used = &gic.lr_used[gic_me()];
	int n;

	for (n = 0; n < gic.nlr; n++)
		if (!(*used & BIT(n)))
			break;
	if (n == gic.nlr) {
		static __hyp_data u32 dumped;

		if (dumped++ < 3) {
			hprintf("gic: cpu%u no free list register for vINTID %d, lr_used %#x\n",
				(unsigned)gic_me(), vintid, (unsigned)*used);
			for (n = 0; n < gic.nlr; n++)
				hprintf("gic: cpu%u lr%d = %#llx\n", (unsigned)gic_me(), n,
					(unsigned long long)gic_read_lr(n));
		}
		return -ENOSPC;
	}

	if (hw)
		val |= LR_HW | LR_PINTID(pintid);

	gic_write_lr(n, val);
	*used |= BIT(n);

	return n;
}

void __hyp hyp_gic_init(void)
{
	ofnode intc, uart;
	phys_addr_t size, rdist_size;
	u64 rdist, stride;
	u32 typer, ctlr, pidr2, aff;
	u32 cells[3];
	bool level = true;
	int max_intid, i;

	intc = gic_find_compat(ofnode_root(), "arm,gic-v3");
	if (!ofnode_valid(intc)) {
		hprintf("gic: no arm,gic-v3 node in the DT, no interrupts\n");
		return;
	}
	gic.dist = ofnode_get_addr_size_index_notrans(intc, 0, &size);

	/*
	 * TYPER is the "can we even see this bank" probe.  This GIC reads back
	 * 0 at GICD_PIDR2 (its peripheral-ID page is not implemented in the
	 * 64 KiB window the DT describes) but a live TYPER: 0x7f041d here,
	 * ITLinesNumber 0x1d = 29 -> 960 SPIs.  A window that is not reachable
	 * reads all zero, so say so instead of silently arming nothing.
	 */
	pidr2 = readl(gic.dist + GICD_PIDR2);
	typer = readl(gic.dist + GICD_TYPER);
	ctlr = readl(gic.dist + GICD_CTLR);
	hprintf("gic: GICD %#llx (%#llx bytes) PIDR2=%#x TYPER=%#x CTLR=%#x\n",
		(unsigned long long)gic.dist, (unsigned long long)size,
		pidr2, typer, ctlr);
	if (!typer) {
		hprintf("gic: GICD %#llx reads nothing, interrupts disabled\n",
			gic.dist);
		gic.dist = 0;
		return;
	}

	/* The non-secure distributor may have been left disabled by XBL. */
	if (!(ctlr & (GICD_CTLR_EN_G1NS | GICD_CTLR_EN_G1S))) {
		writel(ctlr | GICD_CTLR_ARE_NS | GICD_CTLR_EN_G1S | GICD_CTLR_EN_G1NS,
		       gic.dist + GICD_CTLR);
		asm volatile("dsb sy; isb");
		hprintf("gic: GICD_CTLR %#x -> %#x\n",
			ctlr, readl(gic.dist + GICD_CTLR));
	}

	gic_cpuif_setup();

	/* Cross-run hygiene: a warm CPU reset (our run preamble) does not
	 * reset the GIC distributor. A level interrupt left ACTIVE by the
	 * previous run never re-pends, so the new guest's device completions
	 * would never reach EL2 at all (observed: the RPMH status IRQ was
	 * caught in one run and invisible in the next). Drain whatever the
	 * old world left: acknowledge until spurious, drop the priority and
	 * deactivate each (EOImode 1 splits the two). */
	for (int guard = 0; guard < 1024; guard++) {
		u32 iid = (u32)FIELD_GET(ICC_IAR_INTID, mrs(icc_iar1_el1));

		if (iid >= ICC_IAR_SPURIOUS)
			break;
		msr(icc_eoir1_el1, iid);
		msr(icc_dir_el1, iid);
		asm volatile("isb");
	}
	if (!gic.nlr)
		return;

	/*
	 * Find this CPU's redistributor.  DT reg[1] is the region, DT
	 * `redistributor-stride` the distance between redistributors - a 2-cell
	 * property, so ofnode_read_u32() cannot see it (reading it as a u32
	 * yields 0 and the walk below never advances).  Each redistributor names
	 * its CPU in GICR_TYPER[63:32]; GICR_AFFINITY is not implemented on this
	 * GIC (it reads 0 for every CPU), so TYPER is the only identity there is.
	 * The walk is bounded by the region size rather than by GICR_TYPER.Last,
	 * which this implementation does not clear.
	 */
	rdist = ofnode_get_addr_size_index_notrans(intc, 1, &rdist_size);
	if (ofnode_read_u64(intc, "redistributor-stride", &stride))
		stride = 0x20000;		/* RD_base + SGI_base, the default */

	/* Stashed for hyp_gic_cpuif(): a core brought up later looks its own
	 * redistributor up in this region instead of re-reading the DT. */
	gic.rdist_base = rdist;
	gic.rdist_span = rdist_size;
	gic.rdist_stride = stride;
	aff = gic_cpu_affinity();
	hprintf("gic: GICR %#llx (%#llx bytes) stride %#llx, affinity %#x\n",
		(unsigned long long)rdist, (unsigned long long)rdist_size,
		(unsigned long long)stride, aff);

	for (i = 0; !gic.rdist && (u64)i * stride < rdist_size; i++) {
		u64 base = rdist + (u64)i * stride;
		u64 typer = readq(base + GICR_TYPER);

		hprintf("gic: GICR[%d] %#llx TYPER %#llx aff %#llx waker %#x\n",
			i, (unsigned long long)base, (unsigned long long)typer,
			(unsigned long long)FIELD_GET(GICR_TYPER_AFF, typer),
			readl(base + GICR_WAKER));

		if (FIELD_GET(GICR_TYPER_AFF, typer) == ((u64)aff << 32))
			gic.rdist = base;
	}

	if (!gic.rdist)
		hprintf("gic: no redistributor for affinity %#x\n", aff);

	max_intid = 32 * ((int)FIELD_GET(GICD_TYPER_ITS, typer) + 2) - 1;
	gic.max_intid = max_intid;

	uart = gic_find_compat(ofnode_root(), "qcom,geni-debug-uart");
	if (!ofnode_valid(uart)) {
		hprintf("gic: no console UART node in the DT, nothing armed\n");
		gic.ready = true;
		return;
	}
	if (ofnode_read_u32_array(uart, "interrupts", cells, 3)) {
		hprintf("gic: %s has no interrupts property, nothing armed\n",
			ofnode_get_name(uart));
		gic.ready = true;
		return;
	}
	/* arm,gic-v3 cells: type (0 SPI, 1 PPI), index, flags (bit 0 edge,
	 * else level); SM8650 adds a 4th wake cell we do not read.  The index is
	 * relative to its range - SPI n is INTID 32+n, PPI n is 16+n, which is
	 * how the DT's own maintenance interrupt (PPI 9 = INTID 25) reads too.
	 * The console UART is `GIC_SPI 462`, so INTID 494, not 462. */
	if (cells[0] != 0) {
		hprintf("gic: console UART is not an SPI (%#x), nothing armed\n",
			cells[0]);
		gic.ready = true;
		return;
	}
	gic.irq = GICR_SPI_BASE + (int)cells[1];
	level = !(cells[2] & 1);
	if (gic.irq > max_intid) {
		hprintf("gic: console irq %d beyond the %d implemented INTIDs\n",
			gic.irq, max_intid);
		gic.ready = true;
		return;
	}

	gic.irq_level = level;
	gic_arm_irq(gic.irq, level);
	gic.ready = true;
	/* A core brought up later reads this geometry to find its own
	 * redistributor (hyp_gic_cpuif); it has to be in memory, not in this
	 * core's caches. */
	hyp_clean_range(&gic, sizeof(gic));
}

/**
 * hyp_gic_cpuif() - program one core's CPU interface, by MPIDR
 * @mpidr: MPIDR of the core being brought up, as the DT names it
 *
 * hyp_gic_rdist_of() stashes nothing of its own: hyp_gic_init() described the
 * redistributor region from the DT (reg[1], `redistributor-stride`), and the
 * lookup walks those slots matching GICR_TYPER[63:32] against the GIC affinity
 * form of @mpidr (this GIC implements no GICR_AFFINITY, so TYPER is the only
 * identity there is, and the walk is bounded by the region size for the same
 * reason).  On a match, run the same ICC_* / ICH_* setup CPU0 did for itself:
 * the ICH_ / ICC_ sysregs and the list registers are per-PE, so a new core
 * needs its own virtual interface.  gic.rdist - CPU0's bank - and the
 * distributor-side state are left alone.
 *
 * Return: 0 once this core's interface is programmed, -ENOENT if no
 * redistributor reports that affinity (including before hyp_gic_init() has
 * described the region).
 */
int __hyp hyp_gic_cpuif(u64 mpidr)
{
	u64 base = hyp_gic_rdist_of(mpidr);

	if (!base) {
		hprintf("gic: no redistributor for mpidr %#llx (region %#llx span %#llx stride %#llx)\n",
			mpidr, gic.rdist_base, gic.rdist_span, gic.rdist_stride);
		return -ENOENT;
	}

	gic_cpuif_setup();
	hprintf("gic: cpuif for mpidr %#llx (rdist %#llx)\n", mpidr, base);

	return 0;
}

static bool __hyp gic_vintid_in_lr(int vintid);

/*
 * A physical interrupt was taken at EL2 (HCR_EL2.IMO=1 routes every guest IRQ
 * here, whatever the guest's DAIF says).
 *
 * Acknowledge it, mirror it into a list register as HW=1, then drop the
 * physical running priority.  The physical CPU interface runs EOImode 1
 * (gic_cpuif_setup()), so the drop does not deactivate: the interrupt stays
 * active until the guest's own ICC_EOIR1_EL1 retires the HW-backed list
 * register, which deactivates the physical interrupt, and a level source that
 * is still asserted cannot re-fire in the meantime.
 *
 * The priority drop is not optional.  The old code ran EOImode 0 and never
 * dropped here, so the first interrupt's priority stayed the running priority
 * and nothing at that priority or lower could be signalled to EL2 again (by
 * the architecture; the guest's later idle hang turned out to be the WFI
 * problem handled in hyp/hv.c handle_wfx(), not this).
 */
static void __hyp gic_irq_ack(struct hyp_percpu *pcpu)
{
	int intid = (int)FIELD_GET(ICC_IAR_INTID, mrs(icc_iar1_el1));
	hyp_irq_fn fn;
	int lr;

	asm volatile("dsb sy; isb");

	if (intid >= ICC_IAR_SPURIOUS && intid < GIC_LPI_BASE) {
		/* Never read ICC_IAR0_EL1 here: this EL2 is non-secure, the
		 * group-0 acknowledge is UNDEFINED at NS-EL2, and the trap
		 * to EL3 resets the board (observed twice).  A spurious INTID
		 * is routine now - an interrupt can be taken by an earlier
		 * poll of the WFI emulation - so it is not worth a console line. */
		hdebug("gic: cpu%u spurious INTID %d\n", (unsigned)pcpu->id,
		       intid);
		return;
	}

	/* An LPI - what the ITS delivers, i.e. every MSI behind PCIe.  It has no active state and
	 * the list register's physical INTID field cannot name it, so the guest gets a plain
	 * virtual interrupt (HW=0) and the physical side is finished by the priority drop alone:
	 * there is nothing to deactivate (ICC_DIR_EL1 is for INTIDs below 8192; Linux's own
	 * driver skips it too, irq-gic-v3.c gic_eoimode1_eoi_irq()).  The drop is not optional:
	 * an acknowledged LPI whose priority is never dropped stays this core's running
	 * priority, and nothing at that priority or lower - every interrupt Linux programs - is
	 * signalled to the core again.  The virtual interface has 16 INTID bits (ICH_VTR_EL2.
	 * IDbits) and so has the guest's ITS driver (ITS_MAX_LPI_NRBITS).  An LPI that is still
	 * in a list register waits in the deferred table instead of getting a second register
	 * (the same vINTID in two registers is UNPREDICTABLE). */
	if (intid >= GIC_LPI_BASE) {
		static __hyp_data u32 seen[HYP_MAX_CPUS], lost;

		if (seen[gic_me()]++ < 2)
			hprintf("gic: cpu%u LPI %d forwarded to the guest\n",
				(unsigned)pcpu->id, intid);

		lr = -ENOSPC;
		if (intid <= 0xffff && !gic_vintid_in_lr(intid))
			lr = gic_inject(intid, 0, HYP_IRQ_PRIO, false);
		msr(icc_eoir1_el1, intid);
		if (lr < 0 && (intid > 0xffff || !gic_defer(intid)) && lost++ < 3)
			hprintf("gic: cpu%u LPI %d lost: no list register and no room to defer it\n",
				(unsigned)pcpu->id, intid);
		asm volatile("isb");
		return;
	}

	/* Another core's doorbell (gic_kick()): nothing to inject, the exit this
	 * interrupt caused is what flushes the SGIs queued for this core. */
	if (intid == GIC_DOORBELL_SGI) {
		msr(icc_eoir1_el1, intid);
		msr(icc_dir_el1, intid);
		asm volatile("isb");
		return;
	}

	/* An interrupt EL2 owns is consumed here and never reaches the guest.  The
	 * handler runs under gic.lock with DAIF masked and has to quiesce its
	 * source: the interrupt is deactivated as soon as it returns, so a level
	 * line that is still asserted fires again. */
	fn = gic_owner(intid);
	if (fn) {
		fn(intid);
		msr(icc_eoir1_el1, intid);
		msr(icc_dir_el1, intid);
		asm volatile("isb");
		return;
	}

	/* HYP_IRQ_PRIO is 0 (highest): the virtual interface gates on
	 * VMCR.Group*PMR (observed 0x4c/0xf0), so anything numerically above
	 * the guest's mask would leave the LR pending but never signalled. */
	lr = gic_inject(intid, intid, HYP_IRQ_PRIO, true);
	msr(icc_eoir1_el1, intid);		/* priority drop */
	if (lr < 0) {
		static __hyp_data u32 nolr;

		/* No list register is free.  Deactivating the interrupt here lets a
		 * level source that is still asserted (the guest's timer, the rpmh
		 * completion) fire again at once - a storm at EL2 that starves the
		 * guest of the very time it needs to free a register.  Left active,
		 * it stays quiet and gic_flush_deferred() injects it later. */
		if (!gic_defer(intid)) {
			msr(icc_dir_el1, intid);
			if (nolr++ < 3)
				hprintf("gic: cpu%u irq %d, no free list register and no room to defer it, dropped\n",
					(unsigned)pcpu->id, intid);
		}
	} else {
		hdebug("gic: cpu%u irq %d -> lr%d (vINTID %d, hw)\n",
		       (unsigned)pcpu->id, intid, lr, intid);
	}
	asm volatile("isb");
}

/* Public entry points.  lr_used and the list registers are shared by the IRQ
 * path, the sync-entry sweep and the SGI emulation, so all three take the
 * lock.  DAIF is already masked on EL2 exception entry, so the irqsave form
 * costs nothing here and is what keeps these correct the moment a caller
 * exists at EL1 or on a second core. */
void __hyp hyp_gic_irq(struct hyp_percpu *pcpu)
{
	unsigned long flags;

	spin_lock_irqsave(&gic.lock, flags);
	gic_irq_ack(pcpu);
	spin_unlock_irqrestore(&gic.lock, flags);
}

/*
 * Retire list registers the guest has finished with: State[63:62] == 0b00
 * means it acknowledged and end-of-interrupted them.  Called on every
 * guest->EL2 sync entry, which is how the hypervisor sees the guest's side of
 * the virtual interface without trapping it.
 */
static void __hyp gic_retire_lrs(void)
{
	u32 *used = &gic.lr_used[gic_me()];
	int n;

	if (!*used)
		return;

	for (n = 0; n < gic.nlr; n++) {
		u64 lr;

		if (!(*used & BIT(n)))
			continue;

		lr = gic_read_lr(n);
		if (lr & LR_STATE_MASK)
			continue;

		gic_write_lr(n, 0);
		*used &= ~BIT(n);
		hdebug("gic: lr%d drained (vINTID %d)\n", n, LR_GET_VINTID(lr));
	}
}

/* Is vINTID already pending or active in one of this core's list registers?
 * Two list registers with the same vINTID are UNPREDICTABLE. */
static bool __hyp gic_vintid_in_lr(int vintid)
{
	u32 used = gic.lr_used[gic_me()];

	for (int n = 0; n < gic.nlr; n++)
		if ((used & BIT(n)) && LR_GET_VINTID(gic_read_lr(n)) == vintid &&
		    (gic_read_lr(n) & LR_STATE_MASK))
			return true;

	return false;
}

/*
 * The guest drives the physical distributor directly (there is no GICD emulation),
 * and Linux's GIC driver disables every SPI it does not know when it initialises
 * the distributor - which includes the ones EL2 owns.  This runs on every guest
 * exit and switches an owned SPI back on if it has been turned off; after the
 * guest's one-time distributor init nothing turns it off again.
 */
static void __hyp gic_rearm_owned(void)
{
	for (int i = 0; i < GIC_MAX_OWNED; i++) {
		struct gic_irq_regs r;
		int intid = owned[i].intid;

		if (!owned[i].fn)
			continue;

		gic_irq_regs(intid, &r);
		if (!(readl(r.isnabler) & BIT(intid % 32)))
			writel(BIT(intid % 32), r.isnabler);
	}
}

/*
 * Put the SGIs other vCPUs queued for this core into its list registers.  An
 * SGI whose vINTID is still pending or active here, or that finds no free
 * register, stays queued and goes in at the next exit.
 */
static void __hyp gic_flush_vsgis(void)
{
	u16 *pend = &gic.vsgi_pending[gic_me()];

	for (int irq = 0; irq < 16 && *pend; irq++) {
		if (!(*pend & BIT(irq)) || gic_vintid_in_lr(irq))
			continue;
		if (gic_inject(irq, 0, HYP_IRQ_PRIO, false) < 0)
			break;
		*pend &= ~BIT(irq);
	}
}

/*
 * Virtual SPIs, the interrupts of EL2's virtual devices (HYP_VSPI_BASE...): edge triggered,
 * no physical interrupt behind them.  One still pending or active in a list register is not
 * injected again - the guest's handler is about to look at the device's own status - and
 * one that finds no free register stays queued for the next exit.
 */
static void __hyp gic_flush_vspis(void)
{
	u32 *pend = &gic.vspi_pending[gic_me()];

	for (int i = 0; i < HYP_VSPIS && *pend; i++) {
		int irq = HYP_VSPI_BASE + i;

		if (!(*pend & BIT(i)) || gic_vintid_in_lr(irq))
			continue;
		if (gic_inject(irq, 0, HYP_IRQ_PRIO, false) < 0)
			break;
		*pend &= ~BIT(i);
	}
}

/*
 * Physical interrupts gic_irq_ack() could not give a list register (see gic_defer()):
 * inject them as registers free up.  They are still active in the distributor, so the
 * guest's EOI of the HW-backed register deactivates them as usual.
 */
static void __hyp gic_flush_deferred(void)
{
	u16 *def = gic.defer[gic_me()];

	for (int i = 0; i < GIC_DEFER; i++) {
		if (!def[i] || gic_vintid_in_lr(def[i]))
			continue;
		if (gic_inject(def[i], def[i], HYP_IRQ_PRIO, def[i] < GIC_LPI_BASE) < 0)
			break;
		def[i] = 0;
	}
}

/**
 * hyp_gic_vspi_raise() - raise virtual SPI @intid on the guest core that is calling
 */
void __hyp hyp_gic_vspi_raise(int intid)
{
	unsigned long flags;

	if (intid < HYP_VSPI_BASE || intid >= HYP_VSPI_BASE + HYP_VSPIS)
		return;

	spin_lock_irqsave(&gic.lock, flags);
	gic.vspi_pending[gic_me()] |= BIT(intid - HYP_VSPI_BASE);
	gic_flush_vspis();
	spin_unlock_irqrestore(&gic.lock, flags);
}

void __hyp hyp_gic_sync(void)
{
	unsigned long flags;
	static __hyp_data u32 stuck;

	/* Every caller runs with nothing acknowledged, so the physical running priority has to be
	 * idle (0xff).  Anything else is an interrupt that was acknowledged and never
	 * priority-dropped: nothing at that priority or lower is signalled to this core again. */
	if (stuck < 3 && (u32)mrs(S3_0_C12_C11_3) != 0xff) {	/* ICC_RPR_EL1 */
		stuck++;
		hprintf("gic: cpu%u running priority %#lx with nothing acknowledged (HPPIR1 %lu, PMR %#lx)\n",
			(unsigned)gic_me(), (unsigned long)mrs(S3_0_C12_C11_3),
			(unsigned long)(mrs(icc_hppir1_el1) & ICC_IAR_INTID),
			(unsigned long)mrs(icc_pmr_el1));
	}

	spin_lock_irqsave(&gic.lock, flags);
	gic_retire_lrs();
	gic_flush_vsgis();
	gic_flush_vspis();
	gic_flush_deferred();
	gic_rearm_owned();
	spin_unlock_irqrestore(&gic.lock, flags);
}

/*
 * The physical doorbell that makes a core running its guest exit to EL2 so it
 * notices a queued SGI: a real SGI from EL2.  The guest never gets this INTID
 * - EL2 takes it first (HCR_EL2.IMO) and gic_irq_ack() drops it - and every
 * guest write to ICC_SGI1R_EL1 is trapped, so nothing else sends physical SGIs.
 */
static void __hyp gic_kick(int target)
{
	u64 aff = hyp_cpu_mpidr(target) & MPIDR_AFFINITY;
	u64 val = FIELD_PREP(ICC_SGI1R_AFF2, (aff >> 16) & 0xff) |
		  FIELD_PREP(ICC_SGI1R_AFF1, (aff >> 8) & 0xff) |
		  FIELD_PREP(ICC_SGI1R_IRQ, GIC_DOORBELL_SGI) |
		  FIELD_PREP(ICC_SGI1R_TARGET, BIT(aff & 0xf)) |
		  FIELD_PREP(ICC_SGI1R_RS, (aff & 0xff) >> 4);

	msr(icc_sgi1r_el1, val);
	asm volatile("isb");
}

void __hyp hyp_gic_doorbell(int slot)
{
	gic_kick(slot);
}

/*
 * ICC_SGI1R_EL1 is the one CPU-interface access the hardware cannot virtualize:
 * an EL1 write traps to EL2 (EC=0x18) and the hypervisor must turn it into a
 * virtual interrupt.  TargetList bit n of [15:0] names the vCPU whose affinity
 * is Aff3:Aff2:Aff1:(16 * RS + n); IRM set means every vCPU but the sender.
 * A target that is this core gets the SGI in its own list register now; any
 * other vCPU has it queued (its list registers are its own PE's) and is kicked
 * with a doorbell.
 */
static int __hyp gic_sgi1r(u64 val)
{
	int irq = (int)FIELD_GET(ICC_SGI1R_IRQ, val);
	u64 base = (FIELD_GET(ICC_SGI1R_AFF2, val) << 16) |
		   (FIELD_GET(ICC_SGI1R_AFF1, val) << 8);
	u32 targets = FIELD_GET(ICC_SGI1R_TARGET, val);
	u32 rs = FIELD_GET(ICC_SGI1R_RS, val) * 16;
	bool irm = FIELD_GET(ICC_SGI1R_IRQMODE, val);
	int me = gic_me();
	bool aff3 = !!FIELD_GET(ICC_SGI1R_AFF3, val);
	int hit = 0;

	for (int s = 0; s < hyp_cpu_count(); s++) {
		u64 aff = hyp_cpu_mpidr(s) & MPIDR_AFFINITY;
		bool match;

		if (irm)
			match = s != me;
		else
			match = !aff3 && (aff & ~0xffULL) == base &&
				(aff & 0xff) >= rs && (aff & 0xff) < rs + 16 &&
				(targets & BIT((aff & 0xff) - rs));
		if (!match)
			continue;

		hit++;
		if (s == me) {
			/* Not injected straight away: the same SGI may still be pending or active
			 * in a list register (irq_work raises itself from inside its own handler),
			 * and a second register for one vINTID is UNPREDICTABLE.  Queue it like
			 * another core's SGI; the doorbell makes this core exit soon enough to
			 * take it if it could not go in now. */
			gic.vsgi_pending[s] |= BIT(irq);
			gic_flush_vsgis();
			if (gic.vsgi_pending[s] & BIT(irq))
				gic_kick(s);
		} else {
			gic.vsgi_pending[s] |= BIT(irq);
			gic_kick(s);
		}
	}

	if (!hit)
		hdebug("gic: SGI %d targets %#x base %#llx names no vCPU\n", irq,
		       targets, base);

	return 0;
}

int __hyp hyp_gic_emulate_sgi1r(u64 val)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&gic.lock, flags);
	ret = gic_sgi1r(val);
	spin_unlock_irqrestore(&gic.lock, flags);

	return ret;
}

/**
 * hyp_gic_read_lr() - what one list register currently holds
 * @n: list register number
 *
 * For a caller that has to see the virtual interface's state - the self-test
 * vGIC cases, for instance.  Returns 0 for a number this GIC does not
 * implement.
 */
u64 __hyp hyp_gic_read_lr(int n)
{
	unsigned long flags;
	u64 val;

	spin_lock_irqsave(&gic.lock, flags);
	val = gic_read_lr(n);
	spin_unlock_irqrestore(&gic.lock, flags);

	return val;
}

/**
 * hyp_gic_write_lr() - program one list register
 * @n: list register number
 * @val: value to write it
 *
 * Writing State = Inactive (0b00) hands the register back: the next
 * hyp_gic_sync() sees it drained and drops it from gic.lr_used.
 */
void __hyp hyp_gic_write_lr(int n, u64 val)
{
	unsigned long flags;

	spin_lock_irqsave(&gic.lock, flags);
	gic_write_lr(n, val);
	spin_unlock_irqrestore(&gic.lock, flags);
}

/**
 * hyp_gic_nr_lrs() - how many list registers this GIC implements
 *
 * ICH_LR<n>_EL2 above ICH_VTR_EL2.ListRegs is not implemented, so anything
 * that walks the list registers has to stop here.
 */
int __hyp hyp_gic_nr_lrs(void)
{
	return gic.nlr;
}

/**
 * hyp_gic_console_irq() - the console UART's INTID, -1 if the DT did not say
 */
int __hyp hyp_gic_console_irq(void)
{
	return gic.irq;
}

/**
 * hyp_gic_arm_console_irq() - put the console interrupt back on the list
 *
 * The catch path no longer disarms what it catches, so a caught level
 * interrupt keeps firing until the guest's EOI + status clear quench it;
 * this re-arm is for the case where it was disarmed or never armed.
 */
void __hyp hyp_gic_arm_console_irq(void)
{
	if (gic.irq >= 0)
		gic_arm_irq(gic.irq, gic.irq_level);
}

/**
 * hyp_gic_own_irq() - keep an SPI for EL2
 * @intid: INTID of the SPI (32 and up)
 * @level: level-triggered (as the DT's flags say) rather than edge
 * @fn: handler, called with the INTID from the interrupt path
 *
 * Arms @intid (Group 1, EL2's priority, routed to the calling core) and
 * registers @fn.  From then on the interrupt is acknowledged and deactivated at
 * EL2 and nothing is injected into the guest.  @fn runs under the GIC lock with
 * interrupts masked, so it must not call anything that takes the lock, and it
 * must quiesce the device: the interrupt is deactivated when it returns.
 *
 * Return: 0 on success, -EINVAL for an INTID that is not an implemented SPI,
 * -ENOSPC when the owned table is full.
 */
int __hyp hyp_gic_own_irq(int intid, bool level, hyp_irq_fn fn)
{
	int i, slot = -1;

	if (!gic.ready || !fn || intid < GICR_SPI_BASE || intid > gic.max_intid)
		return -EINVAL;

	for (i = 0; i < GIC_MAX_OWNED; i++) {
		if (owned[i].fn && owned[i].intid == intid) {
			slot = i;
			break;
		}
		if (!owned[i].fn && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;

	owned[slot].intid = intid;
	owned[slot].fn = fn;
	hyp_clean_range(owned, sizeof(owned));
	gic_arm_irq(intid, level);

	return 0;
}

/**
 * hyp_gic_disarm_irq() - stop an SPI from being signalled
 * @intid: INTID of the SPI
 *
 * Safe from an owned-interrupt handler.
 */
void __hyp hyp_gic_disarm_irq(int intid)
{
	struct gic_irq_regs r;

	gic_irq_regs(intid, &r);
	writel(BIT(intid % 32), r.icenabler);
	asm volatile("dsb sy; isb");
}

/**
 * hyp_gic_disown_irq() - give an SPI back: disarm it and drop its handler
 * @intid: INTID of the SPI
 */
void __hyp hyp_gic_disown_irq(int intid)
{
	hyp_gic_disarm_irq(intid);

	for (int i = 0; i < GIC_MAX_OWNED; i++)
		if (owned[i].fn && owned[i].intid == intid)
			owned[i].fn = NULL;

	hyp_clean_range(owned, sizeof(owned));
}

/**
 * hyp_gic_dist_base() - the distributor base hyp_gic_init() took from the DT
 */
u64 __hyp hyp_gic_dist_base(void)
{
	return gic.dist;
}

/**
 * hyp_gic_rdist_region() - the redistributor region as the DT described it
 * @span: filled with the region size (DT reg[1] size)
 * @stride: filled with the DT redistributor-stride
 */
void __hyp hyp_gic_rdist_region(u64 *span, u64 *stride)
{
	*span = gic.rdist_span;
	*stride = gic.rdist_stride;
}

/**
 * hyp_gic_rdist_of() - the redistributor bank a core's affinity owns, by MPIDR
 * @mpidr: MPIDR as the DT names it
 *
 * Pure lookup, no side effects on any core: walk the DT-described region and
 * match GICR_TYPER[63:32] against the GIC affinity form of @mpidr.  Bounded by
 * the region size, because this implementation never clears GICR_TYPER.Last.
 *
 * Return: the bank base, or 0 when no redistributor reports that affinity.
 */
u64 __hyp hyp_gic_rdist_of(u64 mpidr)
{
	unsigned long flags;
	u64 aff = gic_affinity_of(mpidr);
	u64 base = 0;
	int i;

	spin_lock_irqsave(&gic.lock, flags);

	for (i = 0; gic.rdist_stride && (u64)i * gic.rdist_stride < gic.rdist_span; i++) {
		u64 slot = gic.rdist_base + (u64)i * gic.rdist_stride;
		u64 typer = readq(slot + GICR_TYPER);

		if (FIELD_GET(GICR_TYPER_AFF, typer) == aff) {
			base = slot;
			break;
		}
	}

	spin_unlock_irqrestore(&gic.lock, flags);

	return base;
}

/**
 * hyp_gic_nr_intids() - the highest INTID the distributor implements
 *
 * GICD_TYPER.ITLinesNumber counts 32-INTID blocks; SGIs and PPIs (0-31) live in
 * every redistributor instead.
 */
int __hyp hyp_gic_nr_intids(void)
{
	return gic.max_intid;
}

/**
 * hyp_gic_rdist_self() - the redistributor bank hyp_gic_init() found for this core
 *
 * Found by matching GICR_TYPER against MPIDR_EL1 during bring-up; a caller can
 * compare it against hyp_gic_rdist_of() on the affinity the DT gives this core.
 */
u64 __hyp hyp_gic_rdist_self(void)
{
	return gic.rdist;
}

/**
 * hyp_gic_irq_pending() - is a physical interrupt waiting at this core's CPU interface?
 *
 * ICC_HPPIR1_EL1 names it.  1020-1023 are the special INTIDs ("nothing", "not ours"), but an
 * LPI (8192 and up, every MSI) is a real interrupt whose number is above them: a test of
 * "below 1020" alone never sees the ITS's interrupts, and an idle core polling for a wake-up
 * would sleep through them until the next timer.
 */
bool __hyp hyp_gic_irq_pending(void)
{
	u32 id = (u32)(mrs(icc_hppir1_el1) & ICC_IAR_INTID);

	return id < ICC_IAR_SPURIOUS || id >= GIC_LPI_BASE;
}

/**
 * hyp_gic_wake_pending() - would a WFI in the guest end now?
 *
 * A pending virtual interrupt is a WFI wakeup event whatever the guest's
 * PSTATE.I says, so the WFI emulation (hyp/hv.c hyp_guest_wait()) returns at
 * once when one sits in a list register (State = Pending or Pending+Active) -
 * or when another vCPU has queued an SGI for this core, which hyp_gic_sync()
 * then puts in a list register.
 */
bool __hyp hyp_gic_wake_pending(void)
{
	if (gic.vsgi_pending[gic_me()] || gic.vspi_pending[gic_me()])
		return true;

	for (int i = 0; i < GIC_DEFER; i++)
		if (gic.defer[gic_me()][i])
			return true;

	for (int n = 0; n < gic.nlr; n++)
		if (gic_read_lr(n) & LR_PENDING)
			return true;

	return false;
}
