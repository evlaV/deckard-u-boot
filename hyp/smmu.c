// SPDX-License-Identifier: GPL-2.0+
/*
 * The apps SMMU, as EL2 hands it over to the OS.
 *
 * U-Boot's own SMMU driver (drivers/iommu/qcom-hyp-smmu.c) gives every peripheral it drives -
 * UFS, SD, the GENI engines, USB - a stream mapping that points at a bypass context bank,
 * because the firmware leaves unmatched streams faulting.  When U-Boot starts the OS those
 * mappings are still there, and Linux's arm-smmu takes every valid stream mapping it finds
 * for "boot mappings" it must not touch (arm-smmu-qcom.c, qcom_smmu_cfg_probe()).  The next
 * stream the OS tries to map that overlaps one of them - the GENI QUP at SID 0xa3 mask 0x3
 * over U-Boot's 0xa3 mask 0 - fails with -EINVAL, and the whole SMMU driver gives up:
 * "Failed to register iommu".  Every device behind the SMMU is then left in bypass, with the
 * attributes of its own master instead of the cacheable, shareable ones a translated context
 * gives it, and a device the DT calls dma-coherent (the UFS host: its descriptors are read
 * stale from DRAM, the controller clears the doorbell without raising an interrupt) does not
 * work.
 *
 * Hypervisor-less, the same boot works because the hypervisor's own SMMU emulation
 * (Gunyah's) never lets U-Boot's mappings reach the hardware.  EL2 does the equivalent: when
 * the guest says it is done with its devices (HV_OS_HANDOFF) it removes the stream mappings
 * U-Boot made, and keeps those of the masters EL2 drives itself.  The firmware's own entries
 * (a bypass type with attributes set, owned by the secure world) are not U-Boot's and stay.
 *
 * The second half of the hand-over is for the processors EL2 boots on the OS's behalf
 * (hyp/pas.c).  They start after the OS's SMMU driver has probed, and their own streams (a
 * DSP's cores and DMA engines) are not described to the OS, which has no business mapping
 * them.  Streams the firmware has no mapping for fault ("unmatched stream fault",
 * sCR0.USFCFG), so EL2 installs a bypass mapping for each of them before the OS's SMMU driver
 * probes.  The driver keeps every valid mapping it finds as a boot mapping, bypass and
 * untouched, which is what the processor needs.
 *
 * Which SMMU comes from the DT: the node carries `hypervisor-smmu`.  The masters EL2 drives
 * itself are the `iommus` of the node tagged `hypervisor-takeover`; the processors EL2 boots
 * are the nodes tagged `hypervisor-pas`.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <dm/ofnode.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <asm/io.h>
#include <errno.h>
#include <linux/sizes.h>
#include <string.h>

#include <hyp/spinlock.h>

#include "hv.h"
#if IS_ENABLED(CONFIG_HYP_USB)
#include <hyp/usb.h>
#endif

#define SMMU_GR0_ID0		0x20
#define SMMU_ID0_NUMSMRG	GENMASK(7, 0)
#define SMMU_GR0_SMR(n)		(0x800 + ((n) << 2))
#define SMMU_SMR_VALID		BIT(31)
#define SMMU_SMR_MASK		GENMASK(30, 16)
#define SMMU_SMR_ID		GENMASK(14, 0)
#define SMMU_GR0_S2CR(n)	(0xc00 + ((n) << 2))
#define SMMU_S2CR_TYPE		GENMASK(17, 16)
#define SMMU_S2CR_CBNDX		GENMASK(7, 0)
#define SMMU_S2CR_TRANS		0
#define SMMU_S2CR_BYPASS	1
#define SMMU_S2CR_FAULT		2

#define SMMU_GR0_ID1		0x24
#define SMMU_ID1_PAGESIZE	BIT(31)
#define SMMU_ID1_NUMPAGENDXB	GENMASK(30, 28)
#define SMMU_ID1_NUMCB		GENMASK(7, 0)
#define SMMU_GR1_CBAR(n)	(4 * (n))
#define SMMU_CBAR_S1_BYPASS	(1 << 16)	/* stage 1 translates, stage 2 is bypass */
#define SMMU_GR1_CBA2R(n)	(0x800 + 4 * (n))
#define SMMU_CBA2R_VA64		BIT(0)
#define SMMU_CB_SCTLR		0x00
#define SMMU_CB_RESUME		0x08
#define SMMU_CB_TCR2		0x10
#define SMMU_CB_TTBR0		0x20
#define SMMU_CB_TCR		0x30
#define SMMU_CB_MAIR0		0x38
#define SMMU_CB_FSR		0x58
#define SMMU_CB_FAR		0x60
#define SMMU_CB_FSYNR0		0x68
#define SMMU_CB_TLBIALL		0x618
#define SMMU_CB_TLBSYNC		0x7f0
#define SMMU_CB_TLBSTATUS	0x7f4
#define SMMU_SCTLR_M		BIT(0)
#define SMMU_SCTLR_TRE		BIT(1)
#define SMMU_SCTLR_AFE		BIT(2)
#define SMMU_SCTLR_CFCFG	BIT(7)		/* stall on a context fault */
#define SMMU_SCTLR_HUPCF	BIT(8)		/* keep translating while one is stalled */
#define SMMU_FSR_STALLED	BIT(30)
#define SMMU_FSYNR0_WNR		BIT(4)

