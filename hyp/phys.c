// SPDX-License-Identifier: GPL-2.0+
/*
 * Physical memory from EL2, with a way to survive a bad address.
 *
 * hyp_phys_read() and hyp_phys_write() reach DRAM and device registers by physical address
 * through EL2's own stage-1 map (identity, with blocks that are not mapped yet filled in
 * on demand).  Only addresses the DRAM map or the device tree names are touched at all
 * (phys_allowed()): the secure world's memory and the holes between devices answer with
 * an error the platform turns into a board reset, which no handler here could catch.
 *
 * A register window that is powered down or refuses the access raises an external abort:
 * a synchronous data abort for a load, an SError some time later for a store.  Neither has a
 * handler at EL2 - both are a hang or a panic - so every access to a device is made by one
 * A64 instruction between the marks of a per-core probe, and hyp/hv.c hands the abort to
 * hyp_phys_fault() while the probe is up: it records the fault and carries on after the
 * instruction.  After each access the core waits for the access to complete and unmasks
 * SError for a moment, so that an error that was going to arrive does so inside the probe.
 *
 * What cannot be survived is an access the bus never answers (a gated clock domain): that
 * stalls the core.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/global_data.h>
#include <asm/ptrace.h>
#include <asm/system.h>
#include <errno.h>
#include <hypervisor.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>
#include <linux/libfdt.h>

#include "hv.h"

DECLARE_GLOBAL_DATA_PTR;

#define PAGE_SZ		4096UL

/* One cache line each: a core's own flags, read and written by that core only */
struct hyp_probe {
	u32 active;		/* an access is in flight: aborts are ours */
	u32 faulted;		/* one was taken */
} __aligned(64);

static __hyp_data struct hyp_probe probe[HYP_MAX_CPUS] = { { 0 } };

/**
 * hyp_phys_fault() - an abort was taken at EL2: is it a probe's?
 * @regs: the exception frame
 * @precise: a synchronous abort, which stops at the faulting instruction (resume after it);
 *	an SError arrives after the instruction that caused it and resumes where it is
 *
 * Return: true when a probe was up on this core and the abort has been dealt with.
 */
bool __hyp hyp_phys_fault(struct pt_regs *regs, bool precise)
{
	int id = hyp_cpu_num();

	if (id < 0 || id >= HYP_MAX_CPUS || !probe[id].active ||
	    (regs->spsr & PSR_MODE_MASK) <= PSR_MODE_EL1h)
		return false;

	probe[id].faulted = 1;
	if (precise)
		regs->elr += 4;		/* the access is one A64 instruction (the asm below) */

	return true;
}

/* The tail of every probed access: wait for it, then let an SError that is coming arrive */
#define PROBE_SETTLE	"dsb	sy\n\t" "isb\n\t" "msr	daifclr, #4\n\t" "isb\n\t" "msr	daifset, #4"

static int __hyp probe_ld32(u64 addr, u32 *val)
{
	struct hyp_probe *p = &probe[hyp_cpu_num()];
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	p->faulted = 0;
	p->active = 1;
	asm volatile("mov	%w0, #0\n\t"
		     "ldr	%w0, [%1]\n\t"
		     PROBE_SETTLE : "=&r"(v) : "r"(addr) : "memory");
	p->active = 0;
	local_irq_restore(flags);

	if (p->faulted)
		return -EFAULT;
	*val = v;

	return 0;
}

static int __hyp probe_ld8(u64 addr, u8 *val)
{
	struct hyp_probe *p = &probe[hyp_cpu_num()];
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	p->faulted = 0;
	p->active = 1;
	asm volatile("mov	%w0, #0\n\t"
		     "ldrb	%w0, [%1]\n\t"
		     PROBE_SETTLE : "=&r"(v) : "r"(addr) : "memory");
	p->active = 0;
	local_irq_restore(flags);

	if (p->faulted)
		return -EFAULT;
	*val = v;

	return 0;
}

static int __hyp probe_st32(u64 addr, u32 val)
{
	struct hyp_probe *p = &probe[hyp_cpu_num()];
	unsigned long flags;

	local_irq_save(flags);
	p->faulted = 0;
	p->active = 1;
	asm volatile("str	%w1, [%0]\n\t"
		     PROBE_SETTLE :: "r"(addr), "r"(val) : "memory");
	p->active = 0;
	local_irq_restore(flags);

	return p->faulted ? -EFAULT : 0;
}

static int __hyp probe_st8(u64 addr, u8 val)
{
	struct hyp_probe *p = &probe[hyp_cpu_num()];
	unsigned long flags;

	local_irq_save(flags);
	p->faulted = 0;
	p->active = 1;
	asm volatile("strb	%w1, [%0]\n\t"
		     PROBE_SETTLE :: "r"(addr), "r"((u32)val) : "memory");
	p->active = 0;
	local_irq_restore(flags);

	return p->faulted ? -EFAULT : 0;
}

