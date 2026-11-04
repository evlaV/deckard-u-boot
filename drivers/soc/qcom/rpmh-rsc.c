// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s " fmt, KBUILD_MODNAME

#include <dm.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <dm/lists.h>
#include <dm/ofnode.h>
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/byteorder/generic.h>
#include <linux/types.h>
#include <asm/bitops.h>
#include <asm/io.h>

#include <log.h>

#include <soc/qcom/tcs.h>
#include <dt-bindings/soc/qcom,rpmh-rsc.h>

#include "rpmh-internal.h"

#if IS_ENABLED(CONFIG_HYP_SPY_RPMH) || IS_ENABLED(CONFIG_HYP_RPMH_BCM_CAP)
#include <hypervisor.h>
#include <soc/qcom/cmd-db.h>
#include <string.h>
#endif


#define RSC_DRV_ID			0

#define MAJOR_VER_MASK			0xFF
#define MAJOR_VER_SHIFT			16
#define MINOR_VER_MASK			0xFF
#define MINOR_VER_SHIFT			8

enum {
	RSC_DRV_TCS_OFFSET,
	RSC_DRV_CMD_OFFSET,
/* DRV HW Solver Configuration Information Register */
	DRV_SOLVER_CONFIG,
/* DRV TCS Configuration Information Register */
	DRV_PRNT_CHLD_CONFIG,
/* Offsets for common TCS Registers, one bit per TCS */
	RSC_DRV_IRQ_ENABLE,
	RSC_DRV_IRQ_STATUS,
	RSC_DRV_IRQ_CLEAR,	/* w/o; write 1 to clear */
/*
 * Offsets for per TCS Registers.
 *
 * TCSes start at 0x10 from tcs_base and are stored one after another.
 * Multiply tcs_id by RSC_DRV_TCS_OFFSET to find a given TCS and add one
 * of the below to find a register.
 */
	RSC_DRV_CMD_WAIT_FOR_CMPL,	/* 1 bit per command */
	RSC_DRV_CONTROL,
	RSC_DRV_STATUS,	/* zero if tcs is busy */
	RSC_DRV_CMD_ENABLE,	/* 1 bit per command */
/*
 * Offsets for per command in a TCS.
 *
 * Commands (up to 16) start at 0x30 in a TCS; multiply command index
 * by RSC_DRV_CMD_OFFSET and add one of the below to find a register.
 */
	RSC_DRV_CMD_MSGID,
	RSC_DRV_CMD_ADDR,
	RSC_DRV_CMD_DATA,
	RSC_DRV_CMD_STATUS,
	RSC_DRV_CMD_RESP_DATA,
};

/* DRV HW Solver Configuration Information Register */
#define DRV_HW_SOLVER_MASK		1
#define DRV_HW_SOLVER_SHIFT		24

/* DRV TCS Configuration Information Register */
#define DRV_NUM_TCS_MASK		0x3F
#define DRV_NUM_TCS_SHIFT		6
#define DRV_NCPT_MASK			0x1F
#define DRV_NCPT_SHIFT			27

/* Offsets for CONTROL TCS Registers */
#define RSC_DRV_CTL_TCS_DATA_HI		0x38
#define RSC_DRV_CTL_TCS_DATA_HI_MASK	0xFFFFFF
#define RSC_DRV_CTL_TCS_DATA_HI_VALID	BIT(31)
#define RSC_DRV_CTL_TCS_DATA_LO		0x40
#define RSC_DRV_CTL_TCS_DATA_LO_MASK	0xFFFFFFFF
#define RSC_DRV_CTL_TCS_DATA_SIZE	32

#define TCS_AMC_MODE_ENABLE		BIT(16)
#define TCS_AMC_MODE_TRIGGER		BIT(24)

/* TCS CMD register bit mask */
#define CMD_MSGID_LEN			8
#define CMD_MSGID_RESP_REQ		BIT(8)
#define CMD_MSGID_WRITE			BIT(16)
#define CMD_STATUS_ISSUED		BIT(8)
#define CMD_STATUS_COMPL		BIT(16)

/*
 * Here's a high level overview of how all the registers in RPMH work
 * together:
 *
 * - The main rpmh-rsc address is the base of a register space that can
 *   be used to find overall configuration of the hardware
 *   (DRV_PRNT_CHLD_CONFIG). Also found within the rpmh-rsc register
 *   space are all the TCS blocks. The offset of the TCS blocks is
 *   specified in the device tree by "qcom,tcs-offset" and used to
 *   compute tcs_base.
 * - TCS blocks come one after another. Type, count, and order are
 *   specified by the device tree as "qcom,tcs-config".
 * - Each TCS block has some registers, then space for up to 16 commands.
 *   Note that though address space is reserved for 16 commands, fewer
 *   might be present. See ncpt (num cmds per TCS).
 *
 * Here's a picture:
 *
 *  +---------------------------------------------------+
 *  |RSC                                                |
 *  | ctrl                                              |
 *  |                                                   |
 *  | Drvs:                                             |
 *  | +-----------------------------------------------+ |
 *  | |DRV0                                           | |
 *  | | ctrl/config                                   | |
 *  | | IRQ                                           | |
 *  | |                                               | |
 *  | | TCSes:                                        | |
 *  | | +------------------------------------------+  | |
 *  | | |TCS0  |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | | ctrl | 0| 1| 2| 3| 4| 5| .| .| .| .|14|15|  | |
 *  | | |      |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | +------------------------------------------+  | |
 *  | | +------------------------------------------+  | |
 *  | | |TCS1  |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | | ctrl | 0| 1| 2| 3| 4| 5| .| .| .| .|14|15|  | |
 *  | | |      |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | +------------------------------------------+  | |
 *  | | +------------------------------------------+  | |
 *  | | |TCS2  |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | | ctrl | 0| 1| 2| 3| 4| 5| .| .| .| .|14|15|  | |
 *  | | |      |  |  |  |  |  |  |  |  |  |  |  |  |  | |
 *  | | +------------------------------------------+  | |
 *  | |                    ......                     | |
 *  | +-----------------------------------------------+ |
 *  | +-----------------------------------------------+ |
 *  | |DRV1                                           | |
 *  | | (same as DRV0)                                | |
 *  | +-----------------------------------------------+ |
 *  |                      ......                       |
 *  +---------------------------------------------------+
 */

