// SPDX-License-Identifier: GPL-2.0+
/*
 * EL2 stage-1 identity map: run the hypervisor with its MMU on.
 *
 * hv_start_vm() calls hyp_s1_enable() just before eret'ing to the guest.
 * The map is identity (VA == PA), so every EL2 pointer keeps working
 * unchanged: hyp code/data (the .hyp + .data.hyp sections at their link
 * addresses, never relocated), the page-table pools and EL2 stacks in the
 * hyp_mem reservation, relocated U-Boot code the hypervisor calls into
 * (printf etc.), DRAM, and the below-DRAM MMIO it touches (debug UART,
 * RPMH, ...).
 *
 * hyp_s1_enable() builds the tables once, on CPU0; hyp_s1_apply() re-publishes
 * them on a secondary core, which never allocates or rebuilds anything
 * (scripts/SMP.md).
 *
 * Geometry mirrors the stage-2 map in hyp/mmio.c: 4 KiB granule,
 * T0SZ=28 (64 GiB VA, initial lookup level 1), 2 MiB blocks.  Regions:
 *   [0, ram_base)                 Device-nGRE, XN   (MMIO, pools, stacks)
 *   DRAM banks                    Normal WB, RWX    (guest + relocated U-Boot)
 *   [CONFIG_TEXT_BASE, +1 MiB)    Normal WB, RWX    (our own code/data;
 *                                     defensive: mapped even if a bank gap
 *                                     or the reservation would exclude it)
 * Anything else is unmapped on purpose: an EL2 access outside the known
 * layout faults visibly instead of reading garbage.
 *
 * Stage-2 still applies to EL2 while HCR_EL2.VM=1 (we are not VHE), so
 * the spy's stage-2-invalid regions are handled by hyp_s2_emulate()
 * dropping VM around its own accesses - see hyp/mmio.c.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <errno.h>
#include <hang.h>
#include <hyp/mutex.h>
#include <hypervisor.h>
#include <stdbool.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <log.h>

#include "hv.h"

DECLARE_GLOBAL_DATA_PTR;

#define S1_BLOCK_SHIFT	21
#define S1_BLOCK_SIZE	(1ULL << S1_BLOCK_SHIFT)
/* arch/arm/cpu/armv8/u-boot.lds: bounds of the resident .hyp output section */
extern char __hyp_start[];
extern char __hyp_stop[];
#define S1_L1_SHIFT	30
#define S1_L1_ENTRIES	512

/* TCR_EL2 fields (DDI0487M C5.2.13): T0SZ=28 -> 64 GiB VA, Normal WB
 * inner and outer, Inner Shareable, 4 KiB granule, PS=0b001 (40-bit PA,
 * covers the top bank at 0xb00000000); RES1 bits 23 and 31 per
 * asm/armv8/mmu.h TCR_EL2_RSVD. */
#define S1_T0SZ_64G		FIELD_PREP(GENMASK(5, 0), 28)
#define S1_IRGN0_WB		FIELD_PREP(GENMASK(9, 8), 1)
#define S1_ORGN0_WB		FIELD_PREP(GENMASK(11, 10), 1)
#define S1_SH0_IS		FIELD_PREP(GENMASK(13, 12), 3)
#define S1_TG0_4K		FIELD_PREP(GENMASK(15, 14), 0)
#define S1_PS_40B		FIELD_PREP(GENMASK(18, 16), 1)
#define S1_TCR			(S1_T0SZ_64G | S1_IRGN0_WB | S1_ORGN0_WB | \
				 S1_SH0_IS | S1_TG0_4K | S1_PS_40B | TCR_EL2_RSVD)

/* Stage-1 block descriptor (DDI0487M Table D8-52, 4 KiB granule):
 * [1:0]=0b01 block, AttrIndx[4:2] (MAIR), AP[2:1]=bits [7:6],
 * SH[9:8]=0b11 IS, AF[10]=1, NS[5]=0.  The EL2 regime is
 * single-privilege-level: XN is bit 54 and AP[2] (bit 7) is RES1.
 * Table descriptor: [1:0]=0b11, OA[47:12]. */
