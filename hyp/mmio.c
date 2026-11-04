// SPDX-License-Identifier: GPL-2.0+
/*
 * Stage-2 identity map, data-abort emulation, and the MMIO spy.
 *
 * The stage-2 tables we inherit from Gunyah have holes: the apps SMMU at
 * 0x15000000 is unmapped there, and a guest LDR into it takes a stage-2
 * translation fault (run-1, scripts/EXCEPTION_DEBUGGING.md).  hv_start_vm()
 * replaces them with our own identity map, built here at EL2 while the MMU
 * is still off: 4 KiB granule, T0SZ=28 (64 GiB IPA, lookup starts at
 * level 1), 2 MiB block entries.  The DRAM banks (gd->bd->bi_dram[], see
 * doc/develop/memory.rst) are Normal WB and executable; everything below
 * DRAM (MMIO, XBL/TZ IRAM/IMEM) is Device-nGRE and XN.
 *
 * hyp_s2_init() and hyp_s2_install() do this once, on CPU0; hyp_s2_apply()
 * points a secondary core's VTTBR_EL2/VTCR_EL2 at the same tables
 * (scripts/SMP.md).
 *
 * The same tables are the MMIO spy API: hyp_mmio_spy() clears the valid
 * bit over a range, every guest access to it then faults into EL2, and
 * hyp_s2_emulate() replays the access from the ESR syndrome (ISV=1) while
 * logging it.  The map is identity, so FAR_EL2 is the physical address.
 *
 * Encodings verified against DDI0487M:
 * - VTTBR_EL2: BADDR, VMID[63:48]; VTCR_EL2: T0SZ[5:0], IRGN0[9:8],
 *   ORGN0[11:10], SH0[13:12], TG0[15:14], PS[18:16], RES1[31].
 * - Block/Page descriptor (Table D8-53, 4 KiB granule, FWB=0): [1:0]=0b01,
 *   MemAttr[3:0]->[5:2] (Table D8-96), S2AP[1:0]->[7:6], SH[1:0]->[9:8],
 *   AF->[10], XN->[54].  Table descriptor (D8-51): [1:0]=0b11, OA[47:12].
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/armv8/mmu.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/ptrace.h>
#include <asm/sections.h>
#include <errno.h>
#include <hypervisor.h>
#include <hyp/mutex.h>
#include <linux/bitfield.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <log.h>
#include <stdbool.h>

/* Uncomment to enable the per-access mmio# trace (hdebug()): */
/* #define LOG_DEBUG */	/* off: each trapped access prints ~100ms of
			 * 115200-baud UART and the rpmh path is hot */
#include "hv.h"

DECLARE_GLOBAL_DATA_PTR;

/* Geometry: 4 KiB granule, T0SZ=28 -> 64 GiB IPA. The stage-2 initial
 * lookup level is programmed by VTCR_EL2.SL0[7:6], NOT derived from
 * T0SZ: SL0 must be consistent with T0SZ or the walker takes a stage-2
 * level-0 translation fault (DDI0487M D24.2.88, scripts/HYP_MMU.md).
 * T0SZ=28 is consistent with SL0=0b01 (start at level 1); level-1
 * entries cover 1 GiB, level-2 entries are 2 MiB blocks (universally
 * supported; we avoid 1 GiB blocks). VTTBR_EL2.BADDR is the level-1
 * table base. */
#define S2_BLOCK_SHIFT	21
#define S2_BLOCK_SIZE	(1ULL << S2_BLOCK_SHIFT)
#define S2_L1_SHIFT	30
#define S2_L1_ENTRIES	512

/* VTTBR_EL2: VMID 1 in [63:48] - deliberately not Gunyah's VMID, so no
 * stale TLB entry tagged with the old VMID can match our tables. */
#define S2_VMID		(1ULL << 48)
/* VTCR_EL2 fields (DDI0487M C5.2.152): T0SZ=28 -> 64 GiB IPA, SL0=0b01
 * (initial lookup level 1), Normal WB inner and outer, Inner Shareable,
 * 4 KiB granule, PS=0b001 (40-bit PA, covers the top bank at
 * 0xb00000000), RES1 bit 31. */
#define S2_T0SZ_64G		FIELD_PREP(GENMASK(5, 0), 28)
#define S2_SL0_L1		FIELD_PREP(GENMASK(7, 6), 1)
#define S2_IRGN0_WB		FIELD_PREP(GENMASK(9, 8), 1)
#define S2_ORGN0_WB		FIELD_PREP(GENMASK(11, 10), 1)
#define S2_SH0_IS		FIELD_PREP(GENMASK(13, 12), 3)
#define S2_TG0_4K		FIELD_PREP(GENMASK(15, 14), 0)
#define S2_PS_40B		FIELD_PREP(GENMASK(18, 16), 1)
#define S2_VTCR_RES1		BIT(31)
#define S2_VTCR			(S2_T0SZ_64G | S2_SL0_L1 | S2_IRGN0_WB | \
				 S2_ORGN0_WB | S2_SH0_IS | S2_TG0_4K | \
				 S2_PS_40B | S2_VTCR_RES1)

/*
 * Tables are carved out of the DTB's hyp_mem reservation by
 * hyp_mem_alloc() (hyp/mem.c, scripts/MEMORY.md).  One L2 table per GiB
 * mapped: the SM8650 map needs ~15, 64 is generous headroom.
 */
#define S2_POOL_SIZE	(64 * 4096)

struct s2_state {
	u64 *l1;		/* level-1 table (512 entries) */
	u64 *l2_next;		/* bump allocator for L2 tables */
	u64 *l2_end;
	u32 emulated;		/* accesses replayed by hyp_s2_emulate() */
};

static __hyp_data struct s2_state s2 = { 0 };

/*
 * Guards every stage-2 table write made after hyp_s2_init(): the lazy block
 * fills in s2_map_pa()/s2_readable(), the table pool bump pointer and the
 * spy toggle.  Any core can take a stage-2 fault, so two of them can race to
 * map the same block or to link a new L2 table.  Taken before s1_lock when
 * both are needed (hyp_s1_map_pa() never calls back here).
 */
static DEFINE_MUTEX(s2_lock);

static u64 *s2_alloc_table(void)
{
	u64 *t = s2.l2_next;
	int i;

	if (t + S2_L1_ENTRIES > s2.l2_end) {
		hprintf("stage-2: table pool exhausted\n");
		hang();
	}
	s2.l2_next = t + S2_L1_ENTRIES;
	for (i = 0; i < S2_L1_ENTRIES; i++)
		t[i] = 0;
	/* publish the zeroed table before any entry that points at it */
	asm volatile("dmb ishst" ::: "memory");
	return t;
}

/* Return the L2 table covering pa, installing one in L1 if needed. */
static u64 *s2_l2_for(u64 pa)
{
	u64 l1i = pa >> S2_L1_SHIFT;
	u64 desc = s2.l1[l1i];

	if (!(desc & 2)) {
		u64 *t = s2_alloc_table();

		/* identity map: the table's IPA is its PA */
		s2.l1[l1i] = (u64)(unsigned long)t | S2_TABLE;
		desc = s2.l1[l1i];
	}
	return (u64 *)(unsigned long)(desc & S2_OA_MASK);
}

static void s2_map_range(u64 start, u64 end, u64 attr)
{
	u64 pa;

	for (pa = start & ~(S2_BLOCK_SIZE - 1); pa < end; pa += S2_BLOCK_SIZE) {
		u64 *l2 = s2_l2_for(pa);

		l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)] =
			pa | attr | S2_VALID;
	}
}

