// SPDX-License-Identifier: GPL-2.0+
/*
 * The hypervisor's own RPMH voter.
 *
 * The apps RSC has one DRV (direct resource voter) per software domain: DRV-0
 * is the secure world's, DRV-2 is what the OS drives, and DRV-1 is the
 * hypervisor's.  RPMh aggregates every DRV's votes per resource (enables are
 * OR-ed, voltages and corners take the highest), so a vote EL2 sends on its own
 * DRV cannot be taken back by the guest, whatever the guest's driver does with
 * its DRV: that is what keeps the supplies EL2 needs on, and it needs no TCS
 * sharing with the guest at all.
 *
 * Which DRV is ours is a board decision: the RSC node carries
 * `hypervisor-drv-id`.  The register layout and TCS geometry follow the RSC's
 * own version and configuration registers, the same way
 * drivers/soc/qcom/rpmh-rsc.c reads them for the guest's DRV.  Votes are
 * active-only: one TCS, one command per transaction, polled to completion (the
 * completion interrupt of a DRV that is not the OS's is not delivered to the
 * non-secure side).
 *
 * Resource addresses come from the command DB, resolved once at boot into
 * __hyp_data because the resident copy's ordinary data is not safe once the
 * guest runs.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <soc/qcom/cmd-db.h>
#include <asm/io.h>
#include <stdio.h>
#include <string.h>
/* After <soc/qcom/cmd-db.h>: it pulls in linux/compat.h, whose no-op spinlocks
 * this header has to override (see its comment). */
#include <hyp/spinlock.h>

#include "hv.h"

#define RSC_DRV_ID		0
#define RSC_VER_MAJOR(id)	(((id) >> 16) & 0xff)

/* DRV_PRNT_CHLD_CONFIG: six bits of TCS count per DRV */
#define DRV_NUM_TCS_MASK	0x3f
#define DRV_NUM_TCS_SHIFT	6

#define TCS_AMC_MODE_ENABLE	BIT(16)
#define TCS_AMC_MODE_TRIGGER	BIT(24)

#define CMD_MSGID_LEN		8
#define CMD_MSGID_RESP_REQ	BIT(8)
#define CMD_MSGID_WRITE		BIT(16)
#define CMD_STATUS_COMPL	BIT(16)

#define RPMH_WAIT_US		100000

enum {
	R_TCS_OFFSET,
	R_CMD_OFFSET,
	R_PRNT_CHLD_CONFIG,
	R_IRQ_ENABLE,
	R_IRQ_STATUS,
	R_IRQ_CLEAR,
	R_WAIT_FOR_CMPL,
	R_CONTROL,
	R_STATUS,		/* zero while the TCS is busy */
	R_CMD_ENABLE,
	R_CMD_MSGID,
	R_CMD_ADDR,
	R_CMD_DATA,
	R_CMD_STATUS,
	R_CMD_RESP_DATA,
	R_NR,
};

/* Same values as rpmh_rsc_reg_offset_ver_2_7 / ver_3_0 in rpmh-rsc.c */
static const u32 __hyp_rodata regs_v2_7[R_NR] = {
	[R_TCS_OFFSET] = 672, [R_CMD_OFFSET] = 20, [R_PRNT_CHLD_CONFIG] = 0x0c,
	[R_IRQ_ENABLE] = 0x00, [R_IRQ_STATUS] = 0x04, [R_IRQ_CLEAR] = 0x08,
	[R_WAIT_FOR_CMPL] = 0x10, [R_CONTROL] = 0x14, [R_STATUS] = 0x18,
	[R_CMD_ENABLE] = 0x1c, [R_CMD_MSGID] = 0x30, [R_CMD_ADDR] = 0x34,
	[R_CMD_DATA] = 0x38, [R_CMD_STATUS] = 0x3c, [R_CMD_RESP_DATA] = 0x40,
};

static const u32 __hyp_rodata regs_v3_0[R_NR] = {
	[R_TCS_OFFSET] = 672, [R_CMD_OFFSET] = 24, [R_PRNT_CHLD_CONFIG] = 0x0c,
	[R_IRQ_ENABLE] = 0x00, [R_IRQ_STATUS] = 0x04, [R_IRQ_CLEAR] = 0x08,
	[R_WAIT_FOR_CMPL] = 0x20, [R_CONTROL] = 0x24, [R_STATUS] = 0x28,
	[R_CMD_ENABLE] = 0x2c, [R_CMD_MSGID] = 0x34, [R_CMD_ADDR] = 0x38,
	[R_CMD_DATA] = 0x3c, [R_CMD_STATUS] = 0x40, [R_CMD_RESP_DATA] = 0x44,
};