static u32 rpmh_rsc_reg_offset_ver_2_7[] = {
	[RSC_DRV_TCS_OFFSET]		= 672,
	[RSC_DRV_CMD_OFFSET]		= 20,
	[DRV_SOLVER_CONFIG]		= 0x04,
	[DRV_PRNT_CHLD_CONFIG]		= 0x0C,
	[RSC_DRV_IRQ_ENABLE]		= 0x00,
	[RSC_DRV_IRQ_STATUS]		= 0x04,
	[RSC_DRV_IRQ_CLEAR]		= 0x08,
	[RSC_DRV_CMD_WAIT_FOR_CMPL]	= 0x10,
	[RSC_DRV_CONTROL]		= 0x14,
	[RSC_DRV_STATUS]		= 0x18,
	[RSC_DRV_CMD_ENABLE]		= 0x1C,
	[RSC_DRV_CMD_MSGID]		= 0x30,
	[RSC_DRV_CMD_ADDR]		= 0x34,
	[RSC_DRV_CMD_DATA]		= 0x38,
	[RSC_DRV_CMD_STATUS]		= 0x3C,
	[RSC_DRV_CMD_RESP_DATA]		= 0x40,
};

static u32 rpmh_rsc_reg_offset_ver_3_0[] = {
	[RSC_DRV_TCS_OFFSET]		= 672,
	[RSC_DRV_CMD_OFFSET]		= 24,
	[DRV_SOLVER_CONFIG]		= 0x04,
	[DRV_PRNT_CHLD_CONFIG]		= 0x0C,
	[RSC_DRV_IRQ_ENABLE]		= 0x00,
	[RSC_DRV_IRQ_STATUS]		= 0x04,
	[RSC_DRV_IRQ_CLEAR]		= 0x08,
	[RSC_DRV_CMD_WAIT_FOR_CMPL]	= 0x20,
	[RSC_DRV_CONTROL]		= 0x24,
	[RSC_DRV_STATUS]		= 0x28,
	[RSC_DRV_CMD_ENABLE]		= 0x2C,
	[RSC_DRV_CMD_MSGID]		= 0x34,
	[RSC_DRV_CMD_ADDR]		= 0x38,
	[RSC_DRV_CMD_DATA]		= 0x3C,
	[RSC_DRV_CMD_STATUS]		= 0x40,
	[RSC_DRV_CMD_RESP_DATA]		= 0x44,
};

static inline void __iomem *
tcs_reg_addr(const struct rsc_drv *drv, int reg, int tcs_id)
{
	return drv->tcs_base + drv->regs[RSC_DRV_TCS_OFFSET] * tcs_id + reg;
}

static inline void __iomem *
tcs_cmd_addr(const struct rsc_drv *drv, int reg, int tcs_id, int cmd_id)
{
	return tcs_reg_addr(drv, reg, tcs_id) + drv->regs[RSC_DRV_CMD_OFFSET] * cmd_id;
}

static u32 read_tcs_cmd(const struct rsc_drv *drv, int reg, int tcs_id,
			int cmd_id)
{
	return readl_relaxed(tcs_cmd_addr(drv, reg, tcs_id, cmd_id));
}

static u32 read_tcs_reg(const struct rsc_drv *drv, int reg, int tcs_id)
{
	return readl_relaxed(tcs_reg_addr(drv, reg, tcs_id));
}

static void write_tcs_cmd(const struct rsc_drv *drv, int reg, int tcs_id,
			  int cmd_id, u32 data)
{
	writel_relaxed(data, tcs_cmd_addr(drv, reg, tcs_id, cmd_id));
}

static void write_tcs_reg(const struct rsc_drv *drv, int reg, int tcs_id,
			  u32 data)
{
	writel_relaxed(data, tcs_reg_addr(drv, reg, tcs_id));
}

static void write_tcs_reg_sync(const struct rsc_drv *drv, int reg, int tcs_id,
			       u32 data)
{
	int i;

	writel(data, tcs_reg_addr(drv, reg, tcs_id));

	/*
	 * Wait until we read back the same value.  Use a counter rather than
	 * ktime for timeout since this may be called after timekeeping stops.
	 */
	for (i = 0; i < USEC_PER_SEC; i++) {
		if (readl(tcs_reg_addr(drv, reg, tcs_id)) == data)
			return;
		udelay(1);
	}
	pr_err("%s: error writing %#x to %d:%#x\n", drv->name,
	       data, tcs_id, reg);
}

/**
 * get_tcs_for_msg() - Get the tcs_group used to send the given message.
 * @drv: The RSC controller.
 * @msg: The message we want to send.
 *
 * This is normally pretty straightforward except if we are trying to send
 * an ACTIVE_ONLY message but don't have any active_only TCSes.
 *
 * Return: A pointer to a tcs_group or an ERR_PTR.
 */
static struct tcs_group *get_tcs_for_msg(struct rsc_drv *drv,
					 const struct tcs_request *msg)
{
	/*
	 * U-Boot: since we're single threaded and running synchronously we can
	 * just always used the first active TCS.
	 */
	if (msg->state != RPMH_ACTIVE_ONLY_STATE) {
		log_debug("WARN: only ACTIVE_ONLY state supported\n");
		return NULL;
	}

	return &drv->tcs[ACTIVE_TCS];
}

/**
 * __tcs_buffer_write() - Write to TCS hardware from a request; don't trigger.
 * @drv:    The controller.
 * @tcs_id: The global ID of this TCS.
 * @cmd_id: The index within the TCS to start writing.
 * @msg:    The message we want to send, which will contain several addr/data
 *          pairs to program (but few enough that they all fit in one TCS).
 *
 * This is used for all types of transfers (active, sleep, and wake).
 */