/**
 * hyp_phys_probe32() - one probed word read at EL2 address @addr, for the self-tests
 * @addr: an address EL2 translates (identity: the physical address of what is mapped)
 * @val: filled with the word
 *
 * Return: 0, or -EFAULT when the access aborted.
 */
int __hyp hyp_phys_probe32(u64 addr, u32 *val)
{
	return probe_ld32(addr, val);
}

static bool __hyp phys_is_dram(u64 pa)
{
	for (int i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		if (pa >= gd->bd->bi_dram[i].start && pa < gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size)
			return true;

	return false;
}

/*
 * What a physical access may touch: DRAM, and the register windows the device tree describes
 * (every enabled node's `reg`, merged and sorted).  An address that is neither is not
 * probed: the secure world's memory and the holes in the map do not answer with an abort a
 * probe could survive, the platform resets the board.
 */
#define PHYS_MAX_RANGES	2048

struct phys_range {
	u64 start, end;			/* [start, end) */
};

static __hyp_data struct phys_range *ranges;
static __hyp_data int nranges;

static void __hyp phys_add_range(u64 start, u64 size)
{
	if (!size || nranges == PHYS_MAX_RANGES)
		return;

	ranges[nranges].start = start;
	ranges[nranges].end = start + size;
	nranges++;
}

/* A cell count property of @off, or @def */
static int __hyp phys_cells(const void *fdt, int off, const char *prop, int def)
{
	const fdt32_t *p = fdt_getprop(fdt, off, prop, NULL);

	return p ? fdt32_to_cpu(*p) : def;
}

static u64 __hyp phys_cell_val(const fdt32_t *p, int cells)
{
	return cells == 2 ? ((u64)fdt32_to_cpu(p[0]) << 32) | fdt32_to_cpu(p[1]) : fdt32_to_cpu(p[0]);
}

/*
 * Every enabled node's register windows, except memory and the reserved carve-outs, in one
 * pass over the blob: fdt_next_node() is linear, where asking ofnode for each sibling in turn
 * rescans the siblings before it and took minutes with the caches off.
 */
static void __hyp phys_collect(void)
{
	const void *fdt = gd->fdt_blob;
	int ac[16], sc[16], depth = 0, skip = -1;

	ac[0] = phys_cells(fdt, 0, "#address-cells", 2);
	sc[0] = phys_cells(fdt, 0, "#size-cells", 1);

	for (int off = fdt_next_node(fdt, 0, &depth); off >= 0 && depth > 0;
	     off = fdt_next_node(fdt, off, &depth)) {
		const char *name = fdt_get_name(fdt, off, NULL), *status;
		const fdt32_t *reg;
		int len, a = ac[depth - 1], s = sc[depth - 1];

		if (skip >= 0 && depth > skip)
			continue;
		skip = -1;

		status = fdt_getprop(fdt, off, "status", NULL);
		if (depth > 14 || !name || !strncmp(name, "memory", 6) ||
		    !strcmp(name, "reserved-memory") || !strcmp(name, "cpus") ||
		    (status && strcmp(status, "okay") && strcmp(status, "ok"))) {
			skip = depth;
			continue;
		}

		ac[depth] = phys_cells(fdt, off, "#address-cells", 2);
		sc[depth] = phys_cells(fdt, off, "#size-cells", 1);

		reg = fdt_getprop(fdt, off, "reg", &len);
		if (!reg || (a != 1 && a != 2) || (s != 1 && s != 2))
			continue;
		for (int i = 0; (i + 1) * (a + s) * 4 <= len; i++)
			phys_add_range(phys_cell_val(reg + i * (a + s), a),
				       phys_cell_val(reg + i * (a + s) + a, s));
	}
}

/**
 * hyp_phys_init() - collect the register windows physical accesses may touch, once at boot
 */
void __hyp hyp_phys_init(void)
{
	int n = 0;

	ranges = hyp_mem_alloc(PHYS_MAX_RANGES * sizeof(*ranges));
	phys_collect();

	/* sort by start (insertion sort: a thousand entries, once), then merge the overlaps */
	for (int i = 1; i < nranges; i++) {
		struct phys_range r = ranges[i];
		int j = i - 1;

		for (; j >= 0 && ranges[j].start > r.start; j--)
			ranges[j + 1] = ranges[j];
		ranges[j + 1] = r;
	}
	for (int i = 0; i < nranges; i++) {
		if (n && ranges[i].start <= ranges[n - 1].end) {
			ranges[n - 1].end = max(ranges[n - 1].end, ranges[i].end);
		} else {
			ranges[n++] = ranges[i];
		}
	}
	nranges = n;
	hyp_clean_range(ranges, nranges * sizeof(*ranges));
	hyp_clean_range(&ranges, sizeof(ranges));
	hyp_clean_range(&nranges, sizeof(nranges));

	hprintf("phys: %d register windows from the device tree\n", nranges);
}

static bool __hyp phys_allowed(u64 pa, u32 n)
{
	int lo = 0, hi = nranges - 1;

	for (int i = 0; i < CONFIG_NR_DRAM_BANKS; i++)
		if (pa >= gd->bd->bi_dram[i].start && pa + n <= gd->bd->bi_dram[i].start + gd->bd->bi_dram[i].size)
			return true;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (pa + n <= ranges[mid].start)
			hi = mid - 1;
		else if (pa >= ranges[mid].end)
			lo = mid + 1;
		else
			return pa >= ranges[mid].start && pa + n <= ranges[mid].end;
	}

	return false;
}

