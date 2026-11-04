// SPDX-License-Identifier: GPL-2.0+
/*
 * Which core is which.
 *
 * The SM8650's eight CPUs differ in MPIDR **Aff1**, not Aff0: the DT's
 * cpu@0..cpu@700 are 0x0, 0x100 ... 0x700 (scripts/SMP.md).  Keying a per-cpu
 * table with `mpidr_el1 & 0xff` - what hyp/hv.c used to do - therefore maps
 * every core on this board onto slot 0.  This file builds the affinity -> slot
 * map from the DT's /cpus children, because the DT is the source of truth for
 * the board's CPU geometry, and hands out the slot ids that the per-cpu table
 * and the per-cpu EL2 stacks (hyp/mem.c) are indexed by.
 *
 * hyp_cpu_init() runs at EL2, before relocation, on the pre-reloc DTB - the
 * same context hyp/mem.c reads its reservation from.
 */

#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <dm/ofnode.h>
#include <hang.h>
#include <hypervisor.h>
#include <linux/types.h>
#include <log.h>
#include <string.h>

#include "hv.h"

struct hyp_cpu {
	u64 mpidr;
};

static __hyp_data struct hyp_cpu cpus[HYP_MAX_CPUS] = { {} };
static __hyp_data int cpu_count;

void __hyp hyp_cpu_init(void)
{
	ofnode cpu_node, cpu;
	int slot = 0;
	u64 mpidr;

	cpu_node = ofnode_path("/cpus");
	if (!ofnode_valid(cpu_node)) {
		hprintf("cpu: no /cpus node in the DT\n");
		hang();
	}

	ofnode_for_each_subnode(cpu, cpu_node) {
		const char *type = ofnode_read_string(cpu, "device_type");

		if (!type || strcmp(type, "cpu"))
			continue;

		if (ofnode_read_u64(cpu, "reg", &mpidr)) {
			hprintf("cpu: %s has no reg\n", ofnode_get_name(cpu));
			continue;
		}

		if (slot >= HYP_MAX_CPUS) {
			hprintf("cpu: %s does not fit in %d slots\n",
				ofnode_get_name(cpu), HYP_MAX_CPUS);
			hang();
		}

		cpus[slot].mpidr = mpidr;
		hdebug("cpu: slot %d = %s mpidr=%#llx\n", slot,
		       ofnode_get_name(cpu), mpidr);
		slot++;
	}

	if (!slot) {
		hprintf("cpu: /cpus lists no CPU\n");
		hang();
	}

	cpu_count = slot;
	/* A core PSCI hands us starts with its caches off; the map it looks
	 * itself up in has to be in memory, not in CPU0's caches. */
	hyp_clean_range(cpus, sizeof(cpus));
	hyp_clean_range(&cpu_count, sizeof(cpu_count));
	hprintf("cpu: %d CPUs from the DT, slot = DT order (affinity %#llx..%#llx)\n",
		cpu_count, cpus[0].mpidr, cpus[cpu_count - 1].mpidr);
}

/**
 * hyp_cpu_id() - slot of a core, by the affinity the DT gives it
 * @mpidr: MPIDR (or affinity) to look up; only Aff2:Aff1:Aff0 are compared
 *
 * Return: the slot number, or -1 if the DT does not describe this core.
 */
int __hyp hyp_cpu_id(u64 mpidr)
{
	u64 aff = mpidr & MPIDR_AFFINITY;

	for (int i = 0; i < cpu_count; i++)
		if (cpus[i].mpidr == aff)
			return i;

	return -1;
}

/**
 * hyp_cpu_mpidr() - MPIDR of the core in a slot
 * @slot: slot number below hyp_cpu_count()
 *
 * Return: the MPIDR from the DT, or 0 if the slot is empty.
 */
u64 __hyp hyp_cpu_mpidr(int slot)
{
	if (slot < 0 || slot >= cpu_count)
		return 0;

	return cpus[slot].mpidr;
}

/**
 * hyp_cpu_count() - number of CPUs the DT describes
 *
 * Return: CPU count, 0 before hyp_cpu_init() has run.
 */
int __hyp hyp_cpu_count(void)
{
	return cpu_count;
}