static void __tcs_buffer_write(struct rsc_drv *drv, int tcs_id, int cmd_id,
			       const struct tcs_request *msg)
{
	u32 msgid;
	u32 cmd_msgid = CMD_MSGID_LEN;
	u32 cmd_enable = 0;
	u32 cmd_complete = 0;
	struct tcs_cmd *cmd;
	int i, j;

	if (!msg->is_read)
		cmd_msgid |= CMD_MSGID_WRITE;

	if (msg->wait_for_compl)
		cmd_msgid |= CMD_MSGID_RESP_REQ;

	cmd_complete = 0; //read_tcs_reg(drv, drv->regs[RSC_DRV_CMD_WAIT_FOR_CMPL], tcs_id);

	for (i = 0, j = cmd_id; i < msg->num_cmds; i++, j++) {
		cmd = &msg->cmds[i];
		cmd_enable |= BIT(j);
		/* U-Boot: always wait for completion */
		cmd_complete |= (!!msg->wait_for_compl) << j;
		msgid = cmd_msgid;

		write_tcs_cmd(drv, drv->regs[RSC_DRV_CMD_MSGID], tcs_id, j, msgid);
		write_tcs_cmd(drv, drv->regs[RSC_DRV_CMD_ADDR], tcs_id, j, cmd->addr);
		if (!msg->is_read)
			write_tcs_cmd(drv, drv->regs[RSC_DRV_CMD_DATA], tcs_id, j, cmd->data);
		debug("tcs(%d): [%s] cmd_id: %d: msgid: %#x addr: %#x data: %#x read: %d\n",
		      tcs_id, msg->state == RPMH_ACTIVE_ONLY_STATE ? "active" : "?", j, msgid,
		      cmd->addr, cmd->data, msg->is_read);
	}

	write_tcs_reg(drv, drv->regs[RSC_DRV_CMD_ENABLE], tcs_id, cmd_enable);
	/* U-Boot: Tell the DRV to wait for completion (?) so we can poll on DRV_STATUS */
	/* This register applies to the entire TCS group not per command */
	/*
	 * FIXME: this seems to rather be a way to impose ordering on commands when multiple are
	 * sent in a single TCS request, needs more testing.
	 */
	write_tcs_reg(drv, drv->regs[RSC_DRV_CMD_WAIT_FOR_CMPL], tcs_id, cmd_complete);
}

/**
 * __tcs_set_trigger() - Start xfer on a TCS or unset trigger on a borrowed TCS
 * @drv:     The controller.
 * @tcs_id:  The global ID of this TCS.
 * @trigger: If true then untrigger/retrigger. If false then just untrigger.
 *
 * In the normal case we only ever call with "trigger=true" to start a
 * transfer. That will un-trigger/disable the TCS from the last transfer
 * then trigger/enable for this transfer.
 *
 * If we borrowed a wake TCS for an active-only transfer we'll also call
 * this function with "trigger=false" to just do the un-trigger/disable
 * before using the TCS for wake purposes again.
 *
 * Note that the AP is only in charge of triggering active-only transfers.
 * The AP never triggers sleep/wake values using this function.
 */
static void __tcs_set_trigger(struct rsc_drv *drv, int tcs_id, bool trigger)
{
	u32 enable;
	u32 reg = drv->regs[RSC_DRV_CONTROL];

	/*
	 * HW req: Clear the DRV_CONTROL and enable TCS again
	 * While clearing ensure that the AMC mode trigger is cleared
	 * U-Boot: keep AMC_MODE_ENABLE flag since clearing it might be
	 * interrupting in-flight commands.
	 */
	enable = read_tcs_reg(drv, reg, tcs_id);
	enable &= ~TCS_AMC_MODE_TRIGGER;
	write_tcs_reg_sync(drv, reg, tcs_id, enable);

	if (trigger) {
		/* Enable the AMC mode on the TCS and then trigger the TCS */
		enable = TCS_AMC_MODE_ENABLE;
		write_tcs_reg_sync(drv, reg, tcs_id, enable);
		enable |= TCS_AMC_MODE_TRIGGER;
		write_tcs_reg(drv, reg, tcs_id, enable);
	}
}

/**
 * rpmh_rsc_send_data() - Write / trigger active-only message.
 * @drv: The controller.
 * @msg: The data to be sent.
 *
 * NOTES:
 * - This is only used for "ACTIVE_ONLY" since the limitations of this
 *   function don't make sense for sleep/wake cases.
 * - To do the transfer, we will grab a whole TCS for ourselves--we don't
 *   try to share. If there are none available we'll wait indefinitely
 *   for a free one.
 * - This function will not wait for the commands to be finished, only for
 *   data to be programmed into the RPMh. See rpmh_tx_done() which will
 *   be called when the transfer is fully complete.
 * - This function must be called with interrupts enabled. If the hardware
 *   is busy doing someone else's transfer we need that transfer to fully
 *   finish so that we can have the hardware, and to fully finish it needs
 *   the interrupt handler to run. If the interrupts is set to run on the
 *   active CPU this can never happen if interrupts are disabled.
 *
 * Return: 0 on success, -EINVAL on error.
 */
int rpmh_rsc_send_data(struct rsc_drv *drv, const struct tcs_request *msg)
{
	struct tcs_group *tcs;
	int tcs_id, i = 0;
	u32 val;
	u32 addr;

	tcs = get_tcs_for_msg(drv, msg);
	if (IS_ERR(tcs))
		return PTR_ERR(tcs);

	/* U-Boot is single-threaded, always use the first TCS as we'll never conflict */
	tcs_id = tcs->offset;
	if (!read_tcs_reg(drv, drv->regs[RSC_DRV_STATUS], tcs_id)) {
		pr_err("%s: TCS %d is busy!\n", __func__, tcs_id);
		return -EBUSY;
	}

	tcs->req[tcs_id - tcs->offset] = msg;

	/*
	 * These two can be done after the lock is released because:
	 * - We marked "tcs_in_use" under lock.
	 * - Once "tcs_in_use" has been marked nobody else could be writing
	 *   to these registers until the interrupt goes off.
	 * - The interrupt can't go off until we trigger w/ the last line
	 *   of __tcs_set_trigger() below.
	 */
	__tcs_buffer_write(drv, tcs_id, 0, msg);
	__tcs_set_trigger(drv, tcs_id, true);

	/*
	 * U-Boot: Now wait for the status done flag and for the TCS to be cleared.
	 * Just waiting for one of these isn't always enough. On reads we have to wait for
	 * the status register to indicate that the response data is ready.
	 */
	for (i = 0; i < USEC_PER_SEC; i++) {
		addr = readl_relaxed(drv->tcs_base + drv->regs[RSC_DRV_IRQ_STATUS]);
		if (addr & BIT(tcs_id))
			break;
		udelay(1);
	}

	for (i = 0; i < USEC_PER_SEC; i++) {
		val = read_tcs_cmd(drv, drv->regs[RSC_DRV_CMD_STATUS], tcs_id, 0);
		if (val & CMD_STATUS_COMPL)
			break;
		udelay(1);
	}

	/* U-Boot: read the response when it becomes available */
	if (msg->is_read) {
		msg->cmds[0].data = read_tcs_cmd(drv, drv->regs[RSC_DRV_CMD_RESP_DATA], tcs_id, 0);
		log_debug("data response: %#x\n", msg->cmds[0].data);
	}

	__tcs_set_trigger(drv, tcs_id, false);
	writel_relaxed(BIT(tcs_id), drv->tcs_base + drv->regs[RSC_DRV_IRQ_CLEAR]);

	if (i == USEC_PER_SEC) {
		log_err("%s: error writing %#x to %d:%#x\n", drv->name,
			msg->cmds[0].addr, tcs_id, drv->regs[RSC_DRV_CMD_ADDR]);
		return -EINVAL;
	}

	return 0;
}