/* Masters EL2 drives itself, whose stream mappings stay: (SID, mask) of their `iommus` */
#define SMMU_MAX_KEEP		4
/* Streams of the processors EL2 boots, mapped as bypass for the OS to keep */
#define SMMU_MAX_BYPASS		8

struct hyp_smmu {
	u64 base;
	u32 nsme;
	u32 nkeep;
	u32 keep_sid[SMMU_MAX_KEEP];
	u32 keep_mask[SMMU_MAX_KEEP];
	u32 nbypass;
	u32 bypass_sid[SMMU_MAX_BYPASS];
	u32 bypass_mask[SMMU_MAX_BYPASS];
	u32 bypass_vmid;	/* the VMID the bypass streams carry (S2CR bits 7:0, BPVMID) */
	u32 ncb;		/* context banks; the first is page cb_pg, a page is 1 << pgshift bytes */
	u32 pgshift;
	u32 cb_pg;
};

static __hyp_data struct hyp_smmu smmu;

/* Call @fn for every enabled node, at any depth, that carries @prop */
static void smmu_each_tagged(ofnode parent, const char *prop, void (*fn)(ofnode node))
{
	ofnode node;

	ofnode_for_each_subnode(node, parent) {
		if (ofnode_is_enabled(node) && ofnode_read_bool(node, prop))
			fn(node);

		smmu_each_tagged(node, prop, fn);
	}
}

/* The first enabled node, at any depth, carrying @prop */
static ofnode smmu_find_tagged(ofnode parent, const char *prop)
{
	ofnode node;

	ofnode_for_each_subnode(node, parent) {
		ofnode found;

		if (ofnode_is_enabled(node) && ofnode_read_bool(node, prop))
			return node;

		found = smmu_find_tagged(node, prop);
		if (ofnode_valid(found))
			return found;
	}

	return ofnode_null();
}

static u32 smmu_ph, smmu_cells;

/*
 * Append the (SID, mask) pairs @master lists for this SMMU to @sid and @mask
 * (@count of @max used so far)
 */
static void smmu_masters(ofnode master, u32 *sid, u32 *mask, u32 *count, u32 max)
{
	const __be32 *p;
	int len, n;

	p = ofnode_read_prop(master, "iommus", &len);
	if (!p)
		return;

	for (n = 0; (n + 1 + smmu_cells) * 4 <= len && *count < max; n += 1 + smmu_cells) {
		if (be32_to_cpu(p[n]) != smmu_ph)
			continue;
		sid[*count] = be32_to_cpu(p[n + 1]);
		mask[*count] = smmu_cells > 1 ? be32_to_cpu(p[n + 2]) : 0;
		(*count)++;
	}
}

static void smmu_pas_master(ofnode node)
{
	/*
	 * A processor with a resource table (hypervisor-pas-rsctable, answered to PAS_GET_RSCTABLE)
	 * gets its SMMU context from the OS: its streams are the OS's to map and the OS's SMMU driver
	 * writes the translation table base.  The bypass mapping is only for the ones without.
	 */
	if (ofnode_read_prop(node, "hypervisor-pas-rsctable", NULL))
		return;

	smmu_masters(node, smmu.bypass_sid, smmu.bypass_mask, &smmu.nbypass, SMMU_MAX_BYPASS);
}