#define S1_BLOCK	(1ULL << 0)
#define S1_TABLE	(3ULL << 0)
#define S1_ATTRIDX(a)	(((u64)(a) & 7ULL) << 2)
/* The EL2 regime supports a single Exception level, so the AP semantics
 * are DDI0487M Table D8-62 + RMGGZM, not the two-level D8-63 table:
 * AP[2] (bit 7) 0 -> PrivRead+PrivWrite, 1 -> PrivRead-only; AP[1]
 * (bit 6) is RES1.  RW at EL2 = 0b01 (see scripts/HYP_MMU.md). */
#define S1_RW		(1ULL << 6)
#define S1_SH_IS	(3ULL << 8)
#define S1_AF		(1ULL << 10)
#define S1_XN		(1ULL << 54)
#define S1_OA_MASK	0x0000fffffffff000ULL

/*
 * Table pool carved out of the DTB hyp_mem reservation by hyp_mem_alloc()
 * (hyp/mem.c); the SM8650 map needs ~15 tables, 32 is headroom.
 */
#define S1_POOL_SIZE	(32 * 4096)

struct s1_state {
	u64 *l1;
	u64 *l2_next;
	u64 *l2_end;
	u64 sctlr;		/* SCTLR_EL2 published by hyp_s1_enable() */
};

static __hyp_data struct s1_state s1 = { 0 };

static u64 *s1_alloc_table(void)
{
	u64 *t = s1.l2_next;
	int i;

	if (t + S1_L1_ENTRIES > s1.l2_end) {
		hprintf("stage-1: table pool exhausted\n");
		hang();
	}
	s1.l2_next = t + S1_L1_ENTRIES;
	for (i = 0; i < S1_L1_ENTRIES; i++)
		t[i] = 0;
	return t;
}

static u64 *s1_l2_for(u64 pa)
{
	u64 l1i = pa >> S1_L1_SHIFT;
	u64 desc = s1.l1[l1i];

	if (!(desc & 2)) {
		u64 *t = s1_alloc_table();

		s1.l1[l1i] = (u64)(unsigned long)t | S1_TABLE;
		desc = s1.l1[l1i];
	}
	return (u64 *)(unsigned long)(desc & S1_OA_MASK);
}

static void s1_map_range(u64 start, u64 end, u64 attr)
{
	u64 pa;

	for (pa = start & ~(S1_BLOCK_SIZE - 1); pa < end; pa += S1_BLOCK_SIZE) {
		u64 *l2 = s1_l2_for(pa);

		l2[(pa >> S1_BLOCK_SHIFT) & (S1_L1_ENTRIES - 1)] =
			pa | attr | S1_BLOCK;
	}
}

/*
 * Publish the tables on the calling core.  Enable M first, then flush: with
 * M=0 these ops do not touch what the now-live stage-1 fetches will use, and
 * stale EL2 I-cache lines / TLB entries left by the previous EL2 owner over
 * these VA ranges fault the very first translated fetch (scripts/HYP_MMU.md).
 * ic iallu / tlbi alle2 are per-PE, so every core that runs this map runs
 * them itself.  The value written to SCTLR_EL2 is recorded in s1.sctlr so a
 * later core applies the identical regime (hyp_s1_apply).
 */
static __hyp void s1_publish(u64 sctlr)
{
	s1.sctlr = sctlr;
	/* A core that applies these tables later reads s1.l1/s1.sctlr with
	 * caches that may hold a stale copy of this line; publish it.
	 */
	hyp_clean_range(&s1, sizeof(s1));
	msr(mair_el2, MEMORY_ATTRIBUTES);
	msr(ttbr0_el2, (u64)(unsigned long)s1.l1);
	msr(tcr_el2, S1_TCR);
	/* The new geometry must be visible before M takes effect: without a
	 * context synchronisation event between these writes and the
	 * SCTLR_EL2 write, the first translated fetch can still walk with the
	 * old TCR_EL2/TTBR0_EL2.
	 */
	asm volatile("isb");
	msr(sctlr_el2, sctlr);
	asm volatile("isb");
	/* dsb nsh is enough here: this core only needs its own invalidation
	 * to complete before its first translated fetch.
	 */
	asm volatile("ic iallu\n\ttlbi alle2\n\tdsb nsh\n\tisb");
}