static int rpmh_probe_tcs_config(struct udevice *dev, struct rsc_drv *drv)
{
	struct tcs_type_config {
		u32 type;
		u32 n;
	} tcs_cfg[TCS_TYPE_NR] = { { 0 } };
	ofnode dn = dev_ofnode(dev);
	u32 config, max_tcs, ncpt, offset;
	int i, ret, n, st = 0;
	struct tcs_group *tcs;

	ret = ofnode_read_u32(dn, "qcom,tcs-offset", &offset);
	if (ret)
		return ret;
	drv->tcs_base = drv->base + offset;

	config = readl_relaxed(drv->base + drv->regs[DRV_PRNT_CHLD_CONFIG]);

	max_tcs = config;
	max_tcs &= DRV_NUM_TCS_MASK << (DRV_NUM_TCS_SHIFT * drv->id);
	max_tcs = max_tcs >> (DRV_NUM_TCS_SHIFT * drv->id);

	ncpt = config & (DRV_NCPT_MASK << DRV_NCPT_SHIFT);
	ncpt = ncpt >> DRV_NCPT_SHIFT;

	n = ofnode_read_u32_array(dn, "qcom,tcs-config", (u32 *)tcs_cfg, 2 * TCS_TYPE_NR);
	if (n < 0) {
		log_err("RPMh: %s: error reading qcom,tcs-config %d\n", dev->name, n);
		return n;
	}

	for (i = 0; i < TCS_TYPE_NR; i++) {
		if (tcs_cfg[i].n > MAX_TCS_PER_TYPE)
			return -EINVAL;
	}

	for (i = 0; i < TCS_TYPE_NR; i++) {
		tcs = &drv->tcs[tcs_cfg[i].type];
		if (tcs->drv)
			return -EINVAL;
		tcs->drv = drv;
		tcs->type = tcs_cfg[i].type;
		tcs->num_tcs = tcs_cfg[i].n;
		tcs->ncpt = ncpt;

		if (!tcs->num_tcs || tcs->type == CONTROL_TCS)
			continue;

		if (st + tcs->num_tcs > max_tcs ||
		    st + tcs->num_tcs >= BITS_PER_BYTE * sizeof(tcs->mask))
			return -EINVAL;

		tcs->mask = ((1 << tcs->num_tcs) - 1) << st;
		tcs->offset = st;
		st += tcs->num_tcs;
	}

	drv->num_tcs = st;

	return 0;
}

static int rpmh_rsc_probe(struct udevice *dev)
{
	ofnode dn = dev_ofnode(dev);
	struct rsc_drv *drv;
	char drv_id[10] = {0};
	int ret;
	u32 rsc_id;

	drv = dev_get_priv(dev);

	ret = ofnode_read_u32(dn, "qcom,drv-id", &drv->id);
	if (ret)
		return ret;

	drv->name = ofnode_get_property(dn, "label", NULL);
	if (!drv->name)
		drv->name = dev->name;

	snprintf(drv_id, ARRAY_SIZE(drv_id), "drv-%d", drv->id);
	drv->base = (void __iomem *)dev_read_addr_name(dev, drv_id);
	if (IS_ERR(drv->base))
		return PTR_ERR(drv->base);

	rsc_id = readl_relaxed(drv->base + RSC_DRV_ID);
	drv->ver.major = rsc_id & (MAJOR_VER_MASK << MAJOR_VER_SHIFT);
	drv->ver.major >>= MAJOR_VER_SHIFT;
	drv->ver.minor = rsc_id & (MINOR_VER_MASK << MINOR_VER_SHIFT);
	drv->ver.minor >>= MINOR_VER_SHIFT;

	if (drv->ver.major == 3)
		drv->regs = rpmh_rsc_reg_offset_ver_3_0;
	else
		drv->regs = rpmh_rsc_reg_offset_ver_2_7;

	ret = rpmh_probe_tcs_config(dev, drv);
	if (ret)
		return ret;

	spin_lock_init(&drv->lock);
	init_waitqueue_head(&drv->tcs_wait);
	bitmap_zero(drv->tcs_in_use, MAX_TCS_NR);

	/* Enable the active TCS to send requests immediately */
	writel_relaxed(drv->tcs[ACTIVE_TCS].mask,
		       drv->tcs_base + drv->regs[RSC_DRV_IRQ_ENABLE]);

	spin_lock_init(&drv->client.cache_lock);
	INIT_LIST_HEAD(&drv->client.cache);
	INIT_LIST_HEAD(&drv->client.batch_cache);

	dev_set_drvdata(dev, drv);
	drv->dev = dev;

	log_debug("RPMh: %s: v%d.%d\n", dev->name, drv->ver.major, drv->ver.minor);

	return ret;
}