void __hyp hyp_smmu_init(void)
{
	ofnode node = smmu_find_tagged(ofnode_root(), "hypervisor-smmu");
	phys_addr_t size;

	if (!ofnode_valid(node)) {
		hprintf("smmu: no SMMU tagged hypervisor-smmu, stream mappings are left as the guest made them\n");
		return;
	}

	smmu.base = ofnode_get_addr_size_index_notrans(node, 0, &size);
	if (smmu.base == FDT_ADDR_T_NONE) {
		smmu.base = 0;
		hprintf("smmu: %s has no register window\n", ofnode_get_name(node));
		return;
	}

	smmu.nsme = FIELD_GET(SMMU_ID0_NUMSMRG, readl(smmu.base + SMMU_GR0_ID0));
	smmu.bypass_vmid = 0xff;
	{
		u32 id1 = readl(smmu.base + SMMU_GR0_ID1);

		smmu.ncb = FIELD_GET(SMMU_ID1_NUMCB, id1);
		smmu.pgshift = id1 & SMMU_ID1_PAGESIZE ? 16 : 12;
		smmu.cb_pg = 1 << (FIELD_GET(SMMU_ID1_NUMPAGENDXB, id1) + 1);
	}
	smmu_cells = ofnode_read_u32_default(node, "#iommu-cells", 1);
	smmu_ph = 0;
	ofnode_read_u32(node, "phandle", &smmu_ph);

#if IS_ENABLED(CONFIG_HYP_USB)
	smmu_masters(hyp_usb_node(), smmu.keep_sid, smmu.keep_mask, &smmu.nkeep, SMMU_MAX_KEEP);
#endif
	smmu_masters(ofnode_path("/soc@0/display-subsystem@ae00000"), smmu.keep_sid, smmu.keep_mask, &smmu.nkeep, SMMU_MAX_KEEP);
	smmu_each_tagged(ofnode_root(), "hypervisor-pas", smmu_pas_master);
	hyp_clean_range(&smmu, sizeof(smmu));

	hprintf("smmu: %s at %#llx, %u stream mapping groups, %u master(s) of EL2's stay mapped, %u bypass stream(s) for the processors EL2 boots\n",
		ofnode_get_name(node), smmu.base, smmu.nsme, smmu.nkeep, smmu.nbypass);
}

static bool __hyp smmu_keeps(u32 sid, u32 mask)
{
	for (u32 i = 0; i < smmu.nkeep; i++)
		if (smmu.keep_sid[i] == sid && smmu.keep_mask[i] == mask)
			return true;

	return false;
}

/* The highest stream mapping group nothing uses, or -1 */
static int __hyp smmu_free_sme(void)
{
	for (int i = (int)smmu.nsme - 1; i >= 0; i--)
		if (!(readl(smmu.base + SMMU_GR0_SMR(i)) & SMMU_SMR_VALID))
			return i;

	return -1;
}

/* Is a mapping of this very (SID, mask) already there? */
static bool __hyp smmu_mapped(u32 sid, u32 mask)
{
	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));

		if ((smr & SMMU_SMR_VALID) && FIELD_GET(SMMU_SMR_ID, smr) == sid &&
		    FIELD_GET(SMMU_SMR_MASK, smr) == mask)
			return true;
	}

	return false;
}

/**
 * hyp_smmu_handoff() - drop the stream mappings U-Boot made and map the processors' streams
 *
 * Return: how many stream mappings of U-Boot's were removed
 */