struct hyp_rpmh {
	bool ready;
	u32 drv_id;
	u64 tcs_base;			/* DRV window + qcom,tcs-offset */
	u32 regs[R_NR];
	u32 tcs;			/* the TCS EL2 drives */
	struct spinlock lock;
};

static __hyp_data struct hyp_rpmh rpmh = { .lock = __SPINLOCK_UNLOCKED };

/* The apps RSC node: the `qcom,rpmh-rsc` the board tags `hypervisor` */
static ofnode rpmh_find_rsc(void)
{
	ofnode node = ofnode_null();

	while (ofnode_valid(node = ofnode_by_compatible(node, "qcom,rpmh-rsc")))
		if (ofnode_read_bool(node, "hypervisor"))
			return node;

	return ofnode_null();
}

/* EL2's own DRV: the registers, the TCS and the command DB its votes need */
static void rpmh_voter_init(ofnode rsc)
{
	char name[10];
	phys_addr_t size;
	fdt_addr_t base;
	u32 off, id, config, ntcs;
	int idx;

	if (ofnode_read_u32(rsc, "hypervisor-drv-id", &rpmh.drv_id)) {
		hprintf("rpmh: %s has no hypervisor-drv-id, no EL2 votes\n",
			ofnode_get_name(rsc));
		return;
	}
	if (ofnode_read_u32(rsc, "qcom,tcs-offset", &off)) {
		hprintf("rpmh: %s has no qcom,tcs-offset\n", ofnode_get_name(rsc));
		return;
	}

	snprintf(name, sizeof(name), "drv-%u", rpmh.drv_id);
	idx = ofnode_stringlist_search(rsc, "reg-names", name);
	base = idx < 0 ? FDT_ADDR_T_NONE :
		ofnode_get_addr_size_index_notrans(rsc, idx, &size);
	if (base == FDT_ADDR_T_NONE) {
		hprintf("rpmh: %s has no %s window\n", ofnode_get_name(rsc), name);
		return;
	}

	id = readl(base + RSC_DRV_ID);
	memcpy(rpmh.regs, RSC_VER_MAJOR(id) == 3 ? regs_v3_0 : regs_v2_7,
	       sizeof(rpmh.regs));
	rpmh.tcs_base = base + off;

	config = readl(base + rpmh.regs[R_PRNT_CHLD_CONFIG]);
	ntcs = (config >> (DRV_NUM_TCS_SHIFT * rpmh.drv_id)) & DRV_NUM_TCS_MASK;

	/* TCS 0: DRV-1's first, the one the firmware itself drives in AMC mode */
	rpmh.tcs = 0;

	if (cmd_db_init_dt())
		hprintf("rpmh: the command DB is not usable, resource names cannot be resolved\n");

	rpmh.ready = ntcs > 0;

	hprintf("rpmh: %s %s at %#llx, RSC v%u.%u, %u TCS, using TCS %u, tcs_base %#llx\n",
		ofnode_get_name(rsc), name, (u64)base, RSC_VER_MAJOR(id),
		(id >> 8) & 0xff, ntcs, rpmh.tcs, rpmh.tcs_base);
}

void __hyp hyp_rpmh_init(void)
{
	ofnode rsc = rpmh_find_rsc();

	if (!ofnode_valid(rsc)) {
		hprintf("rpmh: no RSC node tagged hypervisor, no EL2 votes\n");
		return;
	}

	rpmh_voter_init(rsc);
	hyp_clean_range(&rpmh, sizeof(rpmh));
}

static inline u64 __hyp tcs_reg(int reg, u32 tcs)
{
	return rpmh.tcs_base + rpmh.regs[R_TCS_OFFSET] * tcs + rpmh.regs[reg];
}

static inline u64 __hyp tcs_cmd(int reg, u32 tcs, u32 cmd)
{
	return tcs_reg(reg, tcs) + rpmh.regs[R_CMD_OFFSET] * cmd;
}

static inline u64 __hyp common_reg(int reg)
{
	return rpmh.tcs_base + rpmh.regs[reg];
}