static int rpmh_rsc_remove(struct udevice *dev)
{
	struct rsc_drv *drv = dev_get_priv(dev);
	struct tcs_group *tcs = &drv->tcs[ACTIVE_TCS];
	u32 tcs_id = tcs->offset;

	for (int i = 0; i < 10 && !read_tcs_reg(drv, drv->regs[RSC_DRV_STATUS], tcs_id); i++) {
		if (i == 0)
			printf("Waiting for TCS %d to be free...\n", tcs_id);
		udelay(100);
	}

	/* Clean up for the OS! */
	write_tcs_reg(drv, drv->regs[RSC_DRV_CMD_WAIT_FOR_CMPL], tcs_id, 0);

	/* Clear the AMC enable bit for all TCS commands we used */
	write_tcs_reg_sync(drv, drv->regs[RSC_DRV_CONTROL], tcs_id, 0);
	/* Disable all commands for the single AMC we used */
	write_tcs_reg_sync(drv, drv->regs[RSC_DRV_CMD_ENABLE], tcs_id, 0);

	/* Make sure IRQs are all clear too */
	writel_relaxed(tcs->mask, drv->tcs_base + drv->regs[RSC_DRV_IRQ_CLEAR]);

	return 0;
}

static const struct udevice_id qcom_rpmh_ids[] = {
	{ .compatible = "qcom,rpmh-rsc" },
	{ }
};

U_BOOT_DRIVER(qcom_rpmh_rsc) = {
	.name		= "qcom_rpmh_rsc",
	.id		= UCLASS_MISC,
	.priv_auto	= sizeof(struct rsc_drv),
	.probe		= rpmh_rsc_probe,
	.remove		= rpmh_rsc_remove,
	.of_match	= qcom_rpmh_ids,
	/* rpmh is under CLUSTER_PD which we don't support, so skip trying to enable PDs */
	.flags		= DM_FLAG_DEFAULT_PD_CTRL_OFF | DM_FLAG_OS_PREPARE,
};

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. RPMh Driver");
MODULE_LICENSE("GPL v2");

/*
 * Hyp DM counterpart of the EL1 driver above — co-located on purpose so both
 * share the register-offset tables, the TCS semantics and the drv-version
 * rule: this is where the RPMH MMIO spy lives (dev/RPMH_SPY.md).
 *
 * Flow:
 * - hv_start_vm() runs the Hyp DM pass (dm_hyp_scan()) right after it has
 *   installed its stage-2 map; that binds the `hypervisor`-tagged apps_rsc
 *   node to rpmh_rsc_hyp_probe() below.  The probe reads the register region
 *   from the DT and hands it to the hypervisor with hyp_mmio_watch().
 * - Every guest RPMH access then faults into EL2, where hyp/mmio.c replays it
 *   write-through against the real RSC and calls hyp_spy_hook() here.
 *
 * Pointer semantics:
 * - HYP_DRIVER(qcom_rpmh_rsc_hyp) lands in the .hyp output section
 *   (arch/arm/cpu/armv8/u-boot.lds claims __u_boot_list_2_hyp_driver*
 *   there), so the struct — and its .probe pointer — keep their link-time
 *   addresses: relocate_aarch64.c skips relocations inside
 *   [__hyp_start, __hyp_stop) and the EL2 copy is the live one.
 * - rpmh_rsc_hyp_probe() is an ordinary (non-__hyp) function in this same
 *   file as the EL1 driver: it runs pre-reloc, when the link-time copy of
 *   every section is the one in use, so calling it through the unrelocated
 *   struct driver is safe.
 * - Everything that runs after the EL1 handoff — the hook, the decode helpers
 *   and the state they read and write — is __hyp / __hyp_data / __hyp_rodata,
 *   part of the resident .hyp section hyp/s1.c maps for EL2.
 * - The EL1 driver and its scan path above are untouched; this block is
 *   compiled only with CONFIG_HYP_SPY_RPMH.
 */
#if IS_ENABLED(CONFIG_HYP_SPY_RPMH) || IS_ENABLED(CONFIG_HYP_RPMH_BCM_CAP)

/* Uncomment to enable the per-access [SPY] wr trace: */
/* #define LOG_DEBUG */	/* off: [SPY] per-access prints stall the trap path */

/* One line per call: hyp_printf() takes the console lock, tags the line with the core and puts it
 * in the log ring that the :2222 console serves, as it does for every other EL2 print. */
#define spy_printf(fmt, ...)	hyp_printf("[SPY] ", fmt, ##__VA_ARGS__)
#ifdef LOG_DEBUG
#define spy_dbg(fmt, ...)	spy_printf(fmt, ##__VA_ARGS__)
#else
#define spy_dbg(fmt, ...)	do { } while (0)
#endif

/* Same drv-version rule as rpmh_rsc_probe(): the RSC hardware reports its
 * version in the RSC_DRV_ID register (SM8550/SM8650+ read v3.0); the
 * probe runs at EL2 with the MMU off and no stage-2 installed yet, so the
 * plain readl is safe here. */
static const u32 *rpmh_rsc_reg_offsets(u32 rsc_drv_base)
{
	u32 rsc_id = readl_relaxed((void __iomem *)(uintptr_t)(rsc_drv_base + RSC_DRV_ID));
	u32 major = (rsc_id & (MAJOR_VER_MASK << MAJOR_VER_SHIFT)) >> MAJOR_VER_SHIFT;

	if (major == 3)
		return rpmh_rsc_reg_offset_ver_3_0;
	return rpmh_rsc_reg_offset_ver_2_7;
}

/*
 * Spy state.  hyp_regs is a snapshot of the driver's own register-offset
 * table, taken by the probe: the .data copy of rpmh_rsc_reg_offset_ver_*
 * moves with the guest when U-Boot relocates, this copy stays in .data.hyp
 * next to the code that reads it.  Every access below is indexed with the
 * RSC_* enum above, so the register layout is described in exactly one place.
 */
static __hyp_data u32 hyp_regs[RSC_DRV_CMD_RESP_DATA + 1];
static __hyp_data u64 hyp_tcs_base;
static __hyp_data u32 hyp_n_tcs;
/* BCM_TCS_CMD(): commit [30], valid [29], vote_x [27:14], vote_y [13:0] */
#define BCM_VOTE_VALID		BIT(29)
#define BCM_VOTE_X_SHIFT	14
#define BCM_VOTE_MASK		0x3fff