int __hyp hyp_smmu_handoff(void)
{
	int dropped = 0;

	if (!smmu.base)
		return 0;

	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));
		u32 s2cr = readl(smmu.base + SMMU_GR0_S2CR(i));

		/* U-Boot's mappings point at a context bank; the secure world's are bypass */
		if (!(smr & SMMU_SMR_VALID) || FIELD_GET(SMMU_S2CR_TYPE, s2cr) != SMMU_S2CR_TRANS)
			continue;
		if (smmu_keeps(FIELD_GET(SMMU_SMR_ID, smr), FIELD_GET(SMMU_SMR_MASK, smr)))
			continue;

		writel(FIELD_PREP(SMMU_S2CR_TYPE, SMMU_S2CR_FAULT), smmu.base + SMMU_GR0_S2CR(i));
		writel(0, smmu.base + SMMU_GR0_SMR(i));
		hprintf("smmu: dropped stream mapping %u (SID %#lx mask %#lx, context bank %u)\n", i,
			(unsigned long)FIELD_GET(SMMU_SMR_ID, smr),
			(unsigned long)FIELD_GET(SMMU_SMR_MASK, smr), s2cr & 0xff);
		dropped++;
	}

	for (u32 i = 0; i < smmu.nbypass; i++) {
		u32 sid = smmu.bypass_sid[i], mask = smmu.bypass_mask[i];
		int idx;

		if (smmu_mapped(sid, mask))
			continue;

		idx = smmu_free_sme();
		if (idx < 0) {
			hprintf("smmu: no free stream mapping group for SID %#x\n", sid);
			break;
		}

		/* S2CR first: the stream matches as soon as the SMR is valid */
		writel(FIELD_PREP(SMMU_S2CR_TYPE, SMMU_S2CR_BYPASS) | FIELD_PREP(SMMU_S2CR_CBNDX, smmu.bypass_vmid),
		       smmu.base + SMMU_GR0_S2CR(idx));
		writel(SMMU_SMR_VALID | FIELD_PREP(SMMU_SMR_MASK, mask) | FIELD_PREP(SMMU_SMR_ID, sid),
		       smmu.base + SMMU_GR0_SMR(idx));
		hprintf("smmu: bypass stream mapping %d for SID %#x mask %#x\n", idx, sid, mask);
	}
	asm volatile("dsb sy" ::: "memory");

	return dropped;
}

/**
 * hyp_smmu_describe() - the SMMU's global fault state, for the console
 * @buf: filled
 * @len: its size
 *
 * sGFSR is the OS driver's to clear; this is what it last found, and GFSYNR1 names the stream
 * (the SID) of the last global fault, an unmatched stream among them.
 */
void __hyp hyp_smmu_describe(char *buf, size_t len)
{
	if (!smmu.base) {
		snprintf(buf, len, "smmu: none");
		return;
	}

	snprintf(buf, len, "smmu: sGFSR %#x sGFSYNR0 %#x sGFSYNR1 %#x (SID %#x) sGFSYNR2 %#x sCR0 %#x",
		 readl(smmu.base + 0x48), readl(smmu.base + 0x50), readl(smmu.base + 0x54),
		 readl(smmu.base + 0x54) & 0x7fff, readl(smmu.base + 0x58), readl(smmu.base));
}

/**
 * hyp_smmu_toggle_fault() - make the processors' streams fault instead of bypass, or back
 *
 * A diagnostic (`pas smmu` on the console): with the streams faulting, every access the core makes
 * through them raises an SMMU global fault, which the OS's SMMU driver logs with the SID in
 * GFSYNR1 - the list of streams the firmware really uses.
 *
 * Return: true when the streams now fault
 */
bool __hyp hyp_smmu_toggle_fault(void)
{
	static __hyp_data bool fault;

	if (!smmu.base)
		return false;
	fault = !fault;

	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));

		for (u32 b = 0; (smr & SMMU_SMR_VALID) && b < smmu.nbypass; b++)
			if (FIELD_GET(SMMU_SMR_ID, smr) == smmu.bypass_sid[b] &&
			    FIELD_GET(SMMU_SMR_MASK, smr) == smmu.bypass_mask[b])
				writel(FIELD_PREP(SMMU_S2CR_TYPE, fault ? SMMU_S2CR_FAULT : SMMU_S2CR_BYPASS) |
				       FIELD_PREP(SMMU_S2CR_CBNDX, smmu.bypass_vmid), smmu.base + SMMU_GR0_S2CR(i));
	}
	asm volatile("dsb sy" ::: "memory");

	return fault;
}

/**
 * hyp_smmu_set_vmid() - change the VMID the processors' bypass streams carry
 * @vmid: S2CR bits 7:0 of a bypass mapping (BPVMID), which the memory protection units see
 *
 * A diagnostic (`pas vmid N`).  The mappings were made with 0xff, a value that means "no context
 * bank" for a translating mapping and is no VMID anyone is allowed to use.
 */
void __hyp hyp_smmu_set_vmid(u32 vmid)
{
	smmu.bypass_vmid = vmid & 0xff;
	hyp_clean_range(&smmu.bypass_vmid, sizeof(smmu.bypass_vmid));
	if (!smmu.base)
		return;

	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));

		for (u32 b = 0; (smr & SMMU_SMR_VALID) && b < smmu.nbypass; b++)
			if (FIELD_GET(SMMU_SMR_ID, smr) == smmu.bypass_sid[b] &&
			    FIELD_GET(SMMU_SMR_MASK, smr) == smmu.bypass_mask[b])
				writel(FIELD_PREP(SMMU_S2CR_TYPE, SMMU_S2CR_BYPASS) |
				       FIELD_PREP(SMMU_S2CR_CBNDX, smmu.bypass_vmid), smmu.base + SMMU_GR0_S2CR(i));
	}
	asm volatile("dsb sy" ::: "memory");
}