/*
 * Build the identity map: DRAM banks Normal WB + executable (the guest
 * runs, allocates, and page-tables there), everything below DRAM
 * Device-nGRE + XN (MMIO, XBL/TZ IRAM/IMEM).  gd->bd->bi_dram[] is filled by
 * dram_init_banksize(), a board_f initcall, so it is valid here.
 */
void __hyp hyp_s2_init(void)
{
	int i, banks = 0;
	u64 ram_top = 0;
	u64 pa;

	/* Gunyah ran at EL2 with caches on over this very pool (it is inside
	 * its hyp_mem reservation); dirty lines would be written back over our
	 * MMU-off table writes. Invalidate the whole pool first. */
	s2.l1 = hyp_mem_alloc(S2_POOL_SIZE);
	for (pa = (u64)(unsigned long)s2.l1;
	     pa < (u64)(unsigned long)s2.l1 + S2_POOL_SIZE; pa += 64)
		asm volatile("dc ivac, %0" :: "r"(pa));
	asm volatile("dsb nsh");

	s2.l2_next = s2.l1 + S2_L1_ENTRIES;
	s2.l2_end = s2.l1 + S2_POOL_SIZE / 8;
	for (i = 0; i < S2_L1_ENTRIES; i++)
		s2.l1[i] = 0;

	s2_map_range(0, gd->ram_base, S2_DEV | S2_RW | S2_AF | S2_SH_INNER | S2_XN);

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		if (!gd->bd->bi_dram[i].size)
			continue;
		banks++;
		if (gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size > ram_top)
			ram_top = gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size;
		hprintf("stage-2 bank %d: %#llx + %#llx\n", i,
			(unsigned long long)gd->bd->bi_dram[i].start,
			(unsigned long long)gd->bd->bi_dram[i].size);
		s2_map_range(gd->bd->bi_dram[i].start, gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size,
			     S2_WB | S2_RW | S2_AF | S2_SH_INNER);
	}

	hprintf("stage-2: %d banks (top %#llx), %d L2 tables in pool %#llx\n",
		banks, ram_top,
		(int)(s2.l2_next - s2.l1) / S2_L1_ENTRIES - 1,
		(u64)(unsigned long)s2.l1);
}

void dump_pagetable(u64 ttbr, u64 tcr);

/* Publish the tables: clean them into the walker's view, set VTTBR/VTCR,
 * flush every stage-2 TLB entry (old VMID included). */
void __hyp hyp_s2_install(void)
{
	u64 pa;

	for (pa = (u64)(unsigned long)s2.l1; pa < (u64)(unsigned long)s2.l2_next; pa += 64)
		asm volatile("dc civac, %0" :: "r"(pa));
	asm volatile("dsb nsh");
	/* The s2 struct travels with its tables: hyp_s2_apply() reads s2.l1 on
	 * a core whose caches may still hold the pre-install copy.
	 */
	hyp_clean_range(&s2, sizeof(s2));

	msr(vttbr_el2, S2_VMID | (u64)(unsigned long)s2.l1);
	msr(vtcr_el2, S2_VTCR);
	asm volatile("tlbi alle2\n\tdsb nsh\n\tisb");

	hprintf("stage-2 installed: vttbr=%#lx vtcr=%#lx\n",
		mrs(vttbr_el2), mrs(vtcr_el2));
	/* VTTBR carries the VMID in [63:48]; the walker wants BADDR only */
	dump_pagetable(mrs(vttbr_el2) & S2_OA_MASK, mrs(vtcr_el2));
}

/**
 * hyp_s2_apply() - point this core's stage-2 walk at the existing tables
 *
 * hyp_s2_init() built the identity map and hyp_s2_install() cleaned it into
 * the walker's view and published it, all on CPU0.  The tables are shared, so
 * a secondary core only programs its own VTTBR_EL2 (same VMID, same level-1
 * table) and VTCR_EL2: nothing is rebuilt, re-cleaned or dumped here.  CPU0's
 * install already flushed every stage-2 TLB entry for the shareability
 * domain, so this core needs no tlbi of its own.
 *
 * Return: 0 once stage-2 is live on this core, -ENOENT if the tables do not
 * exist yet.
 */
int __hyp hyp_s2_apply(void)
{
	if (!s2.l1) {
		hprintf("stage-2: no tables to apply on this cpu\n");
		return -ENOENT;
	}

	msr(vttbr_el2, S2_VMID | (u64)(unsigned long)s2.l1);
	msr(vtcr_el2, S2_VTCR);
	asm volatile("isb");

	hprintf("stage-2: applied on this cpu: vttbr=%#lx vtcr=%#lx\n",
		(unsigned long)mrs(vttbr_el2), (unsigned long)mrs(vtcr_el2));
	return 0;
}

static void s2_set_valid(u64 start, u64 end, bool valid)
{
	u64 pa;

	mutex_lock(&s2_lock);
	for (pa = start & ~(S2_BLOCK_SIZE - 1); pa < end; pa += S2_BLOCK_SIZE) {
		u64 desc = s2.l1[pa >> S2_L1_SHIFT];
		u64 *l2, *slot;

		if (!(desc & 2)) {
			hprintf("spy: %#llx not mapped\n", pa);
			continue;
		}
		l2 = (u64 *)(unsigned long)(desc & S2_OA_MASK);
		slot = &l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];
		if (valid)
			*slot |= S2_VALID;
		else
			*slot &= ~S2_VALID;
		asm volatile("dc civac, %0" :: "r"(slot));
	}
	mutex_unlock(&s2_lock);

	/*
	 * alle2 alone is not enough once the guest MMU is on: EL1&0 entries
	 * are stage-1+2 COMBINED and cache the final PA, so a guest TLB hit
	 * would bypass the spy entirely. VMALLS12E1IS invalidates all
	 * stage-1/stage-2 combined entries for every VMID (DDI0487M
	 * D8.14/DTLBILHF); VMALLE1IS would only cover the current VMID.
	 */
	asm volatile("tlbi alle2is\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb");
	hprintf("stage-2: %s [%#llx-%#llx]\n",
		valid ? "unspied" : "spying on", start, end);
}

/*
 * MMIO spy: unmap [start, end) from stage-2 (2 MiB granularity, rounded
 * out) so every guest access faults into EL2 and is emulated + logged by
 * hyp_s2_emulate().  hyp_mmio_unspy() puts the blocks back.
 */
void __hyp hyp_mmio_spy(u64 start, u64 end)
{
	s2_set_valid(start, end, false);
}

void __hyp hyp_mmio_unspy(u64 start, u64 end)
{
	s2_set_valid(start, end, true);
}

/**
 * hyp_s2_block() - the stage-2 block descriptor covering a physical address
 * @pa: address to look up
 * @desc: filled with the 2 MiB block descriptor
 *
 * Read out of the tables themselves: it maps nothing, touches no device and
 * changes no state, so a caller can watch what spying does to the map -
 * S2_VALID cleared, put back, and the attributes (memattr, S2AP, XN, shareability)
 * left exactly as they were - without ever accessing the region it describes.
 * That matters for a region whose blocks are being kept out of the map: the
 * guest faults on them, EL2's own accesses do not (mmio.el2-unaffected).
 *
 * Return: 0 when a level-2 table covers @pa, -ENOENT when it does not.
 */
int __hyp hyp_s2_block(u64 pa, u64 *desc)
{
	u64 entry = s2.l1 ? s2.l1[pa >> S2_L1_SHIFT] : 0;
	u64 *l2;

	if (!(entry & 2))
		return -ENOENT;

	l2 = (u64 *)(unsigned long)(entry & S2_OA_MASK);
	*desc = l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];

	return 0;
}