/*
 * Build the tables, publish TTBR0_EL2/TCR_EL2/MAIR_EL2 and flip
 * SCTLR_EL2.M.  Caches stay as XBL left them (we only add M): with M=1
 * and C=0 data accesses are translated but not cached, which is exactly
 * the semantics the hypervisor's raw MMIO accessors and dc-by-PA cache
 * maintenance already assume under an identity map.
 */
void __hyp hyp_s1_enable(void)
{
	u64 sctlr = mrs(sctlr_el2);
	u64 pa;
	int i, banks = 0;

	if (sctlr & CR_M) {
		hprintf("stage-1: SCTLR_EL2.M already set (%#llx), leaving as-is\n",
			sctlr);
		return;
	}

	/* The reservation was Gunyah's; dirty stale lines would be written
	 * back over our table writes (same trick as hyp_s2_init). */
	s1.l1 = hyp_mem_alloc(S1_POOL_SIZE);
	for (pa = (u64)(unsigned long)s1.l1;
	     pa < (u64)(unsigned long)s1.l1 + S1_POOL_SIZE; pa += 64)
		asm volatile("dc ivac, %0" :: "r"(pa));
	asm volatile("dsb nsh");

	s1.l2_next = s1.l1 + S1_L1_ENTRIES;
	s1.l2_end = s1.l1 + S1_POOL_SIZE / 8;
	for (i = 0; i < S1_L1_ENTRIES; i++)
		s1.l1[i] = 0;

	s1_map_range(0, gd->ram_base,
		     S1_ATTRIDX(MT_DEVICE_NGNRE) | S1_RW | S1_AF | S1_SH_IS | S1_XN);

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!gd->bd->bi_dram[i].size)
			continue;
		banks++;
		s1_map_range(gd->bd->bi_dram[i].start, gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size,
			     S1_ATTRIDX(MT_NORMAL) | S1_RW | S1_AF | S1_SH_IS);
	}

	/* Our own resident copy: the whole .hyp output section (vectors,
	 * .text.hyp, .rodata.hyp, .data.hyp) between the linker script
	 * symbols - Normal + executable regardless of the bank layout. */
	s1_map_range((u64)(unsigned long)&__hyp_start,
		     (u64)(unsigned long)&__hyp_stop,
		     S1_ATTRIDX(MT_NORMAL) | S1_RW | S1_AF | S1_SH_IS);

	for (pa = (u64)(unsigned long)s1.l1; pa < (u64)(unsigned long)s1.l2_next; pa += 64)
		asm volatile("dc civac, %0" :: "r"(pa));
	asm volatile("dsb nsh");

	s1_publish(sctlr | CR_M);

	hprintf("stage-1 MMU on: %d banks, %d L2 tables in pool %#lx\n",
		banks, (int)(s1.l2_next - s1.l1) / S1_L1_ENTRIES - 1,
		(unsigned long)s1.l1);
	hprintf("EL2 MMU: sctlr_el2=%#lx (M=%d) ttbr0_el2=%#lx tcr_el2=%#lx\n",
		(unsigned long)mrs(sctlr_el2), (int)(mrs(sctlr_el2) & 1),
		(unsigned long)mrs(ttbr0_el2), (unsigned long)mrs(tcr_el2));
}

/**
 * hyp_s1_apply() - re-publish the stage-1 tables CPU0 built on this core
 *
 * A secondary core must not allocate or rebuild: hyp_s1_enable() carved the
 * pool and filled the identity map once.  This writes the same
 * MAIR_EL2/TTBR0_EL2/TCR_EL2 and the same SCTLR_EL2 value hyp_s1_enable()
 * published (s1.sctlr) into the calling core's own sysregs, then runs the
 * per-PE I-cache/TLB flush a cold core needs before its first translated
 * fetch (scripts/HYP_MMU.md).  The tables themselves are shared, so a core
 * applying them neither re-walks nor rebuilds them (scripts/SMP.md).
 *
 * Return: 0 once the map is live on this core, -ENOENT if hyp_s1_enable()
 * has not built the tables.
 */
int __hyp hyp_s1_apply(void)
{
	if (!s1.l1) {
		hprintf("s1: no stage-1 tables to apply on this cpu\n");
		return -ENOENT;
	}

	s1_publish(s1.sctlr);

	hprintf("s1: applied existing tables on this cpu: ttbr=%#lx sctlr=%#lx\n",
		(unsigned long)mrs(ttbr0_el2), (unsigned long)mrs(sctlr_el2));
	return 0;
}

