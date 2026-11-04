// SPDX-License-Identifier: GPL-2.0+
/*
 * EL2's own way to the PMICs: the PMIC arbiter's debug bus.
 *
 * The arbiter that serves the SoC's software (`qcom,spmi-pmic-arb`, what the guest's
 * driver runs) gives each execution environment a channel per peripheral, and a
 * transaction on a channel is a sequence of register writes (data, command) and a poll of the
 * channel's status: an access from outside that sequence corrupts the one in flight, and a
 * write needs a channel of the environment that owns the peripheral.  The debug bus is a second
 * arbiter port with one channel of its own that reaches every peripheral of every PMIC whatever
 * its owner (Linux: `qcom,spmi-pmic-arb-debug`; the register layout and the command sequence
 * are the ones the boot firmware's debug interface uses).  It is only available when the
 * chip's debug fuse allows it, which the node names (`reg-names` "fuse", with
 * `qcom,fuse-enable-bit` or `qcom,fuse-disable-bit`).
 *
 * The board's device tree describes the bus; this file never hardcodes where it is.  The
 * guest's own device tree does not have the node, so the guest never touches it.  Only the
 * GDB server (hyp/gdbmem.c) uses it, one request at a time: the channel has no ownership of
 * its own to protect it from two callers.
 *
 * An address in the space the server shows is one SPMI register:
 *
 *	bit 20		SPMI bus instance (0 the primary bus)
 *	bits 19:16	the PMIC's slave id (USID)
 *	bits 15:8	the peripheral (PID)
 *	bits 7:0	the register
 *
 * A transaction moves at most 8 bytes inside one peripheral (extended register read/write long
 * commands), so longer accesses are split.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/io.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <hypervisor.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>

#include "hv.h"

/* The debug bus's registers (the arbiter's "PMIC_ARB_DEBUG" block) */
#define DBG_CMD0		0x00
#define DBG_CMD0_OPCODE		GENMASK(5, 1)
#define DBG_CMD0_PRIORITY	BIT(0)
#define DBG_CMD1		0x04
#define DBG_CMD1_BUS		BIT(7)
#define DBG_CMD1_BYTE_CNT	GENMASK(6, 4)		/* bytes - 1 */
#define DBG_CMD1_SID		GENMASK(3, 0)
#define DBG_CMD2		0x08			/* the peripheral */
#define DBG_CMD3		0x0c			/* the register in it */
#define DBG_STATUS		0x14
#define DBG_STATUS_DROPPED	BIT(3)
#define DBG_STATUS_DENIED	BIT(2)
#define DBG_STATUS_FAILURE	BIT(1)
#define DBG_STATUS_DONE		BIT(0)
#define DBG_WDATA(i)		(0x18 + 4 * (i))	/* one byte each */
#define DBG_RDATA(i)		(0x38 + 4 * (i))
#define DBG_SIZE		0x5c

/* The opcodes the arbiter's command register takes (the same as the normal channels') */
#define OP_EXT_WRITE_LONG	0
#define OP_EXT_READ_LONG	1

#define SPMI_MAX_BYTES		8	/* per transaction */
#define SPMI_PERIPH_SIZE	0x100	/* a peripheral's registers */
#define SPMI_XFER_TIMEOUT_US	1000

/* The addresses the GDB server uses */
#define SPMI_ADDR_BUS		BIT(20)
#define SPMI_ADDR_SID		GENMASK(19, 16)
#define SPMI_ADDR_PID		GENMASK(15, 8)
#define SPMI_ADDR_MAX		GENMASK(20, 0)

enum spmi_fuse { FUSE_UNKNOWN, FUSE_OPEN, FUSE_CLOSED };

struct hyp_spmi {
	bool present;
	u64 base;		/* the debug bus's registers */
	u64 fuse;		/* the fuse word that says whether it works; 0: not described */
	u32 fuse_mask;		/* the bit in it */
	bool fuse_enables;	/* the bit set means available (else: set means disabled) */
	enum spmi_fuse state;	/* what the fuse said, read when the first access is made */
	bool clk_ok;		/* the QDSS clock vote worked: the bus answers */
};

static __hyp_data struct hyp_spmi spmi = { 0 };