/*
 * Watch hooks: a range registered with hyp_mmio_watch() is spied (as
 * above) and, on every trapped access, hyp_s2_emulate() calls the hook
 * with the value involved and may MODIFY it: for stores before the
 * write-through to the real device (paravirtualisation point), for
 * loads after the physical read (override what the guest sees).  All
 * trapped accesses - watched or not - are replayed against the real
 * hardware; the spy never stubs anything.  Registering needs our stage-2
 * tables, so a watcher registers from its HYP_DRIVER probe: the Hyp DM pass
 * in hv_start_vm() runs right after hyp_s2_install().
 */
#define S2_MAX_WATCHES	8

struct s2_watch {
	u64 start, end;
	hyp_mmio_watch_fn fn;
	bool deny;		/* guest loads read zero, guest stores are dropped */
	hyp_mmio_emu_fn emu;	/* a virtual device behind the range */
};

static __hyp_data struct s2_watch watches[S2_MAX_WATCHES] = { {} };

static inline bool __hyp watch_used(const struct s2_watch *w)
{
	return w->fn || w->deny || w->emu;
}

/* The watch table and the stage-2 blocks a registration flips are one piece of
 * state, so they are registered and torn down under one lock.  A mutex rather
 * than a spinlock because registration is a driver-probe-time operation and the
 * owner field is the diagnostic: it says who is holding the map.  DEFINE_MUTEX
 * puts the object in .data.hyp, which matters: hyp code runs at its link-time
 * addresses, and .bss there is never zeroed - crt0_64.S clears BSS after
 * relocation, at the relocated copy's address - so a hyp lock has to be
 * initialized data, not a hole in the leftover .rela.dyn image.
 *
 * The trap path deliberately stays lock-free.  s2_watch_notify() runs with the
 * guest's own trapping write in hand; a lock held across a watched access would
 * be taken by the very CPU whose access is being replayed, and releasing it
 * would need that access to complete. */
static DEFINE_MUTEX(mmio_lock);

static void __hyp mmio_watch(u64 start, u64 end, hyp_mmio_watch_fn fn, bool deny)
{
	int i;

	if (!s2.l1) {
		hprintf("spy: stage-2 not installed, cannot watch [%#llx-%#llx]\n",
			start, end);
		return;
	}

	for (i = 0; i < S2_MAX_WATCHES; i++) {
		if (!watch_used(&watches[i]) || watches[i].start == start)
			break;
	}
	if (i == S2_MAX_WATCHES) {
		hprintf("spy: no free watch slot for [%#llx-%#llx]\n", start, end);
		return;
	}

	watches[i].start = start;
	watches[i].end = end;
	watches[i].fn = fn;
	watches[i].deny = deny;
	hyp_mmio_spy(start, end);
}

void __hyp hyp_mmio_watch(u64 start, u64 end, hyp_mmio_watch_fn fn)
{
	mutex_lock(&mmio_lock);
	mmio_watch(start, end, fn, false);
	/* s2_watch_notify() reads this table lock-free, on whichever core takes
	 * the trapped access; publish the slot to a cold core's view.
	 */
	hyp_clean_range(watches, sizeof(watches));
	mutex_unlock(&mmio_lock);
}

void __hyp hyp_mmio_deny(u64 start, u64 end)
{
	mutex_lock(&mmio_lock);
	mmio_watch(start, end, NULL, true);
	hyp_clean_range(watches, sizeof(watches));
	mutex_unlock(&mmio_lock);
}

/* A virtual device: the covering blocks are cleared from stage 2 as for a denied region, and
 * every guest access to [start, end) is handed to @fn instead of the hardware. */
void __hyp hyp_mmio_emulate(u64 start, u64 end, hyp_mmio_emu_fn fn)
{
	mutex_lock(&mmio_lock);
	mmio_watch(start, end, NULL, true);
	for (int i = 0; i < S2_MAX_WATCHES; i++) {
		if (watches[i].deny && watches[i].start == start) {
			watches[i].deny = false;
			watches[i].emu = fn;
		}
	}
	hyp_clean_range(watches, sizeof(watches));
	mutex_unlock(&mmio_lock);
}

static void __hyp mmio_unwatch(u64 start)
{
	int i;

	for (i = 0; i < S2_MAX_WATCHES; i++) {
		if (watch_used(&watches[i]) && watches[i].start == start) {
			hyp_mmio_unspy(watches[i].start, watches[i].end);
			watches[i].fn = NULL;
			watches[i].deny = false;
			watches[i].emu = NULL;
			return;
		}
	}
	hprintf("spy: no watch registered at %#llx\n", start);
}

void __hyp hyp_mmio_unwatch(u64 start)
{
	mutex_lock(&mmio_lock);
	mmio_unwatch(start);
	hyp_clean_range(watches, sizeof(watches));
	mutex_unlock(&mmio_lock);
}

static void __hyp s2_watch_notify(struct hyp_percpu *pcpu, u64 far, bool store,
				  u32 size, u64 *val)
{
	int i;
	/* Only the guest's accesses fault into stage 2; EL2's own never do
	 * (mmio.el2-unaffected), so this is always the EL1 world.  It does NOT say
	 * *which* EL1 code: after hv_start_vm() U-Boot runs at EL1 too, so a
	 * hook that paravirtualises for the kernel must also ask
	 * hyp_s2_is_uboot_access() below. */
	bool from_guest = (pcpu->vcpu.regs->spsr & PSR_MODE_MASK) != PSR_MODE_EL2h;

	for (i = 0; i < S2_MAX_WATCHES; i++)
		if (watches[i].fn && far >= watches[i].start && far < watches[i].end)
			watches[i].fn((int)pcpu->id, far, store, size, val,
				      from_guest);
}


/* Per-core record of the exception frame hyp_s2_emulate() is replaying, so a
 * watch hook can ask *who* made the trapped access.  Written on the trap path
 * before the hook runs and read by hyp_s2_is_uboot_access() on the same core. */
static __hyp_data struct pt_regs *s2_trap_regs[HYP_MAX_CPUS];

/**
 * hyp_guest_is_uboot() - is this guest frame U-Boot's own code?
 * @regs: the guest's frame
 *
 * hv_start_vm() drops U-Boot to EL1, so from then on U-Boot *is* the stage-2
 * guest: EL1 alone does not separate it from the kernel that boots later.  The
 * ELR does - U-Boot runs inside its link/relocated image, the kernel anywhere
 * else - and EL0 is always the OS's.  A frame from EL2 itself counts as
 * U-Boot's: it is not the OS.
 */
bool __hyp hyp_guest_is_uboot(const struct pt_regs *regs)
{
	u64 mode = regs->spsr & PSR_MODE_MASK;
	u64 elr;

	if (mode == PSR_MODE_EL0t)
		return false;
	if (mode != PSR_MODE_EL1h && mode != PSR_MODE_EL1t)
		return true;			/* EL2 hypervisor code */

	elr = regs->elr - hyp_reloc_off();
	return elr >= (u64)(unsigned long)__image_copy_start &&
	       elr <  (u64)(unsigned long)__image_copy_end;
}

/**
 * hyp_s2_is_uboot_access() - did U-Boot's own code make this trapped access?
 * @cpu: the cpu whose trap is being replayed (the hook's @cpu)
 *
 * Paravirtualising a watched device is for the kernel only; U-Boot's own
 * drivers poll the real hardware and must see their writes land (run 86:
 * masking U-Boot's own RPMH enable stalled every host rpmh transaction).
 */