/*
 * Lazy mapping.
 *
 * hyp_s1_enable() maps the layout it knows about and leaves the rest unmapped
 * on purpose, so an EL2 access outside that layout faults visibly.  That is
 * the right policy until the guest starts using its MMU: from then on the trap
 * path has to read addresses the layout does not describe - the guest's
 * page-table pages, the guest's own instructions - and "fault visibly" there
 * means the *hypervisor* takes a data abort on the way to reporting the
 * guest's (run 3: ESR=0x96000004, DFSC=0x04, a level-0 translation fault on a
 * guest kernel VA that had been read at face value).
 *
 * hyp_s1_covers() asks whether an address is reachable; hyp_s1_map_pa() makes
 * it reachable.  Both are table-only operations: neither touches @pa itself,
 * which is what makes it safe to ask about an address that is not mapped.
 */

/*
 * The pool and the L1 table are shared with every core, and a lazy map is the
 * only place in this file that allocates after bring-up.  Lock held: unlike a
 * watched MMIO access, a table fill completes without any cooperation from the
 * core that took the fault.
 */
static DEFINE_MUTEX(s1_lock);

/**
 * hyp_s1_covers() - is @pa inside EL2's stage-1 map?
 * @pa: physical address to look up
 *
 * Return: true when a valid block descriptor covers @pa.
 */
bool __hyp hyp_s1_covers(u64 pa)
{
	u64 entry = s1.l1 ? s1.l1[pa >> S1_L1_SHIFT] : 0;
	u64 *l2;

	if (!(entry & 2))
		return false;

	l2 = (u64 *)(unsigned long)(entry & S1_OA_MASK);

	return l2[(pa >> S1_BLOCK_SHIFT) & (S1_L1_ENTRIES - 1)] & 1;
}

/*
 * A lazily mapped block is execute-never, and Device unless it sits inside a
 * DRAM bank: the trap path only ever *reads* through these mappings (one guest
 * instruction, one page-table entry), and keeping MMIO Device means nothing
 * here is ever cached or speculated.
 */
static u64 s1_lazy_attr(u64 pa)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		if (pa >= gd->bd->bi_dram[i].start &&
		    pa < gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size)
			return S1_ATTRIDX(MT_NORMAL) | S1_RW | S1_AF | S1_SH_IS | S1_XN;

	return S1_ATTRIDX(MT_DEVICE_NGNRE) | S1_RW | S1_AF | S1_SH_IS | S1_XN;
}

/**
 * hyp_s1_map_pa() - map the 2 MiB block holding @pa into EL2's stage-1 map
 * @pa: physical address the caller needs to reach
 *
 * Only ever fills a hole: an existing block keeps the attributes
 * hyp_s1_enable() gave it, because a DRAM bank has to stay executable for the
 * guest no matter what the hypervisor wants to read there.  The new entry is
 * cleaned into the walker's view and this core's EL2 TLB is flushed, so the
 * caller's next access translates.
 *
 * tlbi alle2 is per-PE, and that is enough: an EL2 stage-1 entry is only ever
 * used by the core that cached it, and every core fills its own holes.
 *
 * Return: 0 when @pa is reachable at EL2, -ENOENT if there are no tables.
 */
int __hyp hyp_s1_map_pa(u64 pa)
{
	if (!s1.l1)
		return -ENOENT;

	if (!hyp_s1_covers(pa)) {
		u64 *l2, *slot;

		mutex_lock(&s1_lock);
		if (!hyp_s1_covers(pa)) {
			l2 = s1_l2_for(pa);
			slot = &l2[(pa >> S1_BLOCK_SHIFT) & (S1_L1_ENTRIES - 1)];
			*slot = (pa & ~(S1_BLOCK_SIZE - 1)) | s1_lazy_attr(pa) | S1_BLOCK;
			asm volatile("dc civac, %0\n\tdsb ish" :: "r"(slot));
		}
		mutex_unlock(&s1_lock);

		asm volatile("tlbi alle2\n\tdsb nsh\n\tisb");
	}

	return 0;
}