/* Write, then wait until the register reads back what was written */
static void __hyp tcs_write_sync(int reg, u32 tcs, u32 val)
{
	u64 t0 = hyp_ticks();

	writel(val, tcs_reg(reg, tcs));
	while (readl(tcs_reg(reg, tcs)) != val && !hyp_ticks_elapsed(t0, RPMH_WAIT_US))
		;
}

/*
 * One active-only command on EL2's DRV: program TCS command 0, trigger the TCS
 * in AMC mode, wait for the controller to report completion, optionally return
 * the response, and leave the TCS idle for the next caller.
 */
static int __hyp rpmh_xfer(u32 addr, u32 *data, bool is_read)
{
	u32 tcs = rpmh.tcs, msgid = CMD_MSGID_LEN | CMD_MSGID_RESP_REQ;
	unsigned long flags;
	u32 ctl;
	u64 t0;
	int ret = 0;

	if (!rpmh.ready)
		return -ENODEV;

	spin_lock_irqsave(&rpmh.lock, flags);

	if (!readl(tcs_reg(R_STATUS, tcs))) {
		ret = -EBUSY;
		goto out;
	}

	/* Whatever the firmware left: an old completion must not end the wait */
	writel(BIT(tcs), common_reg(R_IRQ_CLEAR));

	if (!is_read)
		msgid |= CMD_MSGID_WRITE;
	writel(msgid, tcs_cmd(R_CMD_MSGID, tcs, 0));
	writel(addr, tcs_cmd(R_CMD_ADDR, tcs, 0));
	if (!is_read)
		writel(*data, tcs_cmd(R_CMD_DATA, tcs, 0));
	writel(BIT(0), tcs_reg(R_CMD_ENABLE, tcs));
	writel(BIT(0), tcs_reg(R_WAIT_FOR_CMPL, tcs));

	/* Untrigger, enable AMC mode, trigger - the order rpmh-rsc.c uses */
	ctl = readl(tcs_reg(R_CONTROL, tcs)) & ~TCS_AMC_MODE_TRIGGER;
	tcs_write_sync(R_CONTROL, tcs, ctl);
	ctl = TCS_AMC_MODE_ENABLE;
	tcs_write_sync(R_CONTROL, tcs, ctl);
	writel(ctl | TCS_AMC_MODE_TRIGGER, tcs_reg(R_CONTROL, tcs));

	t0 = hyp_ticks();
	while (!(readl(common_reg(R_IRQ_STATUS)) & BIT(tcs)))
		if (hyp_ticks_elapsed(t0, RPMH_WAIT_US)) {
			ret = -ETIMEDOUT;
			break;
		}
	while (!ret && !(readl(tcs_cmd(R_CMD_STATUS, tcs, 0)) & CMD_STATUS_COMPL))
		if (hyp_ticks_elapsed(t0, RPMH_WAIT_US)) {
			ret = -ETIMEDOUT;
			break;
		}

	if (!ret && is_read)
		*data = readl(tcs_cmd(R_CMD_RESP_DATA, tcs, 0));

	/* Idle again: no trigger, no AMC, no enabled command */
	tcs_write_sync(R_CONTROL, tcs, 0);
	writel(0, tcs_reg(R_WAIT_FOR_CMPL, tcs));
	writel(0, tcs_reg(R_CMD_ENABLE, tcs));
	writel(BIT(tcs), common_reg(R_IRQ_CLEAR));

out:
	spin_unlock_irqrestore(&rpmh.lock, flags);

	return ret;
}

/**
 * hyp_rpmh_write() - vote one value on EL2's DRV and wait for the RPMh ack
 * @addr: resource address (command DB address plus register offset)
 * @data: value to vote
 *
 * Return: 0 on success, -ENODEV without a usable DRV, -EBUSY when the TCS is
 * busy, -ETIMEDOUT when the controller never reported completion.
 */
int __hyp hyp_rpmh_write(u32 addr, u32 data)
{
	return rpmh_xfer(addr, &data, false);
}

/**
 * hyp_rpmh_read() - read a resource register through EL2's DRV
 * @addr: resource address
 * @data: filled with the response
 */
int __hyp hyp_rpmh_read(u32 addr, u32 *data)
{
	return rpmh_xfer(addr, data, true);
}

bool __hyp hyp_rpmh_ready(void)
{
	return rpmh.ready;
}