bool __hyp hyp_s2_is_uboot_access(int cpu)
{
	if (cpu < 0 || cpu >= HYP_MAX_CPUS || !s2_trap_regs[cpu])
		return true;

	return hyp_guest_is_uboot(s2_trap_regs[cpu]);
}


/* Is this PA inside a registered spy watch region (stage-2-invalid)? */
static bool __hyp s2_watched(u64 far)
{
	int i;

	/*
	 * hyp_mmio_spy() invalidates the *whole* 2 MiB blocks a watch range
	 * touches, so every PA inside those blocks is watched: revalidating
	 * one hole in such a block (demand map or readable-fill) would
	 * silently un-spy the watched range inside it too.
	 */
	for (i = 0; i < S2_MAX_WATCHES; i++)
		if (watch_used(&watches[i]) &&
		    far >= (watches[i].start & ~(S2_BLOCK_SIZE - 1)) &&
		    far < ((watches[i].end + S2_BLOCK_SIZE - 1) &
			   ~(S2_BLOCK_SIZE - 1)))
			return true;
	return false;
}

/* Is this PA inside a range the guest has been denied (hyp_mmio_deny())? */
static bool __hyp s2_denied(u64 pa)
{
	int i;

	for (i = 0; i < S2_MAX_WATCHES; i++)
		if (watches[i].deny && pa >= watches[i].start && pa < watches[i].end)
			return true;
	return false;
}


/*
 * Stage-2 attributes for a block mapped on demand: the same split
 * hyp_s2_init() uses.  DRAM banks Normal WB + executable (the guest runs,
 * allocates and page-tables there), everything else Device-nGRE + XN.  A
 * Device block the guest turns out to use as Normal memory is upgraded
 * where it faults (s2_map_pa()).
 */
static u64 s2_default_attr(u64 pa)
{
	int i;

	for (i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		if (pa >= gd->bd->bi_dram[i].start &&
		    pa < gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size)
			return S2_WB | S2_RW | S2_AF | S2_SH_INNER;

	return S2_DEV | S2_RW | S2_AF | S2_SH_INNER | S2_XN;
}

/*
 * Make a physical address safe for EL2 to *read*: valid in stage-2 (stage-2
 * applies to EL2's own accesses too - HCR_EL2.VM=1, we are not VHE) and
 * covered by EL2's stage-1 map.  A guest page-table page or a guest
 * instruction is in a hole for no reason, so filling one has no side effects.
 * A watched block is invalid on purpose and stays that way: the answer there
 * is "unreadable", not "map it".
 *
 * Return: 0 when the caller may load from @pa, -EPROTO when it may not.
 */
static int __hyp s2_readable(u64 pa)
{
	u64 desc;

	if (hyp_s2_block(pa, &desc) != 0 || !(desc & S2_VALID)) {
		u64 l1i = pa >> S2_L1_SHIFT;
		u64 *l2, *slot;

		if (!s2.l1 || s2_watched(pa))
			return -EPROTO;

		mutex_lock(&s2_lock);
		/* another core may have mapped it while this one took the lock */
		if (hyp_s2_block(pa, &desc) != 0 || !(desc & S2_VALID)) {
			if (!(s2.l1[l1i] & 2))
				s2.l1[l1i] = (u64)(unsigned long)s2_alloc_table() | S2_TABLE;

			l2 = (u64 *)(unsigned long)(s2.l1[l1i] & S2_OA_MASK);
			slot = &l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];
			*slot = (pa & ~(S2_BLOCK_SIZE - 1)) | s2_default_attr(pa) | S2_VALID;
			asm volatile("dc civac, %0\n\tdsb ish" :: "r"(slot));
			asm volatile("tlbi alle2is\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb");
		}
		mutex_unlock(&s2_lock);
	}

	if (hyp_s1_covers(pa))
		return 0;

	return hyp_s1_map_pa(pa);
}

/*
 * Clean+invalidate the line holding @pa so the next EL2 load sees what the
 * guest last stored (see hyp_guest_va_to_pa()).
 */
static inline void __hyp hyp_guest_read_clean(u64 pa)
{
	asm volatile("dc civac, %0\n\tdsb sy" :: "r"(pa) : "memory");
}

/*
 * hyp_guest_va_to_pa() - translate a guest VA through the guest's own tables
 * @va: address to translate (ELR_EL2 or FAR_EL2, as the abort reported it)
 * @pa: filled with the physical address, may be NULL
 * @pte: filled with the leaf descriptor, may be NULL
 *
 * A stage-2 abort reports the address the *guest* used: FAR_EL2 is the input
 * address of the whole stage-1+2 translation.  Once the guest runs with its
 * MMU on that is a virtual address, and EL2's stage-1 map covers only the
 * hypervisor's layout - so reading the trapping instruction at ELR, or
 * replaying the access at FAR, is an EL2 data abort on the way to the guest's
 * (run 3: ESR=0x96000004, DFSC=0x04, a level-0 translation fault on the guest
 * kernel VA that had been left in ELR_EL2).
 *
 * The walk is what the hardware walker would do.  4 KiB granule only (U-Boot
 * and Linux both use it here); the initial lookup level follows TCR.T0SZ /
 * TCR.T1SZ as level = (TSZ - 16) / 9, so a 48-bit VA starts at level 0 and a
 * 39-bit VA at level 1.  VA[55] picks the half; TCR_EL1.EPD0/EPD1 disable
 * one.  Descriptor type is bit[1] read *with the level*: 0b11 is a Table
 * below L3 but a Page at L3; 0b01 is a Block below L3 (512 GiB at L0,
 * 1 GiB at L1, 2 MiB at L2) but reserved - invalid - at L3
 * (DDI0487M Table D8-48).
 *
 * Guest memory is read coherently: EL2 runs with SCTLR_EL2.C=0, so its loads
 * are Non-cacheable while the guest's stores are Write-Back.  Arm's
 * mismatched-attribute rule gives no coherency guarantee between the two, even
 * on one PE - a PTE or instruction the guest wrote a moment ago can still be
 * dirty in its D-cache with stale data in DRAM (run 110: a freshly written
 * kernel PTE read back as 0 and the guest was handed a bogus translation
 * fault).  hyp_guest_read_clean() cleans and invalidates the line first.
 * What does need checking is coverage, which is what s2_readable() is for.
 *
 * Return: 0 on success, -EFAULT on a guest translation fault, -EOPNOTSUPP for
 * a geometry this walker does not implement, -EPROTO when our own maps get in
 * the way.
 */
