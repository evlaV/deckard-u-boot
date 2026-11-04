// SPDX-License-Identifier: GPL-2.0+
/*
 * Hypervisor scratch memory: carve the DTB's hyp_mem reservation with a
 * bounds-checked bump allocator instead of scattering hardcoded addresses
 * through hyp/.
 *
 * The reservation is `hyp_mem: hyp@80000000` in
 * dts/upstream/src/arm64/qcom/sm8650.dtsi (no-map, 0x80000000..0x80e00000,
 * 14 MiB; see scripts/MEMORY.md). hyp_mem_init() reads base/size from the
 * DTB, so moving the node moves everything below with it.
 *
 * Layout inside the reservation (bottom-up pools, top-down stacks, the
 * allocator refuses to let them meet):
 *
 *   base + 0x000000   stage-2 table pool   256 KiB  (hyp/mmio.c)
 *   base + 0x040000   stage-1 table pool   128 KiB  (hyp/s1.c)
 *   ...               free (bump frontier)
 *   end - 0x020000    per-cpu EL2 stacks: 8 slots x 8 KiB; cpu N owns
 *                     [end - 0x2000*(N+1), end - 0x2000*N) and grows DOWN
 *
 * If the node is missing, too small, or a pool would cross the stacks,
 * the hypervisor hangs loudly rather than clobbering firmware memory.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <dm/ofnode.h>
#include <hang.h>
#include <linux/sizes.h>
#include <hypervisor.h>
#include <linux/types.h>
#include <log.h>

#include "hv.h"

#define HYP_STACK_SIZE	0x2000		/* per-cpu EL2 stack (grows down) */
#define HYP_STACK_SLOTS	8		/* percpu[8] in hyp/hv.c */

static __hyp_data struct {
	u64 base, end;		/* the DT reservation */
	u64 next;		/* bump frontier, bottom-up */
	u64 stack_top;		/* EL2 stacks live in [stack_top, end) */
} hyp_mem = { 0 };

void __hyp hyp_mem_init(void)
{
	ofnode node = ofnode_path("/reserved-memory/hyp@80000000");
	fdt_size_t size;
	fdt_addr_t addr;

	addr = ofnode_get_addr_size_index(node, 0, &size);
	if (addr == FDT_ADDR_T_NONE || (u64)size < HYP_STACK_SLOTS * HYP_STACK_SIZE + SZ_512K) {
		hprintf("hyp_mem: /reserved-memory/hyp@80000000 missing or too small\n");
		hang();
	}

	hyp_mem.base = addr;
	hyp_mem.end = addr + size;
	hyp_mem.next = addr;
	hyp_mem.stack_top = hyp_mem.end - HYP_STACK_SLOTS * HYP_STACK_SIZE;

	hprintf("hyp_mem: [%#llx-%#llx] pools from %#llx up to %#llx, stacks >= %#llx\n",
		hyp_mem.base, hyp_mem.end, hyp_mem.base, hyp_mem.stack_top,
		hyp_mem.stack_top);
}

/* 4 KiB-aligned bump allocation; returns NULL (loudly) past the frontier */
void *__hyp hyp_mem_alloc(u64 size)
{
	u64 p = ALIGN(hyp_mem.next, SZ_4K);

	if (p + size > hyp_mem.stack_top) {
		hprintf("hyp_mem: alloc %#llx at %#llx would hit the EL2 stacks (%#llx)\n",
			size, p, hyp_mem.stack_top);
		hang();
	}
	hyp_mem.next = p + size;
	return (void *)p;
}

/* Top of the per-cpu EL2 stack (the stack grows down from here) */
u64 __hyp hyp_mem_stack(int id)
{
	if (id < 0 || id >= HYP_STACK_SLOTS) {
		hprintf("hyp_mem: cpu %d has no stack slot (%d max)\n", id, HYP_STACK_SLOTS);
		hang();
	}
	return hyp_mem.end - (u64)HYP_STACK_SIZE * (id + 1);
}