/* The register @name of @node's `reg` list, or FDT_ADDR_T_NONE */
static fdt_addr_t spmi_reg(ofnode node, const char *name, phys_addr_t *size)
{
	int idx = ofnode_stringlist_search(node, "reg-names", name);

	return idx < 0 ? FDT_ADDR_T_NONE : ofnode_get_addr_size_index_notrans(node, idx, size);
}

/*
 * The debug bus runs on the QDSS clock, which the AOSS owns: the boot firmware holds it for its
 * own debug interface, and the OS's driver asks for it through the `core_clk` clock of the node
 * (`clocks = <&aoss_qmp>`, a QMP message "{class: clock, res: qdss, val: 1}").  Without the vote
 * (nothing holds it without a debugger attached) the first register access never finishes and the
 * board resets.  This is the part of the QMP protocol that sends one message (the OS's
 * drivers/soc/qcom/qcom_aoss.c): message RAM and the IPCC doorbell both come from the DT.  It
 * runs once at boot, before the guest does, and leaves the clock on and the link open: the AOP
 * does not open its side of a link again once one was closed (the guest's driver then fails with
 * "ucore didn't open channel", and everything that waits for the AOSS node with it, the GPU's
 * GMU for one), while an open link satisfies every wait of the guest's own handshake at once.
 */
#define QMP_DESC_MAGIC			0x00
#define QMP_DESC_VERSION		0x04
#define QMP_DESC_UCORE_LINK_STATE	0x0c
#define QMP_DESC_UCORE_LINK_STATE_ACK	0x10
#define QMP_DESC_UCORE_CH_STATE		0x14
#define QMP_DESC_UCORE_CH_STATE_ACK	0x18
#define QMP_DESC_MCORE_LINK_STATE	0x24
#define QMP_DESC_MCORE_LINK_STATE_ACK	0x28
#define QMP_DESC_MCORE_CH_STATE		0x2c
#define QMP_DESC_MCORE_CH_STATE_ACK	0x30
#define QMP_DESC_MCORE_MBOX_SIZE	0x34
#define QMP_DESC_MCORE_MBOX_OFFSET	0x38
#define QMP_STATE_UP			0x0000ffff
#define QMP_STATE_DOWN			0xffff0000
#define QMP_MAGIC			0x4d41494c
#define QMP_VERSION			1
#define QMP_MSG_LEN			64
#define QMP_TIMEOUT_US			1000000
#define IPCC_REG_SEND_ID		0x0c

struct qmp {
	u64 ram;		/* the message RAM */
	u64 ipcc_send;		/* the doorbell register */
	u32 ipcc_id;		/* client << 16 | signal of the AOP's doorbell */
	u32 off;		/* the message mailbox in the RAM */
};

static void qmp_kick(const struct qmp *q)
{
	writel(q->ipcc_id, q->ipcc_send);
}

static int qmp_wait(const struct qmp *q, u32 off, u32 want)
{
	u64 t0 = hyp_ticks();

	while (readl(q->ram + off) != want)
		if (hyp_ticks_elapsed(t0, QMP_TIMEOUT_US))
			return -ETIMEDOUT;

	return 0;
}

/* Take our half of the link down again: only for a handshake that failed half way */
static void qmp_close(const struct qmp *q)
{
	writel(QMP_STATE_DOWN, q->ram + QMP_DESC_MCORE_CH_STATE);
	writel(QMP_STATE_DOWN, q->ram + QMP_DESC_MCORE_LINK_STATE);
	qmp_kick(q);
}