/**
 * struct hyp_cmd - one TCS command slot as the guest programmed it
 *
 * The spy only observes the stores; the real write-through to the RSC
 * happens in hyp/mmio.c.
 *
 * @msgid:	value written to CMD_MSGID
 * @addr:	value written to CMD_ADDR
 * @data:	value written to CMD_DATA
 * @capped:	the hypervisor lowered a saturated bandwidth vote (HYP_RPMH_BCM_CAP)
 */
struct hyp_cmd {
	u64 msgid;
	u32 addr;
	u32 data;
	bool capped;
};

static __hyp_data struct hyp_cmd hyp_cmds[MAX_TCS_NR][MAX_CMDS_PER_TCS];


/* VRM command-register offsets inside a vote (RPMH_REGULATOR_REG_* in
 * drivers/power/regulator/qcom-rpmh-regulator.c) */
#define VRM_REG_VOLTAGE		0x0
#define VRM_REG_ENABLE		0x4
#define VRM_REG_MODE		0x8
#define VRM_VOLTAGE_MASK	GENMASK(12, 0)
#define VRM_ENABLE_MASK		BIT(0)
#define VRM_MODE_MASK		GENMASK(2, 0)

/* Mode names indexed by the PMIC5 register value, per the PMIC5_*_MODE_*
 * defines in drivers/power/regulator/qcom-rpmh-regulator.c:
 * LDO 3=retention 4=lpm 7=hpm; SMPS 3=retention 4=pfm 6=auto 7=pwm;
 * BOB 2=pass 4=pfm 6=auto 7=pwm.  Gaps (0, 1, 5) are type-specific or
 * undefined on PMIC5 and print as mode<N>. */
static const char __hyp_rodata *const vrm_mode_names[] = {
	"mode0", "mode1", "bypass", "retention", "low-power",
	"mode5", "nominal", "high-power",
};

#define HYP_MAX_VOTES		96

/* cmd-db slave ids (address bits 18:16) of the resources a vote can name */
#define VOTE_SLAVE_ARC		3
#define VOTE_SLAVE_VRM		4
#define VOTE_SLAVE_BCM		5
#define BCM_VOTE_COMMIT		BIT(30)

/**
 * struct hyp_vote - the last value the guest wrote for one named resource
 *
 * A resource is reported when a trigger changes it, not every time it is written again.
 *
 * @name:	cmd-db resource name (bobb1, cx.lvl, MC0, ...)
 * @slave:	cmd-db slave id: 4 regulator, 3 ARC, 5 BCM
 * @capped:	the BCM vote was lowered by the hypervisor
 * @dirty:	changed since it was last reported
 * @uv:		a regulator's voltage in microvolts, -1 until captured
 * @enabled:	its enable register, -1 until captured
 * @mode:	its mode register, -1 until captured
 * @data:	the ARC level or BCM vote word, ~0 until captured
 */
struct hyp_vote {
	char name[12];
	u8 slave;
	bool capped;
	bool dirty;
	int uv;
	int enabled;
	int mode;
	u32 data;
};

static __hyp_data struct hyp_vote hyp_votes[HYP_MAX_VOTES];

/* Which TCS and which register inside it a trapped address hit */
static __hyp bool hyp_tcs_of(u64 far, u32 *id, u32 *off)
{
	u64 span = (u64)hyp_regs[RSC_DRV_TCS_OFFSET] * hyp_n_tcs;

	if (!hyp_n_tcs || far < hyp_tcs_base || far >= hyp_tcs_base + span)
		return false;

	*id = (u32)((far - hyp_tcs_base) / hyp_regs[RSC_DRV_TCS_OFFSET]);
	*off = (u32)((far - hyp_tcs_base) % hyp_regs[RSC_DRV_TCS_OFFSET]);
	return true;
}

/* Find (or lazily add) the row for a cmd-db resource name */
static __hyp struct hyp_vote *vote_lookup(const char *name, u8 slave)
{
	int i, free = -1;

	for (i = 0; i < HYP_MAX_VOTES; i++) {
		if (hyp_votes[i].name[0] &&
		    !strncmp(hyp_votes[i].name, name, sizeof(hyp_votes[i].name)))
			return &hyp_votes[i];
		if (free < 0 && !hyp_votes[i].name[0])
			free = i;
	}

	if (free < 0)
		return NULL;

	strncpy(hyp_votes[free].name, name, sizeof(hyp_votes[free].name));
	hyp_votes[free].name[sizeof(hyp_votes[free].name) - 1] = '\0';
	hyp_votes[free].slave = slave;
	hyp_votes[free].uv = -1;
	hyp_votes[free].enabled = -1;
	hyp_votes[free].mode = -1;
	hyp_votes[free].data = ~0U;
	return &hyp_votes[free];
}

/* A regulator register: the row is dirty only when the value changed */
static __hyp void vote_vrm(const char *name, u32 reg, u32 data)
{
	struct hyp_vote *v = vote_lookup(name, VOTE_SLAVE_VRM);
	int *field, val;

	if (!v)
		return;

	switch (reg) {
	case VRM_REG_VOLTAGE:
		field = &v->uv;
		val = (int)(data & VRM_VOLTAGE_MASK) * 1000;
		break;
	case VRM_REG_ENABLE:
		field = &v->enabled;
		val = (int)(data & VRM_ENABLE_MASK);
		break;
	case VRM_REG_MODE:
		field = &v->mode;
		val = (int)(data & VRM_MODE_MASK);
		break;
	default:
		return;
	}

	if (*field != val) {
		*field = val;
		v->dirty = true;
	}
}

/* An ARC level or a BCM vote word */
static __hyp void vote_word(const char *name, u8 slave, u32 data, bool capped)
{
	struct hyp_vote *v = vote_lookup(name, slave);

	if (v && (v->data != data || v->capped != capped)) {
		v->data = data;
		v->capped = capped;
		v->dirty = true;
	}
}