int __hyp hyp_guest_va_to_pa(u64 va, u64 *pa, u64 *pte)
{
	bool upper = !!(va & (1ULL << 55));
	u64 tcr = mrs(tcr_el1), ttbr, desc;
	u32 tsz, tg, tg_raw;
	int level;

	/*
	 * TCR_EL1 places the TTBR1-range fields in bits [31:16], mirroring
	 * the low half: T1SZ [21:16], EPD1 [23], TG1 [31:30]; T0SZ [5:0],
	 * EPD0 [7], TG0 [15:14] (DDI0487 TCR_EL1, confirmed with
	 * scripts/isa.py reg TCR_EL1 - the old masks read T1SZ from
	 * [39:34] and tested BIT(23) as "EPD0", which is really EPD1: a
	 * guest with TTBR1 disabled (U-Boot sets TCR_EPD1_DISABLE) failed
	 * every *lower*-range walk, silently).
	 */
	if (upper) {
		if (tcr & BIT(23)) {		/* TCR_EL1.EPD1: TTBR1 disabled */
			hprintf("walk: %#llx: TCR_EL1.EPD1 set, upper range disabled (tcr=%#lx)\n",
				va, tcr);
			return -EFAULT;
		}
		tsz = FIELD_GET(GENMASK(21, 16), tcr);	/* T1SZ [21:16] */
		tg = FIELD_GET(GENMASK(31, 30), tcr);	/* TG1 [31:30] */
		ttbr = mrs(ttbr1_el1) & S2_OA_MASK;
	} else {
		if (tcr & BIT(7)) {		/* TCR_EL1.EPD0: TTBR0 disabled */
			hprintf("walk: %#llx: TCR_EL1.EPD0 set, lower range disabled (tcr=%#lx)\n",
				va, tcr);
			return -EFAULT;
		}
		tsz = FIELD_GET(GENMASK(5, 0), tcr);	/* T0SZ [5:0] */
		tg = FIELD_GET(GENMASK(15, 14), tcr);	/* TG0 [15:14] */
		ttbr = mrs(ttbr0_el1) & S2_OA_MASK;
	}

	/*
	 * TCR_EL1.TG1 and TCR_EL1.TG0 encode the granule differently
	 * (DDI0487M C5.2.185; TG0 0b00=4K, 0b01=64K, 0b10=16K;
	 * TG1 0b01=16K, 0b10=4K, 0b11=64K, 0b00 reserved - compare
	 * linux/arch/arm64/include/generated/asm/sysreg-defs.h
	 * TCR_EL1_TG1_4K == 0b10).  Reading the upper-half value with
	 * the TG0 table is what broke the Linux walk: a 4K-page kernel
	 * (CONFIG_ARM64_VA_BITS=52 falling back to a 48-bit window at
	 * runtime: T1SZ=16, TG1=0b10) decoded as tg==2 == "16 KiB",
	 * started at level 0 with 16 KiB geometry, read TTBR1[1] and
	 * printed "level 0 idx 1 entry 0xab51bd008 -> 0x0 invalid" on
	 * every guest-VA fetch.  Normalize the upper-half encoding to
	 * this walker's TG0-style convention (0 = 4 KiB, 1 = 64 KiB
	 * rejected below, 2 = 16 KiB); tg_raw keeps the raw value for
	 * the diagnostics.
	 */
	tg_raw = tg;
	if (upper) {
		switch (tg) {
		case 1U:			/* TG1: 16 KiB */
			tg = 2U;
			break;
		case 2U:			/* TG1: 4 KiB */
			tg = 0U;
			break;
		case 3U:			/* TG1: 64 KiB: rejected below */
			tg = 1U;
			break;
		default:
			hprintf("walk: %#llx: TCR_EL1.TG1=%u is reserved (tcr=%#lx)\n",
				va, tg, tcr);
			return -EOPNOTSUPP;
		}
	} else if (tg == 3U) {
		hprintf("walk: %#llx: TCR_EL1.TG0=%u is reserved (tcr=%#lx)\n",
			va, tg, tcr);
		return -EOPNOTSUPP;
	}

	if (tcr & BIT(59)) {		/* TCR_EL1.DS (FEAT_LPA2): 52-bit tables */
		hprintf("walk: %#llx: TCR_EL1.DS set, 52-bit tables unsupported (tcr=%#lx)\n",
			va, tcr);
		return -EOPNOTSUPP;
	}

	/*
	 * Granule geometry, VMSAv8-64 with TCR_ELx.DS == 0:
	 * 4 KiB  (Tables D8-16/D8-18): L0..L3 index 9 bits each
	 *        (IA[47:39], IA[38:30], IA[29:21], IA[20:12]); initial level
	 *        0/1/2 for TnSZ 16-24 / 25-33 / 34 (TTST levels unsupported
	 *        here); Blocks at L0-L2 (512 GiB / 1 GiB / 2 MiB), Page at L3.
	 * 16 KiB (Tables D8-26/D8-27/D8-28): L0 index IA[47] (TnSZ 16 only),
	 *        L1 IA[46:36], L2 IA[35:25], L3 IA[24:14], all 11 bits;
	 *        initial level 0/1/2/3 for TnSZ 16 / 17-27 / 28-38 / 39-48;
	 *        Block only at L2 (32 MiB, IA[24:0] direct mapped), Page at
	 *        L3 (16 KiB, IA[13:0] direct).  L0/L1 Blocks are unsupported
	 *        unless DS is 1 (rejected above).
	 */
	if (tg == 2U) {
		if (tsz < 16 || tsz > 48) {
			hprintf("walk: %#llx: T%uSZ=%u TG%u=%u unsupported (tcr=%#lx)\n",
				va, upper ? 1U : 0U, tsz, upper ? 1U : 0U, tg_raw, tcr);
			return -EOPNOTSUPP;
		}
	} else if (tg || tsz < 16 || tsz > 34) {	/* 64 KiB granule unsupported */
		hprintf("walk: %#llx: T%uSZ=%u TG%u=%u unsupported (tcr=%#lx)\n",
			va, upper ? 1U : 0U, tsz, upper ? 1U : 0U, tg_raw, tcr);
		return -EOPNOTSUPP;
	}

	for (level = (tg == 2U) ? ((tsz == 16) ? 0 : (tsz - 17) / 11 + 1)
			       : (tsz - 16) / 9;
	     level <= 3; level++) {
		int shift = (tg == 2U) ? 47 - 11 * level : 39 - 9 * level;
		u64 idx_mask = (tg == 2U) ? (level == 0 ? 0x1ULL : 0x7ffULL) : 0x1ffULL;
		int oa_shift = (tg == 2U) ? 14 : 12;	/* table & page OA[47:oa_shift] */
		u64 entry = ttbr + 8 * ((va >> shift) & idx_mask);

		if (s2_readable(entry)) {
			hprintf("walk: table entry %#llx is not readable from EL2\n", entry);
			return -EPROTO;
		}

		hyp_guest_read_clean(entry);
		desc = *(volatile u64 *)entry;

		if (!(desc & 1)) {
			hprintf("walk: %#llx: level %d idx %llu entry %#llx -> %#llx invalid (ttbr raw=%#llx masked=%#llx T%uSZ=%u TG%u=%u)\n",
				va, level,
				(unsigned long long)((va >> shift) & idx_mask),
				entry, desc,
				(unsigned long long)(upper ? mrs(ttbr1_el1) : mrs(ttbr0_el1)),
				(unsigned long long)ttbr,
				upper ? 1U : 0U, tsz, upper ? 1U : 0U, tg_raw);
			return -EFAULT;
		}

		/*
		 * Table D8-48: bit[1] selects the type together with the
		 * level.  0b11 is a Table below L3 but a Page at L3; 0b01 is
		 * a Block below L3 but reserved (invalid) at L3.  (Testing
		 * (desc & 3) == 3 for "table" at *every* level rejected
		 * U-Boot's real L3 pages - every one of them is 0b11.)
		 */
		if (desc & 2) {
			if (level == 3) {	/* page: OA[47:oa_shift] */
				if (pa)
					*pa = (desc & GENMASK_ULL(47, oa_shift)) |
					      (va & ((1ULL << oa_shift) - 1));
				if (pte)
					*pte = desc;
				return 0;
			}
			ttbr = desc & GENMASK_ULL(47, oa_shift);	/* table: descend */
			continue;
		}

		if (level == 3 || (tg == 2U && level != 2)) {
			hprintf("walk: %#llx: level %d entry %#llx -> %#llx: 0b01 reserved here (16 KiB blocks are L2-only)\n",
				va, level, entry, desc);
			return -EFAULT;
		}

		{
			/* block: 4 KiB: 512 GiB/1 GiB/2 MiB at L0/L1/L2;
			 * 16 KiB: 32 MiB at L2 (Table D8-27) */
			int blk_shift = (tg == 2U) ? 25 : 39 - 9 * level;

			if (pa)
				*pa = (desc & GENMASK_ULL(47, blk_shift)) |
				      (va & ((1ULL << blk_shift) - 1));
			if (pte)
				*pte = desc;
			return 0;
		}
	}

	return -EFAULT;
}