/* The link handshake of qmp_open() and one message; leaves the link open when it worked */
static int qmp_send_once(struct qmp *q, const char *msg, bool close_on_fail)
{
	char buf[QMP_MSG_LEN] __aligned(4) = { 0 };
	int ret;

	if (readl(q->ram + QMP_DESC_MAGIC) != QMP_MAGIC ||
	    readl(q->ram + QMP_DESC_VERSION) != QMP_VERSION)
		return -EINVAL;

	q->off = readl(q->ram + QMP_DESC_MCORE_MBOX_OFFSET);
	if (!readl(q->ram + QMP_DESC_MCORE_MBOX_SIZE))
		return -EINVAL;

	writel(readl(q->ram + QMP_DESC_UCORE_LINK_STATE), q->ram + QMP_DESC_UCORE_LINK_STATE_ACK);
	writel(QMP_STATE_UP, q->ram + QMP_DESC_MCORE_LINK_STATE);
	qmp_kick(q);
	ret = qmp_wait(q, QMP_DESC_MCORE_LINK_STATE_ACK, QMP_STATE_UP);
	if (ret)
		goto out;

	writel(QMP_STATE_UP, q->ram + QMP_DESC_MCORE_CH_STATE);
	qmp_kick(q);
	ret = qmp_wait(q, QMP_DESC_UCORE_CH_STATE, QMP_STATE_UP);
	if (ret)
		goto out;

	writel(QMP_STATE_UP, q->ram + QMP_DESC_UCORE_CH_STATE_ACK);
	qmp_kick(q);
	ret = qmp_wait(q, QMP_DESC_MCORE_CH_STATE_ACK, QMP_STATE_UP);
	if (ret)
		goto out;

	/* the guest's driver shares this mailbox: only write into an empty one */
	ret = qmp_wait(q, q->off, 0);
	if (ret)
		goto out;

	/* the message RAM only takes 32-bit accesses: the text, then its length */
	strlcpy(buf, msg, sizeof(buf));
	for (u32 i = 0; i < QMP_MSG_LEN / 4; i++)
		writel(((u32 *)buf)[i], q->ram + q->off + 4 + 4 * i);
	writel(QMP_MSG_LEN, q->ram + q->off);
	readl(q->ram + q->off);
	qmp_kick(q);
	ret = qmp_wait(q, q->off, 0);
	if (ret)
		writel(0, q->ram + q->off);
out:
	if (ret && close_on_fail)
		qmp_close(q);

	return ret;
}

#define QDSS_ON		"{class: clock, res: qdss, val: 1}"

static __hyp_data struct qmp spmi_qmp;

/* Ask the AOSS (the node `clocks` points to) for the QDSS clock; 0 when it is on */
static int spmi_vote_qdss(ofnode node)
{
	phys_addr_t size;
	fdt_addr_t ipcc;
	u32 ph, mbox[3];
	ofnode aoss, mb;

	if (ofnode_read_u32(node, "clocks", &ph))
		return -ENOENT;
	aoss = ofnode_get_by_phandle(ph);
	if (!ofnode_valid(aoss) || ofnode_read_u32_array(aoss, "mboxes", mbox, 3))
		return -ENOENT;
	mb = ofnode_get_by_phandle(mbox[0]);
	if (!ofnode_valid(mb))
		return -ENOENT;

	spmi_qmp.ram = ofnode_get_addr_size_index_notrans(aoss, 0, &size);
	ipcc = ofnode_get_addr_size_index_notrans(mb, 0, &size);
	if (spmi_qmp.ram == FDT_ADDR_T_NONE || ipcc == FDT_ADDR_T_NONE) {
		spmi_qmp.ram = 0;
		return -ENOENT;
	}
	spmi_qmp.ipcc_send = ipcc + IPCC_REG_SEND_ID;
	spmi_qmp.ipcc_id = mbox[1] << 16 | mbox[2];
	hyp_clean_range(&spmi_qmp, sizeof(spmi_qmp));

	return qmp_send_once(&spmi_qmp, QDSS_ON, true);
}

/**
 * hyp_spmi_init() - find the debug bus in the device tree, once at boot
 *
 * The QDSS clock is voted for here (the bus does not answer without it).  Nothing else is
 * accessed: the fuse is read when the first transaction is made.
 */