/*
 * Make sure the @n bytes at @pa may be touched and that EL2's stage-1 map reaches them: 64 GiB is
 * all that map describes (T0SZ 28)
 */
static int __hyp phys_reach(u64 pa, u32 n)
{
	if ((pa + n - 1) >> 36 || !phys_allowed(pa, n))
		return -EFAULT;

	if (hyp_s1_covers(pa))
		return 0;

	return hyp_s1_map_pa(pa) ? -EFAULT : 0;
}

/**
 * hyp_phys_read() - copy @len bytes of physical memory at @pa into @buf
 *
 * DRAM is read after cleaning and invalidating the lines, so what the guest last stored in its
 * caches is what is read.  Device memory is read a word at a time where @pa and the length
 * are word aligned (what a register needs), by bytes elsewhere.
 *
 * Return: 0, or -EFAULT for the first page that is not there or aborts.
 */
int __hyp hyp_phys_read(u64 pa, void *buf, u32 len)
{
	u8 *out = buf;

	while (len) {
		u32 n = min_t(u32, len, PAGE_SZ - (pa & (PAGE_SZ - 1)));
		int ret = phys_reach(pa, n);

		if (ret)
			return ret;

		if (phys_is_dram(pa)) {
			for (u64 line = pa & ~63ULL; line < pa + n; line += 64)
				asm volatile("dc civac, %0" :: "r"(line) : "memory");
			asm volatile("dsb sy" ::: "memory");
			memcpy(out, (void *)(unsigned long)pa, n);
		} else {
			for (u32 i = 0; i < n;) {
				if (!((pa + i) & 3) && n - i >= 4) {
					u32 v;

					ret = probe_ld32(pa + i, &v);
					if (!ret)
						memcpy(out + i, &v, 4);
					i += 4;
				} else {
					ret = probe_ld8(pa + i, out + i);
					i++;
				}
				if (ret)
					return ret;
			}
		}

		pa += n;
		out += n;
		len -= n;
	}

	return 0;
}

/**
 * hyp_phys_write() - copy @len bytes at @buf into physical memory at @pa
 *
 * DRAM is written through and the instruction caches are invalidated, since the bytes may be
 * code; device memory like hyp_phys_read() reads it, a word at a time where it is aligned.
 *
 * Return: 0, or -EFAULT for the first page that is not there or aborts.
 */
int __hyp hyp_phys_write(u64 pa, const void *buf, u32 len)
{
	const u8 *in = buf;
	bool dram = false;

	while (len) {
		u32 n = min_t(u32, len, PAGE_SZ - (pa & (PAGE_SZ - 1)));
		int ret = phys_reach(pa, n);

		if (ret)
			return ret;

		if (phys_is_dram(pa)) {
			dram = true;
			for (u64 line = pa & ~63ULL; line < pa + n; line += 64)
				asm volatile("dc civac, %0" :: "r"(line) : "memory");
			asm volatile("dsb sy" ::: "memory");
			memcpy((void *)(unsigned long)pa, in, n);
			for (u64 line = pa & ~63ULL; line < pa + n; line += 64)
				asm volatile("dc civac, %0" :: "r"(line) : "memory");
			asm volatile("dsb sy" ::: "memory");
		} else {
			for (u32 i = 0; i < n;) {
				if (!((pa + i) & 3) && n - i >= 4) {
					u32 v;

					memcpy(&v, in + i, 4);
					ret = probe_st32(pa + i, v);
					i += 4;
				} else {
					ret = probe_st8(pa + i, in[i]);
					i++;
				}
				if (ret)
					return ret;
			}
		}

		pa += n;
		in += n;
		len -= n;
	}

	if (dram)
		asm volatile("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");

	return 0;
}