/* One line for every resource this trigger changed */
static __hyp void report_votes(const char *who)
{
	int i;

	for (i = 0; i < HYP_MAX_VOTES; i++) {
		struct hyp_vote *v = &hyp_votes[i];

		if (!v->name[0] || !v->dirty)
			continue;
		v->dirty = false;

		switch (v->slave) {
		case VOTE_SLAVE_VRM: {
			char uv[12], mode[12];

			if (v->uv >= 0)
				snprintf(uv, sizeof(uv), "%d mV", v->uv / 1000);
			else
				snprintf(uv, sizeof(uv), "- mV");
			if (v->mode >= 0 && v->mode < (int)ARRAY_SIZE(vrm_mode_names))
				snprintf(mode, sizeof(mode), "%s", vrm_mode_names[v->mode]);
			else if (v->mode >= 0)
				snprintf(mode, sizeof(mode), "mode%d", v->mode);
			else
				snprintf(mode, sizeof(mode), "-");
			spy_printf("%s: %s %s, %s, %s\n", who, v->name,
				   v->enabled > 0 ? "enabled" : (v->enabled == 0 ? "disabled" : "-"),
				   uv, mode);
			break;
		}
		case VOTE_SLAVE_ARC:
			spy_printf("%s: %s level %u\n", who, v->name, v->data);
			break;
		case VOTE_SLAVE_BCM:
			spy_printf("%s: %s bandwidth x=%#x y=%#x%s%s%s\n", who, v->name,
				   (v->data >> BCM_VOTE_X_SHIFT) & BCM_VOTE_MASK, v->data & BCM_VOTE_MASK,
				   v->data & BCM_VOTE_COMMIT ? ", commit" : "",
				   v->data & BCM_VOTE_VALID ? "" : ", not valid",
				   v->capped ? ", capped" : "");
			break;
		default:
			spy_printf("%s: %s slave %u data=%#x\n", who, v->name, v->slave, v->data);
		}
	}
}

/*
 * Decode every captured command slot of a triggered TCS.  Named votes update the resource table
 * (regulators, ARC levels, BCM bandwidth) and the resources the trigger changed are reported
 * once, one line each; a vote that repeats what was already sent says nothing.  Unnamed votes
 * print raw.  Before EL1 initialises cmd-db the votes print as unnamed - the hyp probe
 * (rpmh_rsc_hyp_probe) already called cmd_db_init_dt() for us.
 */
static __hyp void hyp_decode_tcs(u32 id, const char *who)
{
	u32 slot;

	if (id >= MAX_TCS_NR)
		return;

	for (slot = 0; slot < MAX_CMDS_PER_TCS; slot++) {
		struct hyp_cmd *cmd = &hyp_cmds[id][slot];
		char nm[12];

		if (!cmd->addr)
			continue;

		/* rpmh_read programs MSGID (without the WRITE bit) and
		 * ADDR into the same slots to fetch a resource value;
		 * that is a read, not a vote */
		if (!(cmd->msgid & CMD_MSGID_WRITE)) {
			cmd->msgid = 0;
			cmd->addr = 0;
			cmd->data = 0;
			cmd->capped = false;
			continue;
		}

		if (cmd_db_ready() &&
		    !cmd_db_read_name(cmd->addr, nm, sizeof(nm))) {
			u8 slave = (cmd->addr >> 16) & 7;

			if (slave == VOTE_SLAVE_VRM)
				/* reg = addr & 0xC picks voltage/enable/mode */
				vote_vrm(nm, cmd->addr & 0xC, cmd->data);
			else
				vote_word(nm, slave, cmd->data, cmd->capped);
		} else {
			spy_printf("%s: tcs%u cmd%u addr=%#x data=%#x (unnamed)\n", who, id, slot,
				   cmd->addr, cmd->data);
		}

		/* Consumed: the next trigger rewrites the slots it uses,
		 * so drop the capture instead of letting stale slots
		 * produce phantom votes */
		cmd->msgid = 0;
		cmd->addr = 0;
		cmd->data = 0;
		cmd->capped = false;
	}

	report_votes(who);
}

/*
 * hyp_mmio_watch() hook on the apps_rsc span.  The RSC itself is passed
 * through: every guest write reaches the hardware and every guest read is
 * answered by it.  This hook only *observes* - it records the TCS programming
 * for the console decode.  The completion interrupt is not its business:
 * hyp/rpmh.c forwards that for every TCS of the guest's DRV, watched or not
 * (hyp_rpmh_guest_irq()).
 *
 * Two callers share this window.  U-Boot at EL1, after hv_start_vm() drops it
 * there, IS the stage-2 guest, and the kernel booted from the UKI is the
 * other; @os tells the kernel's writes apart (hyp_s2_is_uboot_access()), and
 * EL2's own accesses never reach the hook at all.
 */