/*
 * A translation context of EL2's own for the processors' streams (`pas ctx`).
 *
 * The processors' streams are bypass: whatever a core touches goes through, and nothing says what
 * that is.  For a diagnostic window EL2 points them at a stage-1 context bank instead, one that
 * maps the processor's carveout 1:1 and nothing else, and that stalls a transaction it cannot
 * translate instead of aborting it (SCTLR.CFCFG: an abort is a NoC error, which the secure world
 * answers with a board reset).  Each stall is a first touch of a 2 MiB block the core had not
 * been given: EL2 records its address, maps the block 1:1 and lets the transaction go again.  The
 * list of blocks, in order, says which memory the firmware uses and where it was when it stopped
 * asking for new ones.
 *
 * The bank is the highest one below 60 that nothing uses, programmed the way the OS's own banks
 * are on this SMMU (36-bit input, 36-bit output, the same walk attributes; the first attempt with
 * a 39-bit input and a 48-bit output size faulted at the first level for every access).  The OS's
 * SMMU driver has already probed and takes no stream it does not know of; it would take this bank
 * if it ran out of lower ones.
 *
 * What the first runs showed (scripts/ADSP.md): the firmware reaches the shared-memory table
 * (SMEM) about 130 ms after the start.  Once several accesses are stalled at the same time some
 * come back as non-stalled faults (FSR 0x400), which terminate the access instead of waiting for
 * EL2, so the list is complete only up to that point.
 */
#define SMMU_CTX_L2		8		/* 1 GiB regions the context can map */
#define SMMU_CTX_LOG		192
#define SMMU_CTX_BLOCK		SZ_2M
#define SMMU_DRAM_BASE		0x80000000ULL	/* below it are registers: device memory */

struct smmu_ctx {
	u32 cb;
	bool on;
	u64 *l1;
	u64 *l2[SMMU_CTX_L2];
	u32 l2_gb[SMMU_CTX_L2];		/* which 1 GiB each second-level table covers */
	u32 nl2;
	u32 nlog;
	u32 nfault, nterm;
	u64 last_addr;
	bool last_write;
	u32 last_fsr, last_fsynr;
	struct {
		u64 addr;
		u32 us;
		bool write;
		u32 fsr;
	} log[SMMU_CTX_LOG];
	u32 s2cr[SMMU_MAX_BYPASS * 2];	/* the stream mappings switched to the context: index, old S2CR */
	u32 ns2cr;
};

static __hyp_data struct smmu_ctx ctx;

/*
 * The translation tables: the top level (64 entries, a 36-bit input as the OS's banks have) and
 * the second-level tables.  They live in the resident part of the hypervisor image, which the OS
 * never maps.  Not in the processor's carveout: the firmware clears and uses its end, and a table
 * it overwrote answers every access with a translation fault at the second level.
 */
static __hyp_data u64 smmu_pt[1 + SMMU_CTX_L2][512] __aligned(SZ_4K);

static u64 __hyp smmu_page(u32 page)
{
	return smmu.base + ((u64)page << smmu.pgshift);
}

static u64 __hyp smmu_cb(u32 cb)
{
	return smmu_page(smmu.cb_pg + cb);
}

/* The second-level table for the 1 GiB region of @addr, allocated on first use */
static u64 *__hyp smmu_ctx_l2(u64 addr)
{
	u32 gb = addr >> 30;

	for (u32 i = 0; i < ctx.nl2; i++)
		if (ctx.l2_gb[i] == gb)
			return ctx.l2[i];

	if (ctx.nl2 == SMMU_CTX_L2)
		return NULL;

	if (!ctx.l2[ctx.nl2])
		ctx.l2[ctx.nl2] = smmu_pt[1 + ctx.nl2];
	memset(ctx.l2[ctx.nl2], 0, SZ_4K);
	hyp_clean_range(ctx.l2[ctx.nl2], SZ_4K);
	ctx.l2_gb[ctx.nl2] = gb;
	ctx.l1[gb] = (u64)ctx.l2[ctx.nl2] | 3;
	hyp_clean_range(&ctx.l1[gb], sizeof(u64));

	return ctx.l2[ctx.nl2++];
}