/*
 * The physical address behind guest address @va, checked to be one EL2 may touch.  With
 * the guest MMU off the address already is physical.
 */
static int __hyp guest_copy_pa(u64 va, u64 *pa)
{
	if (!hyp_guest_mmu_on()) {
		*pa = va;

		return s2_readable(va);
	}

	if (hyp_guest_va_to_pa(va, pa, NULL))
		return -EFAULT;

	return s2_readable(*pa);
}

/**
 * hyp_guest_read() - copy @len bytes of guest memory at @va into @buf
 *
 * A page at a time through the guest's own translation.  Each line is cleaned and
 * invalidated first, so what the guest last stored in its caches is what is read.
 *
 * Return: 0, or a negative errno for the first page that does not translate.
 */
int __hyp hyp_guest_read(u64 va, void *buf, u32 len)
{
	u8 *out = buf;

	while (len) {
		u32 n = min_t(u32, len, SZ_4K - (va & (SZ_4K - 1)));
		u64 pa;
		int ret = guest_copy_pa(va, &pa);

		if (ret)
			return ret;

		for (u64 line = pa & ~63ULL; line < pa + n; line += 64)
			hyp_guest_read_clean(line);
		memcpy(out, (void *)(unsigned long)pa, n);
		va += n;
		out += n;
		len -= n;
	}

	return 0;
}

/**
 * hyp_guest_write() - copy @len bytes at @buf into guest memory at @va
 *
 * The guest's caches are cleaned first (so a dirty line cannot land on top of the new
 * bytes later), the bytes are stored and pushed to memory, and the instruction caches
 * are invalidated, since the debugger plants breakpoints in code.
 *
 * Return: 0, or a negative errno for the first page that does not translate.
 */
int __hyp hyp_guest_write(u64 va, const void *buf, u32 len)
{
	const u8 *in = buf;

	while (len) {
		u32 n = min_t(u32, len, SZ_4K - (va & (SZ_4K - 1)));
		u64 pa;
		int ret = guest_copy_pa(va, &pa);

		if (ret)
			return ret;

		for (u64 line = pa & ~63ULL; line < pa + n; line += 64)
			hyp_guest_read_clean(line);
		memcpy((void *)(unsigned long)pa, in, n);
		hyp_clean_range((void *)(unsigned long)pa, n);
		va += n;
		in += n;
		len -= n;
	}

	asm volatile("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

	return 0;
}

/**
 * hyp_fetch_guest_insn() - read the guest instruction at @elr without faulting
 * @elr: ELR_EL2 as the exit reported it
 * @insn: filled with the 32-bit word
 *
 * With the guest MMU off ELR is already a physical address and our identity
 * maps reach it.  With it on it has to be walked first - which is also the
 * answer to "why did the hypervisor panic while printing a guest exit".
 *
 * Return: 0 when @insn is the guest's instruction, negative on failure.
 */
int __hyp hyp_fetch_guest_insn(u64 elr, u32 *insn)
{
	u64 pa = elr & ~3UL;

	if (hyp_guest_mmu_on()) {
		int ret = hyp_guest_va_to_pa(pa, &pa, NULL);

		if (ret)
			return ret;
	}

	if (s2_readable(pa))
		return -EPROTO;

	hyp_guest_read_clean(pa);
	*insn = *(volatile u32 *)pa;
	return 0;
}

/*
 * "It faulted again on a block we just mapped" is a bug, not something to keep
 * fixing: either the flush did not take effect or the abort was never a missing
 * mapping.  Count the blocks mapped on demand so the caller can park on the
 * third hit for the same one instead of ping-ponging between EL2 and the guest.
 *
 * Initialized data, not a .bss hole: .bss inside .hyp is never zeroed (crt0_64.S
 * clears BSS after relocation, at the relocated copy's address).
 */
#define S2_MAP_HISTORY	8

struct s2_mapped { u64 pa; u32 hits; };

static __hyp_data struct s2_mapped s2_mapped[S2_MAP_HISTORY] = { { 0 } };

static u32 __hyp s2_map_hits(u64 pa)
{
	int i;

	for (i = 0; i < S2_MAP_HISTORY; i++) {
		if (s2_mapped[i].pa == pa)
			return ++s2_mapped[i].hits;
		if (!s2_mapped[i].pa) {
			s2_mapped[i].pa = pa;
			s2_mapped[i].hits = 1;
			return 1;
		}
	}

	s2_mapped[0].pa = pa;			/* full: recycle the first slot */
	s2_mapped[0].hits = 1;
	return 1;
}

/*
 * Map the 2 MiB block a stage-2 fault landed in, then let the guest run the
 * access natively: the caller does NOT advance ELR, so the trapping instruction
 * re-executes and the hardware walk finds the new mapping.  That is the whole
 * difference from the spy path.  The spy replays the access itself - it has to,
 * because it wants to see and possibly change the value - and replaying an
 * ordinary access would duplicate a device write and depend on a syndrome this
 * core does not report reliably (see hyp_s2_emulate()).
 */
static int __hyp s2_map_pa(struct hyp_percpu *pcpu, u64 pa)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	u32 dfsc = (u32)(regs->esr & 0x3f);
	u64 l1i = pa >> S2_L1_SHIFT, desc = 0;
	u64 *l2, *slot;
	u32 hits;

	if (!s2.l1) {
		hprintf("s2 fault cpu%u: no stage-2 tables\n", (unsigned)pcpu->id);
		return -1;
	}

	mutex_lock(&s2_lock);
	if (hyp_s2_block(pa, &desc) == 0) {
		if (desc & S2_VALID) {
			/* Access-flag fault (DFSC 0b0010LL): the block is there,
			 * it has just never been touched.  Set AF and retry. */
			if (dfsc >= 0x08 && dfsc <= 0x0b) {
				l2 = (u64 *)(unsigned long)(s2.l1[l1i] & S2_OA_MASK);
				slot = &l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];
				*slot |= S2_AF;
				asm volatile("dc civac, %0\n\tdsb ish" :: "r"(slot));
				asm volatile("tlbi alle2is\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb");
				mutex_unlock(&s2_lock);
				return 0;
			}
			/*
			 * Translation fault on a block that is valid now: another core
			 * mapped it between this core's fault and its taking the lock.
			 * A faulting walk leaves nothing in the TLB, so retrying is enough.
			 */
			if (dfsc >= 0x04 && dfsc <= 0x07) {
				mutex_unlock(&s2_lock);
				return 0;
			}
			/*
			 * Alignment fault (DFSC 0b100001) on a block this map made Device.  A stage-1
			 * alignment fault is the guest's own and never gets here, so this one is stage
			 * 2's: the guest mapped the page Normal (a DMA pool memremap()ed
			 * write-combined, an SRAM) and stage 2 turned it into Device, where DC ZVA
			 * (memset) and unaligned accesses fault.  Device is only this map's default
			 * for everything below DRAM, so the block becomes Normal: stage 1 decides, and
			 * a mapping the guest made Device stays Device (the stricter type wins).
			 * Break-before-make: the memory type of a valid entry is not rewritten in place.
			 */
			if (dfsc == 0x21 && (desc & S2_MEMATTR(0xf)) == S2_DEV) {
				l2 = (u64 *)(unsigned long)(s2.l1[l1i] & S2_OA_MASK);
				slot = &l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];
				*slot = desc & ~S2_VALID;
				asm volatile("dc civac, %0\n\tdsb ish" :: "r"(slot));
				asm volatile("tlbi alle2is\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb");
				*slot = (desc & ~S2_MEMATTR(0xf)) | S2_WB;
				asm volatile("dc civac, %0\n\tdsb ish\n\tisb" :: "r"(slot));
				mutex_unlock(&s2_lock);
				hdebug("s2 fault cpu%u: %#llx was Device at stage 2 but the guest uses it as Normal memory (alignment fault, ELR=%#lx): block %#llx is Normal now\n",
					(unsigned)pcpu->id, pa, (unsigned long)regs->elr,
					pa & ~(S2_BLOCK_SIZE - 1));
				return 0;
			}
			u64 fva = (u64)mrs(far_el2);
			u64 gpa = 0, gpte = 0;
			int gwalk = -1;

			/* hyp_guest_va_to_pa() is the guarded forward walk;
			 * pagetable_find_map() searches by output PA and
			 * descends every table entry - unsafe against guest
			 * tables (run 62: it dereferenced garbage and took
			 * an EL2 abort while instrumenting this fault). */
			if (hyp_guest_mmu_on() && fva)
				gwalk = hyp_guest_va_to_pa(fva, &gpa, &gpte);
			hprintf("s2 fault cpu%u: %#llx already maps %#llx, DFSC=%#x: not a missing mapping (va %#llx guest pte %#llx -> %#llx walk %d elr %#llx)\n",
				(unsigned)pcpu->id, pa, desc, dfsc, fva, gpte,
				gpa, gwalk, regs->elr);
			mutex_unlock(&s2_lock);
			return -1;
		}
		if (s2_watched(pa)) {
			hprintf("s2 fault cpu%u: %#llx is inside a watch region, refusing to map it\n",
				(unsigned)pcpu->id, pa);
			mutex_unlock(&s2_lock);
			return -1;
		}
	}

	if (!(s2.l1[l1i] & 2))
		s2.l1[l1i] = (u64)(unsigned long)s2_alloc_table() | S2_TABLE;

	l2 = (u64 *)(unsigned long)(s2.l1[l1i] & S2_OA_MASK);
	slot = &l2[(pa >> S2_BLOCK_SHIFT) & (S2_L1_ENTRIES - 1)];
	*slot = (pa & ~(S2_BLOCK_SIZE - 1)) | s2_default_attr(pa) | S2_VALID;
	asm volatile("dc civac, %0\n\tdsb ish" :: "r"(slot));
	asm volatile("tlbi alle2is\n\ttlbi vmalls12e1is\n\tdsb ish\n\tisb");
	hits = s2_map_hits(pa);
	mutex_unlock(&s2_lock);

	/* EL2 has to reach this block too, or the next trap in it is ours. */
	if (hyp_s1_map_pa(pa))
		hprintf("s2 fault cpu%u: mapped %#llx in stage-2 but not at EL2\n",
			(unsigned)pcpu->id, pa);

	if (hits > 2) {
		hprintf("s2 fault cpu%u: %#llx faulted again after being mapped\n",
			(unsigned)pcpu->id, pa);
		return -1;
	}

	hprintf("s2 fault cpu%u: mapped %#llx (%#llx) for FAR=%#lx DFSC=%#x ESR=%#lx ELR=%#lx\n",
		(unsigned)pcpu->id, pa & ~(S2_BLOCK_SIZE - 1), *slot,
		(unsigned long)mrs(far_el2), dfsc, (unsigned long)regs->esr,
		regs->elr);
	return 0;
}