static __hyp void hyp_spy_hook(int cpu, u64 far, bool store, u32 size, u64 *val,
			       bool from_guest)
{
	bool os = from_guest && !hyp_s2_is_uboot_access(cpu);
	u32 id, off;

	if (!store)
		return;

	spy_dbg("wr %#llx size=%u val=%#llx (cpu%d, %s)\n", far, size, *val, cpu,
		os ? "kernel" : (from_guest ? "u-boot" : "el2"));

	if (!hyp_tcs_of(far, &id, &off))
		return;

	/* Everything the guest writes to the RSC reaches the RSC; the hook only
	 * reads the writes.  The bank header (DRV_IRQ_ENABLE at +0x00) and the
	 * status/clear registers carry nothing to decode. */
	if (off == 0 || off == hyp_regs[RSC_DRV_IRQ_STATUS] ||
	    off == hyp_regs[RSC_DRV_IRQ_CLEAR])
		return;

	/* TCS trigger: the AMC CONTROL register with the TRIGGER bit set.  Log
	 * and decode what the guest just programmed (spy builds only). */
	if (off == hyp_regs[RSC_DRV_CONTROL] && (*val & TCS_AMC_MODE_TRIGGER)) {
		if (IS_ENABLED(CONFIG_HYP_SPY_RPMH))
			hyp_decode_tcs(id, os ? "kernel" : (from_guest ? "u-boot" : "el2"));
		return;
	}

	/* Command slots: CMD_MSGID/CMD_ADDR/CMD_DATA are CMD_OFFSET apart,
	 * starting at CMD_MSGID.  `reg` is the offset INSIDE the slot. */
	if (off >= hyp_regs[RSC_DRV_CMD_MSGID]) {
		u32 delta = off - hyp_regs[RSC_DRV_CMD_MSGID];
		u32 slot = delta / hyp_regs[RSC_DRV_CMD_OFFSET];
		u32 reg = delta % hyp_regs[RSC_DRV_CMD_OFFSET];

		if (slot < MAX_CMDS_PER_TCS && id < hyp_n_tcs) {
			if (reg == 0)
				hyp_cmds[id][slot].msgid = *val;
			else if (reg == hyp_regs[RSC_DRV_CMD_ADDR] -
				 hyp_regs[RSC_DRV_CMD_MSGID])
				hyp_cmds[id][slot].addr = (u32)*val;
			else if (reg == hyp_regs[RSC_DRV_CMD_DATA] -
				 hyp_regs[RSC_DRV_CMD_MSGID]) {
				/*
				 * Until sync_state the kernel's interconnect driver votes
				 * every bandwidth BCM at its saturated maximum (x = y =
				 * 0x3fff).  The one on the memory controller (MC0) makes the
				 * AOP raise DDR to its top frequency and the board resets,
				 * whatever ACV, the QUP votes or the number of TCS in use
				 * (x = y = 0x400 on MC0 is safe).  Cap such votes; a vote
				 * the OS computed itself, below the cap, passes unchanged.
				 */
				hyp_cmds[id][slot].capped = false;
				if (IS_ENABLED(CONFIG_HYP_RPMH_BCM_CAP) && os && (*val & BCM_VOTE_VALID) &&
				    ((*val >> BCM_VOTE_X_SHIFT) & BCM_VOTE_MASK) == BCM_VOTE_MASK &&
				    (*val & BCM_VOTE_MASK) == BCM_VOTE_MASK) {
					*val = (*val & ~((u64)BCM_VOTE_MASK << BCM_VOTE_X_SHIFT | BCM_VOTE_MASK)) |
					       (u64)CONFIG_HYP_RPMH_BCM_CAP_VOTE << BCM_VOTE_X_SHIFT |
					       CONFIG_HYP_RPMH_BCM_CAP_VOTE;
					hyp_cmds[id][slot].capped = true;
				}
				hyp_cmds[id][slot].data = (u32)*val;
			}
		}
	}
}

static int rpmh_rsc_hyp_probe(struct udevice *dev)
{
	ofnode dn = dev_ofnode(dev);
	const u32 *regs;
	const __be32 *cells;
	u32 drv_id, offset, n_tcs = 0;
	u32 tcs_pairs[10];	/* <type count> pairs of qcom,tcs-config */
	phys_addr_t base, start, end = 0;
	int i, n, len;

	/* Which drv window the apps processor writes (qcom,drv-id), and its
	 * reg entry — the same lookup rpmh_rsc_probe() does via
	 * dev_read_addr_name(dev, "drv-N"). */
	drv_id = dev_read_u32_default(dev, "qcom,drv-id", 2);
	base = dev_read_addr_index(dev, drv_id);
	if (base == FDT_ADDR_T_NONE) {
		dev_err(dev, "hyp: no reg window for drv-%u\n", drv_id);
		return -EINVAL;
	}

	regs = rpmh_rsc_reg_offsets((u32)base);
	memcpy(hyp_regs, regs, sizeof(hyp_regs));

	/* Watch span: from the first reg window's base to the last window's
	 * end (drv-0..drv-2 on sm8650.dtsi), so all three drv windows trap. */
	start = FDT_ADDR_T_NONE;
	for (i = 0; ; i++) {
		fdt_size_t size;
		phys_addr_t wbase = dev_read_addr_size_index(dev, i, &size);

		if (wbase == FDT_ADDR_T_NONE)
			break;
		if (wbase < start)
			start = wbase;
		if (wbase + size > end)
			end = wbase + size;
	}

	/* TCS bank of the apps drv window (qcom,tcs-offset) */
	if (dev_read_u32(dev, "qcom,tcs-offset", &offset)) {
		dev_err(dev, "hyp: missing qcom,tcs-offset\n");
		return -EINVAL;
	}
	hyp_tcs_base = base + offset;

	/* n_tcs = sum of the counts in the qcom,tcs-config <type count>
	 * pairs (same rule as rpmh_probe_tcs_config(): apps_rsc has
	 * ACTIVE 3 + SLEEP 2 + WAKE 2 + CONTROL 0 = 7 TCS) */
	cells = ofnode_read_prop(dn, "qcom,tcs-config", &len);
	if (cells) {
		n = len / 4;
		if (n > (int)ARRAY_SIZE(tcs_pairs))
			n = ARRAY_SIZE(tcs_pairs);
		for (i = 0; i < n; i++)
			tcs_pairs[i] = be32_to_cpu(cells[i]);
		for (i = 1; i < n; i += 2)
			n_tcs += tcs_pairs[i];
	} else {
		n = 0;
	}
	hyp_n_tcs = n_tcs ? min_t(u32, n_tcs, MAX_TCS_NR) : MAX_TCS_NR;

	/* Trap the whole RSC region for the rest of the run: our stage-2
	 * blocks for it go invalid, so every guest access faults into EL2 and
	 * comes back here through hyp_s2_emulate(). */
	hyp_mmio_watch(start, end, hyp_spy_hook);

	/* Name the votes later: read the cmd-db location from the DTB now
	 * (EL1's qcom_cmd_db bind path stays as-is). */
	cmd_db_init_dt();

	printf("rpmh hyp: tcs_base=%#llx tcs_size=%u cmd_size=%u n_tcs=%u (tcs-config cells: %d)\n",
	       hyp_tcs_base, hyp_regs[RSC_DRV_TCS_OFFSET],
	       hyp_regs[RSC_DRV_CMD_OFFSET], hyp_n_tcs, n);

	return 0;
}

static const struct udevice_id qcom_rpmh_hyp_ids[] = {
	{ .compatible = "qcom,rpmh-rsc" },
	{ }
};

HYP_DRIVER(qcom_rpmh_rsc_hyp) = {
	.name		= "rpmh_rsc_hyp",
	.id		= UCLASS_MISC,
	.of_match	= qcom_rpmh_hyp_ids,
	.probe		= rpmh_rsc_hyp_probe,
	/* rpmh is under CLUSTER_PD which we don't support */
	.flags		= DM_FLAG_DEFAULT_PD_CTRL_OFF,
};
#endif