/* Map the 2 MiB block holding @addr 1:1, read-write; false when it cannot be */
static bool __hyp smmu_ctx_map(u64 addr)
{
	u64 *l2;
	u64 pte;

	if (addr >= 1ULL << 36)
		return false;

	l2 = smmu_ctx_l2(addr);
	if (!l2)
		return false;

	/* a block descriptor: attribute 0 (normal, write-back) or 1 (device), non-secure,
	 * read-write for any privilege, inner shareable (outer for device), access flag */
	pte = (addr & ~(SMMU_CTX_BLOCK - 1)) | 1 | BIT(5) | (1 << 6) | BIT(10);
	pte |= 3ULL << 8;
	l2[(addr >> 21) & 511] = pte;
	hyp_clean_range(&l2[(addr >> 21) & 511], sizeof(u64));
	asm volatile("dsb sy" ::: "memory");

	return true;
}

static void __hyp smmu_ctx_tlb_flush(void)
{
	writel(0, smmu_cb(ctx.cb) + SMMU_CB_TLBIALL);
	writel(0, smmu_cb(ctx.cb) + SMMU_CB_TLBSYNC);
	for (u32 i = 0; i < 1000 && (readl(smmu_cb(ctx.cb) + SMMU_CB_TLBSTATUS) & 1); i++)
		hyp_udelay(1);
}

/**
 * hyp_smmu_ctx_enable() - send the processors' streams through the context
 * @mem: the carveout's base, mapped to begin with
 * @size: its size
 *
 * Return: 0, or a negative errno
 */
int __hyp hyp_smmu_ctx_enable(u64 mem, u64 size)
{
	int cb = -1;

	if (!smmu.base || !smmu.nbypass)
		return -ENODEV;

	for (int n = min_t(int, (int)smmu.ncb - 1, 60); n >= 0 && cb < 0; n--) {
		bool used = readl(smmu_page(1) + SMMU_GR1_CBAR(n)) || readl(smmu_cb(n) + SMMU_CB_SCTLR);

		for (u32 i = 0; i < smmu.nsme && !used; i++) {
			u32 s2cr = readl(smmu.base + SMMU_GR0_S2CR(i));

			used = FIELD_GET(SMMU_S2CR_TYPE, s2cr) == SMMU_S2CR_TRANS && (s2cr & 0xff) == (u32)n;
		}
		if (!used)
			cb = n;
	}
	if (cb < 0)
		return -EBUSY;

	ctx.cb = cb;
	ctx.nl2 = 0;
	ctx.nlog = 0;
	ctx.nfault = 0;
	ctx.nterm = 0;
	ctx.l1 = smmu_pt[0];
	memset(ctx.l1, 0, SZ_4K);
	hyp_clean_range(ctx.l1, SZ_4K);
	for (u64 a = mem & ~(SMMU_CTX_BLOCK - 1); a < mem + size; a += SMMU_CTX_BLOCK)
		smmu_ctx_map(a);

	/* the bank: translation table, 39-bit input, 4 KiB granule, write-back table walks */
	writel(0, smmu_cb(cb) + SMMU_CB_SCTLR);
	/* the OS's own banks on this SMMU: 36-bit input from the second level (T0SZ 28), 36-bit output,
	 * the same table-walk attributes, the bank number as the ASID */
	writel(SMMU_CBA2R_VA64, smmu_page(1) + SMMU_GR1_CBA2R(cb));
	writel(SMMU_CBAR_S1_BYPASS | 0xf300 | (smmu.bypass_vmid == 0xff ? 0 : smmu.bypass_vmid),
	       smmu_page(1) + SMMU_GR1_CBAR(cb));
	writeq((u64)smmu_pt[0] | ((u64)cb << 48), smmu_cb(cb) + SMMU_CB_TTBR0);
	writel(0x80351c, smmu_cb(cb) + SMMU_CB_TCR);
	writel(0x38071, smmu_cb(cb) + SMMU_CB_TCR2);
	writel(0x04ff, smmu_cb(cb) + SMMU_CB_MAIR0);	/* attribute 0: normal write-back; 1: device */
	smmu_ctx_tlb_flush();
	writel(SMMU_SCTLR_M | SMMU_SCTLR_TRE | SMMU_SCTLR_AFE | SMMU_SCTLR_CFCFG, smmu_cb(cb) + SMMU_CB_SCTLR);
	asm volatile("dsb sy" ::: "memory");

	/* the streams: S2CR first, the SMR already matches */
	ctx.ns2cr = 0;
	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));

		for (u32 b = 0; (smr & SMMU_SMR_VALID) && b < smmu.nbypass && ctx.ns2cr < SMMU_MAX_BYPASS; b++) {
			if (FIELD_GET(SMMU_SMR_ID, smr) != smmu.bypass_sid[b] ||
			    FIELD_GET(SMMU_SMR_MASK, smr) != smmu.bypass_mask[b])
				continue;
			ctx.s2cr[2 * ctx.ns2cr] = i;
			ctx.s2cr[2 * ctx.ns2cr + 1] = readl(smmu.base + SMMU_GR0_S2CR(i));
			ctx.ns2cr++;
			writel(FIELD_PREP(SMMU_S2CR_TYPE, SMMU_S2CR_TRANS) | FIELD_PREP(SMMU_S2CR_CBNDX, cb),
			       smmu.base + SMMU_GR0_S2CR(i));
		}
	}
	asm volatile("dsb sy" ::: "memory");
	ctx.on = true;

	hprintf("smmu: context bank %d translates %u stream(s), %#llx + %#llx mapped\n", cb, ctx.ns2cr, mem, size);

	return 0;
}