void __hyp hyp_spmi_init(void)
{
	ofnode node = ofnode_by_compatible(ofnode_null(), "qcom,spmi-pmic-arb-debug");
	phys_addr_t size = 0;
	fdt_addr_t base, fuse;
	u32 bit;
	int ret;

	if (!ofnode_valid(node) || !ofnode_is_enabled(node)) {
		hprintf("spmi: no debug arbiter in the device tree, no SPMI server\n");
		return;
	}

	base = spmi_reg(node, "core", &size);
	if (base == FDT_ADDR_T_NONE || size < DBG_SIZE) {
		hprintf("spmi: %s has no usable \"core\" window\n", ofnode_get_name(node));
		return;
	}
	spmi.base = base;

	fuse = spmi_reg(node, "fuse", &size);
	if (fuse != FDT_ADDR_T_NONE) {
		if (!ofnode_read_u32(node, "qcom,fuse-enable-bit", &bit))
			spmi.fuse_enables = true;
		else if (ofnode_read_u32(node, "qcom,fuse-disable-bit", &bit))
			bit = 32;

		if (bit < 32) {
			spmi.fuse = fuse;
			spmi.fuse_mask = BIT(bit);
		} else {
			hprintf("spmi: %s lists a fuse but no bit, not checking it\n",
				ofnode_get_name(node));
		}
	}

	/* The bus needs the QDSS clock: no vote, no bus (the first access would reset the board) */
	ret = spmi_vote_qdss(node);
	if (ret) {
		hprintf("spmi: the QDSS clock vote for %s failed (%d), no SPMI server\n",
			ofnode_get_name(node), ret);
		return;
	}

	spmi.present = true;
	spmi.clk_ok = true;
	hyp_clean_range(&spmi, sizeof(spmi));
	hprintf("spmi: debug arbiter %s at %#llx\n", ofnode_get_name(node), (u64)spmi.base);
}

bool __hyp hyp_spmi_present(void)
{
	return spmi.present;
}

/**
 * hyp_spmi_connect() - a client of the SPMI server connected: vote the QDSS clock on again
 *
 * The OS's coresight drivers (or anything else on its AOSS clock) may have voted it off since
 * boot, and the AOSS keeps no use count.  The guest's driver owns the QMP link by now, so the
 * handshake finds it open and only the message goes out, into the mailbox if it is empty.  Until
 * a vote has worked, the bus is not touched.
 */
void __hyp hyp_spmi_connect(void)
{
	int ret;

	if (!spmi.present || !spmi_qmp.ram)
		return;

	ret = qmp_send_once(&spmi_qmp, QDSS_ON, false);
	spmi.clk_ok = !ret;
	hyp_clean_range(&spmi, sizeof(spmi));
	if (ret)
		hprintf("spmi: the QDSS clock vote failed (%d), the SPMI server refuses access\n", ret);
}

static int __hyp arb_rd(u32 off, u32 *val)
{
	return hyp_phys_read(spmi.base + off, val, sizeof(*val));
}

static int __hyp arb_wr(u32 off, u32 val)
{
	return hyp_phys_write(spmi.base + off, &val, sizeof(val));
}

/* Whether the chip allows the debug bus: the fuse, read once */
static int __hyp arb_available(void)
{
	u32 val;

	if (spmi.state == FUSE_UNKNOWN) {
		spmi.state = FUSE_OPEN;
		if (spmi.fuse) {
			if (hyp_phys_read(spmi.fuse, &val, sizeof(val))) {
				hprintf("spmi: the debug fuse at %#llx does not answer\n", (u64)spmi.fuse);
				spmi.state = FUSE_CLOSED;
			} else if (!!(val & spmi.fuse_mask) != spmi.fuse_enables) {
				hprintf("spmi: the debug bus is disabled by the fuse (%#llx = %#x)\n",
					(u64)spmi.fuse, val);
				spmi.state = FUSE_CLOSED;
			}
		}
	}

	return spmi.state == FUSE_OPEN ? 0 : -ENODEV;
}

/* Wait until the channel reports a status, which the command write clears: returns it, 0 on time-out */
static int __hyp arb_wait(u32 *status)
{
	u64 t0 = hyp_ticks();
	int ret;

	do {
		ret = arb_rd(DBG_STATUS, status);
		if (ret)
			return -EFAULT;
		if (*status)
			return 0;
	} while (!hyp_ticks_elapsed(t0, SPMI_XFER_TIMEOUT_US));

	return -EBUSY;
}

/*
 * One transaction of @n bytes (1 to 8) at @addr, inside one peripheral.
 *
 * Return: 0; -EFAULT when the debug registers abort; -EBUSY when the channel does not finish
 * (a transaction of someone else's is stuck, or the bus is not running); -EPERM when the
 * arbiter denies it; -EIO when the PMIC does not take it (no such peripheral or register) or
 * the arbiter drops it.
 */