/**
 * hyp_s2_map_fault() - serve a stage-2 fault by mapping the block it hit
 * @pcpu: the core that took the abort
 *
 *
* FAR_EL2 is the address the guest used, so it is walked first when the guest
* has its MMU on.  Used for instruction aborts (EC=0x20/0x21) and for the data
* aborts (EC=0x24) that are not inside a watch region.
*
* Return: 0 when the fault was mapped away and the guest may resume where it
* was, -1 when it is not ours to fix.
*/
int __hyp hyp_s2_map_fault(struct hyp_percpu *pcpu)
{
	u64 pa = mrs(far_el2);
	u32 ec = (u32)(pcpu->vcpu.regs->esr >> 26);	/* EC, hv.c's ESR_EC */

	/*
	 * EC=0x21 is our own fetch fault: FAR_EL2 holds an EL2 virtual
	 * address, and EL2's stage-1 map is identity, so it is already a
	 * physical address.  Walking it through the *guest's* stage-1
	 * would translate the wrong regime.
	 */
	if (ec != 0x21 && hyp_guest_mmu_on() && hyp_guest_va_to_pa(pa, &pa, NULL)) {
		hprintf("s2 fault cpu%u: guest VA %#lx does not translate (FAR=%#lx HPFAR=%#lx ESR=%#lx)\n",
			(unsigned)pcpu->id, pa, pa, (unsigned long)mrs(hpfar_el2),
			(unsigned long)pcpu->vcpu.regs->esr);
		return -1;
	}

	return s2_map_pa(pcpu, pa);
}

/*
 * EC=0x24: data abort from the guest (our own, EC=0x25, goes to _hv_panic()).
 *
 * Two different faults arrive here and only one of them is emulated:
 *
 * - the address is inside a spy watch region: its stage-2 block is invalid on
 *   purpose, so replay the access against the real hardware and report it.
 *   That needs the trapping instruction (this core's data-abort syndrome
 *   layout - ISV=24, SET=23, SAS=22:21, SSE=20, TTL=19:16, FN=11:5, DIR=0 -
 *   has untrustworthy FN/DIR, so direction, register and width come from the
 *   instruction) and it needs the *physical* address, which means walking the
 *   guest's stage-1 first once its MMU is on.
 *
 * - the address is anywhere else: a hole in our identity map, between the DRAM
 *   banks, above the last bank, or a region hyp_s2_init() had no reason to
 *   describe.  Map it and let the guest re-execute (s2_map_pa()); nothing is
 *   replayed, so no device write is duplicated and no syndrome is trusted.
 *   This is also the only sane answer to ISV=0 - a fault on the guest's own
 *   page-table walk has no access to replay, but it is always a missing mapping.
 *
 * The replay clears HCR_EL2.VM around the physical access into a spy-invalid
 * block and then invalidates the EL2 entry it created.  EL2 is not subject to
 * stage 2 (mmio.el2-unaffected), so that is not needed for the access to
 * succeed; it is kept because the replay predates the experiment.  The trace
 * line is printed *before* the
 * replay touches the address: if the access itself is what goes wrong (device
 * side effects, an EL2 abort), we still know what was attempted.
 *
 * Returns 0 when the guest may resume - ELR advanced for a replay, left alone
 * for a mapping - and -1 when the caller should park.
 */