/* Serve the stalled transactions for @ms milliseconds: map the block each one wanted and retry it */
void __hyp hyp_smmu_ctx_service(u32 ms)
{
	u64 t0 = hyp_ticks();

	if (!ctx.on)
		return;

	do {
		u32 fsr = readl(smmu_cb(ctx.cb) + SMMU_CB_FSR);
		u64 addr;
		bool write, mapped, fresh = false;

		if (!fsr)
			continue;

		addr = readq(smmu_cb(ctx.cb) + SMMU_CB_FAR);
		write = readl(smmu_cb(ctx.cb) + SMMU_CB_FSYNR0) & SMMU_FSYNR0_WNR;
		mapped = smmu_ctx_map(addr);

		if (ctx.nlog < SMMU_CTX_LOG && (!ctx.nlog || (ctx.log[ctx.nlog - 1].addr ^ addr) >> 21)) {
			fresh = true;
			ctx.log[ctx.nlog].addr = addr;
			ctx.log[ctx.nlog].write = write;
			ctx.log[ctx.nlog].fsr = fsr;
			ctx.log[ctx.nlog++].us = hyp_ticks_us();
		}
		ctx.nfault++;
		ctx.last_addr = addr;
		ctx.last_write = write;
		ctx.last_fsr = fsr;
		ctx.last_fsynr = readl(smmu_cb(ctx.cb) + SMMU_CB_FSYNR0);
		if (!mapped)
			ctx.nterm++;
		smmu_ctx_tlb_flush();
		writel(fsr, smmu_cb(ctx.cb) + SMMU_CB_FSR);
		if (fsr & SMMU_FSR_STALLED)
			writel(mapped ? 0 : 1, smmu_cb(ctx.cb) + SMMU_CB_RESUME);	/* 0: retry, 1: terminate */
		/* printed once the access is on its way: the processor waits for EL2 while it is stalled */
		if (fresh)
			hprintf("smmu: first touch %#llx %s\n", addr, write ? "write" : "read");
	} while (!hyp_ticks_elapsed(t0, ms * 1000));
}

/* Put the streams back as they were and print what the processor touched */
void __hyp hyp_smmu_ctx_disable(void)
{
	if (!ctx.on)
		return;

	for (u32 i = 0; i < ctx.ns2cr; i++)
		writel(ctx.s2cr[2 * i + 1], smmu.base + SMMU_GR0_S2CR(ctx.s2cr[2 * i]));
	asm volatile("dsb sy" ::: "memory");

	/* let anything still stalled go on through the bypass the streams have again */
	writel(readl(smmu_cb(ctx.cb) + SMMU_CB_FSR), smmu_cb(ctx.cb) + SMMU_CB_FSR);
	writel(0, smmu_cb(ctx.cb) + SMMU_CB_RESUME);
	writel(0, smmu_cb(ctx.cb) + SMMU_CB_SCTLR);
	writel(0, smmu_page(1) + SMMU_GR1_CBAR(ctx.cb));
	writel(0, smmu_page(1) + SMMU_GR1_CBA2R(ctx.cb));
	ctx.on = false;

	hprintf("smmu: the context saw %u fault(s), %u terminated; the last: %#llx %s FSR %#x FSYNR0 %#x\n",
		ctx.nfault, ctx.nterm, ctx.last_addr, ctx.last_write ? "write" : "read", ctx.last_fsr, ctx.last_fsynr);
	hprintf("smmu: first touches, 2 MiB blocks in order:\n");
	for (u32 i = 0; i < ctx.nlog; i++)
		hprintf("smmu:   +%u us %#llx %s FSR %#x\n", ctx.log[i].us - ctx.log[0].us, ctx.log[i].addr,
			ctx.log[i].write ? "write" : "read", ctx.log[i].fsr);
}