static int __hyp arb_xfer(u32 addr, bool write, u8 *buf, u32 n)
{
	u32 status, val;
	int ret;

	/* the previous transaction has to be over (the channel resets to done) */
	ret = arb_wait(&status);
	if (ret)
		return ret;
	if (!(status & DBG_STATUS_DONE))
		return -EBUSY;

	if (write) {
		for (u32 i = 0; i < n; i++) {
			ret = arb_wr(DBG_WDATA(i), buf[i]);
			if (ret)
				return ret;
		}
	}

	ret = arb_wr(DBG_CMD1, FIELD_PREP(DBG_CMD1_BUS, !!(addr & SPMI_ADDR_BUS)) |
			       FIELD_PREP(DBG_CMD1_BYTE_CNT, n - 1) |
			       FIELD_PREP(DBG_CMD1_SID, FIELD_GET(SPMI_ADDR_SID, addr)));
	ret = ret ?: arb_wr(DBG_CMD2, FIELD_GET(SPMI_ADDR_PID, addr));
	ret = ret ?: arb_wr(DBG_CMD3, addr & (SPMI_PERIPH_SIZE - 1));
	/* the write that starts it, normal priority */
	ret = ret ?: arb_wr(DBG_CMD0, FIELD_PREP(DBG_CMD0_OPCODE,
						 write ? OP_EXT_WRITE_LONG : OP_EXT_READ_LONG));
	if (ret)
		return ret;

	/* the command write clears the status; give it a moment before the first poll */
	hyp_udelay(1);
	ret = arb_wait(&status);
	if (ret)
		return ret;
	if (status & DBG_STATUS_DROPPED)
		return -EIO;
	if (!(status & DBG_STATUS_DONE))
		return -EBUSY;
	if (status & DBG_STATUS_DENIED)
		return -EPERM;
	if (status & DBG_STATUS_FAILURE)
		return -EIO;

	if (!write) {
		for (u32 i = 0; i < n; i++) {
			ret = arb_rd(DBG_RDATA(i), &val);
			if (ret)
				return ret;
			buf[i] = val;
		}
	}

	return 0;
}

static int __hyp spmi_access(u64 addr, u8 *buf, u32 len, bool write)
{
	int ret = spmi.clk_ok ? arb_available() : -ENODEV;

	if (ret)
		return ret;
	if (!len)
		return 0;

	/* the bus bit is part of the address: an access cannot run from one bus into the other */
	if (addr > SPMI_ADDR_MAX || addr + len - 1 > SPMI_ADDR_MAX ||
	    (addr ^ (addr + len - 1)) & SPMI_ADDR_BUS)
		return -EFAULT;

	while (len) {
		u32 n = min3(len, (u32)SPMI_MAX_BYTES, SPMI_PERIPH_SIZE - ((u32)addr & (SPMI_PERIPH_SIZE - 1)));

		ret = arb_xfer(addr, write, buf, n);
		if (ret)
			return ret;

		addr += n;
		buf += n;
		len -= n;
	}

	return 0;
}

/**
 * hyp_spmi_read() - read @len bytes of PMIC registers at SPMI address @addr into @buf
 *
 * @addr is bus, slave id, peripheral and register as described at the top of the file.  Both
 * accesses split into transactions of at most 8 bytes inside one peripheral; the registers
 * after the last one of a peripheral are the first of the next peripheral.
 *
 * Return: 0, or a negative errno below 100 (see arb_xfer(); -ENODEV when the fuse does not
 * allow the debug bus, -EFAULT for an address outside the space).
 */
int __hyp hyp_spmi_read(u64 addr, void *buf, u32 len)
{
	return spmi_access(addr, buf, len, false);
}

/**
 * hyp_spmi_write() - write @len bytes at @buf to the PMIC registers at SPMI address @addr
 *
 * Return: as hyp_spmi_read().
 */
int __hyp hyp_spmi_write(u64 addr, const void *buf, u32 len)
{
	u8 copy[SPMI_MAX_BYTES];
	int ret = 0;

	/* a write goes out in transactions of 8: the const buffer is never written, copy per chunk */
	while (len && !ret) {
		u32 n = min_t(u32, len, sizeof(copy));

		memcpy(copy, buf, n);
		ret = spmi_access(addr, copy, n, true);
		addr += n;
		buf += n;
		len -= n;
	}

	return ret;
}