int __hyp hyp_s2_emulate(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	s2_trap_regs[pcpu->id] = regs;
	unsigned long esr = regs->esr, far = mrs(far_el2);
	u64 val = 0, pa = far;
	u32 sas, rt, insn;
	/* Direction, target register and width decoded from the trapping
	 * instruction, not from the syndrome: see above. */
	bool store, sse, w32;

	if (hyp_guest_mmu_on()) {
		if (hyp_guest_va_to_pa(far, &pa, NULL)) {
			hprintf("s2 abort cpu%u: guest VA %#lx does not translate (ESR=%#lx ELR=%#lx)\n",
				(unsigned)pcpu->id, far, esr, regs->elr);
			return -1;
		}
	}

	if (!s2_watched(pa))
		return s2_map_pa(pcpu, pa);

	/*
	 * ISV is unreliable on this core: the garbled exits (EC 0x9 with
	 * ISS == 0) are real guest accesses whose direction, width and target
	 * register the instruction at ELR describes exactly.  A fault whose
	 * FAR does not even translate (a guest walk fault with no access to
	 * replay) was already refused above, so reaching a watched block
	 * means there is an access to replay; only refuse when the trapping
	 * instruction cannot be fetched.
	 */
	if (hyp_fetch_guest_insn(regs->elr, &insn)) {
		hprintf("s2 abort cpu%u FAR=%#lx (pa %#llx) ESR=%#lx ELR=%#lx: ISV=%u and the trapping instruction is unreadable, nothing to replay\n",
			(unsigned)pcpu->id, far, pa, esr, regs->elr,
			(esr >> 24) & 1U);
		return -1;
	}

	/* Field positions: opc[23:22] (00=STR, 01=LDR zero-extending, 10=LDRS
	 * into Xt, 11=LDRS into Wt), size[31:30] (00=B, 01=H, 10=W, 11=X),
	 * Rt[4:0].  Only the "load/store register" class with a general
	 * register is replayed: bits[29:27] == 0b111 and V (bit 26) == 0.
	 * Pairs (0b101), literal loads (0b011), exclusives (0b001), SIMD/FP
	 * (V=1), LSE atomics (bit 21 set with bits[11:10] == 0 and bit 24
	 * clear) and the pre/post-indexed forms are refused: the last would
	 * also have to write the base register back, which is not in the
	 * exit frame when it is SP, and no driver uses it for MMIO. */
	{
		u32 opc = (insn >> 22) & 3;
		bool not_ldst_reg = ((insn >> 27) & 7U) != 7U || (insn & BIT(26));
		bool atomic = (insn & BIT(21)) && !(insn & BIT(24)) &&
			      !((insn >> 10) & 3U);
		bool indexed = !(insn & BIT(21)) && !(insn & BIT(24)) &&
			       (((insn >> 10) & 3U) == 1U || ((insn >> 10) & 3U) == 3U);
		bool prefetch = opc == 2U && ((insn >> 30) & 3U) == 3U;

		if (not_ldst_reg || atomic || indexed || prefetch) {
			hprintf("s2 abort cpu%u: unsupported access insn=%#08x (pa %#llx)\n",
				(unsigned)pcpu->id, insn, pa);
			return -1;
		}
		store = (opc == 0);
		sse   = (opc >= 2);
		w32   = (opc == 3);
		rt    = insn & 0x1f;
		/* size[31:30] is the access width verbatim: 00=B, 01=H, 10=W,
		 * 11=X - the same encoding the sas field would have held. */
		sas   = (insn >> 30) & 3U;
	}

	/* A virtual device (hyp_mmio_emulate()): the handler answers loads and takes stores. */
	for (int i = 0; i < S2_MAX_WATCHES; i++) {
		u64 v;

		if (!watches[i].emu || pa < watches[i].start || pa >= watches[i].end)
			continue;

		v = store && rt != 31 ? regs->regs[rt] : 0;
		if (sas < 3)
			v &= (1ULL << (8u << sas)) - 1;
		if (!watches[i].emu((int)pcpu->id, pa, store, 1u << sas, &v))
			v = 0;			/* an access the device does not take reads zero */
		if (!store && rt != 31) {
			if (sse && sas < 3) {
				v = (u64)((s64)(v << (64 - (8u << sas))) >> (64 - (8u << sas)));
				if (w32)
					v &= 0xffffffffULL;
			}
			regs->regs[rt] = v;
		}
		regs->elr += 4;
		return 0;
	}

	/* A denied range is not the guest's any more: loads read zero, stores
	 * are dropped, and nothing reaches the device. */
	if (s2_denied(pa)) {
		if (!store && rt != 31)
			regs->regs[rt] = 0;
		hdebug("mmio: guest %s of denied %#llx ignored\n",
		       store ? "write" : "read", pa);
		regs->elr += 4;
		return 0;
	}

	if (store)
		hdebug("mmio#%u cpu%u wr %#llx size=%u x%u=%#llx (FAR=%#lx ESR=%#lx ELR=%#lx)\n",
			++s2.emulated, (unsigned)pcpu->id, pa, 1u << sas, rt,
			rt == 31 ? 0ULL : regs->regs[rt], far, esr, regs->elr);
	else
		hdebug("mmio#%u cpu%u rd %#llx size=%u -> x%u (FAR=%#lx ESR=%#lx ELR=%#lx)\n",
			++s2.emulated, (unsigned)pcpu->id, pa, 1u << sas, rt,
			far, esr, regs->elr);

	/*
	 * A watched block that is DRAM (stage-2 Normal WB) can have dirty guest
	 * cache lines that hold newer data than memory (load) or write back over
	 * our replay (store): clean the line to the PoC first.  Caches are PIPT,
	 * so a DC by PA at EL2 hits the line no matter which guest VA created
	 * it.  Device blocks - every MMIO watch - cannot be cached by the guest
	 * and need nothing.  (This used to search the guest's TTBR0 tables for
	 * the PA with pagetable_find_map(), which descends every entry of a
	 * table EL2 reads uncached: once userspace ran its TTBR0 held stale
	 * garbage and the walk took an EL2 abort, run 115.)
	 */
	if ((s2_default_attr(pa) & S2_MEMATTR(0xf)) != S2_DEV) {
		u64 line = pa & ~63ULL;

		asm volatile("dc civac, %0\n\tdsb nsh" :: "r"(line));
	}

	/*
	 * Spy hook first for stores: it may modify the value before it is
	 * written through to the real device (paravirtualisation point).
	 * Watched stores ARE replayed - the spy never stubs anything.
	 */
	if (store) {
		val = rt == 31 ? 0 : regs->regs[rt];
		if (sas < 3)
			val &= (1ULL << (8u << sas)) - 1;
		s2_watch_notify(pcpu, pa, true, 1u << sas, &val);
	}

	/* Only a watched block gets here, and by definition it is invalid in
	 * stage-2: with the EL2 MMU on, our own access into it would take a
	 * stage-2 fault, so drop VM around the physical access and flush the
	 * EL2 entry afterwards. */
	msr(hcr_el2, mrs(hcr_el2) & ~HCR_VM);
	asm volatile("isb");

	switch (1u << sas) {
	case 1:
		if (store)
			writeb((u8)val, pa);
		else {
			val = readb(pa);
			if (sse)
				val = (u64)(s64)(s8)val;
		}
		break;
	case 2:
		if (store)
			writew((u16)val, pa);
		else {
			val = readw(pa);
			if (sse)
				val = (u64)(s64)(s16)val;
		}
		break;
	case 4:
		if (store)
			writel((u32)val, pa);
		else {
			val = readl(pa);
			if (sse)
				val = (u64)(s64)(s32)val;
		}
		break;
	default: /* 8 */
		if (store)
			writeq(val, pa);
		else
			val = readq(pa);
		break;
	}

	msr(hcr_el2, mrs(hcr_el2) | HCR_VM);
	asm volatile("tlbi vale2is, %0\n\tdsb ish\n\tisb" :: "r"(pa));

	if (!store) {
		if (w32)	/* LDRS* Wt: sign-extend to 32 bits, upper half zero */
			val &= 0xffffffffULL;
		/* The hook may override what the guest sees. */
		s2_watch_notify(pcpu, pa, false, 1u << sas, &val);
		if (rt != 31)
			regs->regs[rt] = val;
		hdebug("mmio: x%u=%#llx\n", rt, val);
	}

	regs->elr += 4;
	return 0;
}