/*
 * What the OS's SMMU driver set up for the processor's streams, and every context fault for the next
 * @ms milliseconds, printed from EL2 (`pas watch N`).  When the OS owns the SMMU a hung board prints
 * nothing of its own: the stream mappings (a SID's S2CR type, and the context bank it points to with
 * its SCTLR and translation table base) say whether the OS got that far, and a faulting access shows
 * up as a non-zero FSR with the address in FAR.
 */
void __hyp hyp_smmu_watch(u32 ms, u32 sid_lo, u32 sid_hi)
{
	u64 t0 = hyp_ticks();
	u32 gfsr_seen = 0;
	u32 fsr_seen[256] = { 0 };
	u32 used[8] = { 0 };

	if (!smmu.base)
		return;

	for (u32 i = 0; i < smmu.nsme; i++) {
		u32 smr = readl(smmu.base + SMMU_GR0_SMR(i));
		u32 s2cr = readl(smmu.base + SMMU_GR0_S2CR(i));
		u32 id = FIELD_GET(SMMU_SMR_ID, smr);

		if ((smr & SMMU_SMR_VALID) && id >= sid_lo && id <= sid_hi) {
			u32 cb = s2cr & 0xff;

			hprintf("smmu: SMR %u SID %#x mask %#lx S2CR %#x (type %lu, context bank/VMID %u)\n", i, id,
				(unsigned long)FIELD_GET(SMMU_SMR_MASK, smr), s2cr,
				(unsigned long)FIELD_GET(SMMU_S2CR_TYPE, s2cr), cb);
			if (FIELD_GET(SMMU_S2CR_TYPE, s2cr) == SMMU_S2CR_TRANS && cb < smmu.ncb && cb < 256)
				used[cb / 32] |= BIT(cb % 32);
			if (FIELD_GET(SMMU_S2CR_TYPE, s2cr) == SMMU_S2CR_TRANS && cb < smmu.ncb)
				hprintf("smmu:   bank %u SCTLR %#x TTBR0 %#llx TCR %#x CBAR %#x\n", cb,
					readl(smmu_cb(cb) + SMMU_CB_SCTLR), readq(smmu_cb(cb) + SMMU_CB_TTBR0),
					readl(smmu_cb(cb) + SMMU_CB_TCR), readl(smmu_page(1) + SMMU_GR1_CBAR(cb)));
		}
	}

	do {
		u32 gfsr = readl(smmu.base + 0x48);

		if (gfsr && gfsr != gfsr_seen) {
			gfsr_seen = gfsr;
			hprintf("smmu: +%llu us global fault sGFSR %#x GFAR %#llx SID %#x\n", hyp_ticks_us(), gfsr,
				readq(smmu.base + 0x40), readl(smmu.base + 0x54) & 0x7fff);
		}
		for (u32 cb = 0; cb < smmu.ncb && cb < 256; cb++) {
			u32 fsr;

			if (!(used[cb / 32] & BIT(cb % 32)))
				continue;
			fsr = readl(smmu_cb(cb) + SMMU_CB_FSR);

			if (fsr && fsr != fsr_seen[cb]) {
				fsr_seen[cb] = fsr;
				hprintf("smmu: +%llu us bank %u fault FSR %#x FAR %#llx FSYNR0 %#x\n", hyp_ticks_us(),
					cb, fsr, readq(smmu_cb(cb) + SMMU_CB_FAR), readl(smmu_cb(cb) + SMMU_CB_FSYNR0));
			}
		}
	} while (!hyp_ticks_elapsed(t0, ms * 1000));
}
