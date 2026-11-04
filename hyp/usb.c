// SPDX-License-Identifier: GPL-2.0+
/*
 * The USB controller the hypervisor takes over from the guest
 * (scripts/USB_GADGET.md).
 *
 * The board DT tags the controller node with `hypervisor-takeover`.  The EL1
 * guest (U-Boot) keeps using it, for fastboot, until it is about to boot the
 * kernel; its board setup then releases its own drivers, deletes the node from
 * the kernel's device tree and issues HV_USB_TAKEOVER.  The takeover:
 *
 *  - votes the supplies of the controller's HS PHY and eUSB2 repeater on the
 *    hypervisor's own RPMH DRV (hyp/rpmh.c).  RPMh aggregates the DRVs, so the
 *    kernel's regulator framework cannot switch them off by voting its own DRV;
 *  - denies the register windows of the controller and of its HS PHY to the
 *    guest: a load reads zero and a store is dropped (hyp_mmio_deny());
 *  - watches the clock controller for stores to the controller's gates, reset
 *    and power domain, and rewrites them so the guest cannot gate, reset or
 *    collapse what EL2 is using.  The GDSC and the branch clocks are plain MMIO
 *    in the clock controller, not RPMH, so a vote cannot protect them.
 *
 * Everything needed from the DT is read once at boot and kept in __hyp_data,
 * because the DT is not reachable after the guest runs.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <hyp/usb.h>
#include <dm/ofnode.h>
#include <asm/io.h>
#include <errno.h>
#include <linux/types.h>
#include <soc/qcom/cmd-db.h>
#include <stdio.h>
#include <string.h>

#include "hv.h"
#include "net.h"

#define USB_MAX_VREGS	8

/* RPMh VRM resource registers: voltage in mV, then enable */
#define VRM_REG_VOLTAGE	0x0
#define VRM_REG_ENABLE	0x4

struct usb_vreg {
	u32 addr;		/* command DB address of the resource */
	u32 mv;			/* voltage to vote, 0 = leave it */
	char name[12];		/* "ldob15", for the log */
};

struct hyp_usb {
	bool found;		/* the DT names a controller for EL2 to take over */
	bool taken;		/* the guest has released it; EL2 owns it */
	bool link;		/* the controller is up and the network stack runs on it */
	bool waiting;		/* the guest is halted in HV_USB_TAKEOVER until `boot` */
	u64 start, end;		/* controller register window [start, end) */
	u64 phy_start, phy_end;	/* its HS PHY's window, 0 when there is none */
	u64 gcc_start, gcc_end;	/* the clock controller that gates it */
	u64 tcsr_start;		/* the controller that gates the PHY's reference clock */
	u32 rail_addr;		/* RPMh ARC resource of the rail the power domain sits on, 0 = none */
	u32 rail_level;		/* the corner the controller's required-opps ask for */
	u32 master_cfg;		/* GCC_USB30_PRIM_MASTER_CLK_SRC's CFG_RCGR as U-Boot set it, 0 = unknown */
	int intid;		/* the controller's INTID, 0 = unknown */
	int nvreg;
	struct usb_vreg vreg[USB_MAX_VREGS];
};

static __hyp_data struct hyp_usb usb = { 0 };

/*
 * Registers of the clock controller (offsets from its base, from
 * gcc-sm8650.c) whose guest stores are rewritten: reset bits cleared, power
 * domain collapse cleared, branch clock enables forced on.
 */
struct usb_gate {
	u32 off;
	u32 clear;
	u32 set;
};

static const struct usb_gate __hyp_rodata usb_gates[] = {
	{ 0x12000, BIT(0), 0 },		/* GCC_QUSB2PHY_PRIM_BCR */
	{ 0x39000, BIT(0), 0 },		/* GCC_USB30_PRIM_BCR */
	{ 0x39004, BIT(0), 0 },		/* USB30_PRIM_GDSC: SW_COLLAPSE */
	{ 0x39018, 0, BIT(0) },		/* GCC_USB30_PRIM_MASTER_CLK */
	{ 0x39024, 0, BIT(0) },		/* GCC_USB30_PRIM_SLEEP_CLK */
	{ 0x39028, 0, BIT(0) },		/* GCC_USB30_PRIM_MOCK_UTMI_CLK */
	{ 0x39088, 0, BIT(0) },		/* GCC_CFG_NOC_USB3_PRIM_AXI_CLK */
	{ 0x3908c, 0, BIT(0) },		/* GCC_AGGRE_USB3_PRIM_AXI_CLK */
};

#define GCC_USB30_PRIM_MASTER_RCG	0x3902c	/* CMD_RCGR; CFG_RCGR follows */
#define RCG_UPDATE			BIT(0)

/* The first enabled node, at any depth, carrying the `hypervisor-takeover` flag */
static ofnode usb_find_owned(ofnode parent)
{
	ofnode node;

	ofnode_for_each_subnode(node, parent) {
		ofnode found;

		if (ofnode_is_enabled(node) &&
		    ofnode_read_bool(node, "hypervisor-takeover"))
			return node;

		found = usb_find_owned(node);
		if (ofnode_valid(found))
			return found;
	}

	return ofnode_null();
}

ofnode hyp_usb_node(void)
{
	return usb_find_owned(ofnode_root());
}

/* The node the @index'th phandle of @prop names (the first cell of each entry only) */
static ofnode usb_phandle_node(ofnode node, const char *prop, int index)
{
	u32 ph;

	if (ofnode_read_u32_index(node, prop, index, &ph))
		return ofnode_null();

	return ofnode_get_by_phandle(ph);
}

static bool usb_window(ofnode node, u64 *start, u64 *end)
{
	phys_addr_t size;
	fdt_addr_t addr = ofnode_get_addr_size_index_notrans(node, 0, &size);

	if (addr == FDT_ADDR_T_NONE || !size)
		return false;

	*start = addr;
	*end = addr + size;

	return true;
}

/*
 * One regulator node of a `qcom,*-rpmh-regulators` block ("ldo5" under
 * `qcom,pmic-id = "b"`) as a command DB resource ("ldob5").
 */
static void usb_add_vreg(ofnode reg)
{
	const char *id = ofnode_read_string(ofnode_get_parent(reg), "qcom,pmic-id");
	const char *name = ofnode_get_name(reg);
	struct usb_vreg *v;
	const char *pfx, *num;
	char res[sizeof(v->name)];
	u32 addr, uv;
	int i;

	if (!id)
		return;			/* not an RPMh regulator (a fixed supply) */

	if (!strncmp(name, "ldo", 3)) {
		pfx = "ldo";
		num = name + 3;
	} else if (!strncmp(name, "smps", 4)) {
		pfx = "smp";
		num = name + 4;
	} else if (!strncmp(name, "bob", 3)) {
		pfx = "bob";
		num = name + 3;
	} else {
		return;
	}

	snprintf(res, sizeof(res), "%s%s%s", pfx, id, num);
	addr = cmd_db_read_addr(res);
	if (!addr) {
		hprintf("usb: no command DB entry for %s, its supply is not voted\n", res);
		return;
	}

	for (i = 0; i < usb.nvreg; i++)
		if (usb.vreg[i].addr == addr)
			return;
	if (usb.nvreg == USB_MAX_VREGS)
		return;

	v = &usb.vreg[usb.nvreg++];
	v->addr = addr;
	uv = ofnode_read_u32_default(reg, "regulator-min-microvolt", 0);
	v->mv = uv / 1000;
	strlcpy(v->name, res, sizeof(v->name));
}

/* Every `*-supply` the node names */
static void usb_add_supplies(ofnode node)
{
	struct ofprop prop;

	ofnode_for_each_prop(prop, node) {
		const char *pname;
		const fdt32_t *val;
		size_t n;
		int len;

		val = ofprop_get_property(&prop, &pname, &len);
		n = strlen(pname);
		if (!val || len != sizeof(*val) || n < 8 || strcmp(pname + n - 7, "-supply"))
			continue;

		usb_add_vreg(ofnode_get_by_phandle(fdt32_to_cpu(*val)));
	}
}

void __hyp hyp_usb_init(void)
{
	ofnode node = hyp_usb_node(), phy, rep, gcc, tcsr;
	u64 tcsr_end;

	if (!ofnode_valid(node)) {
		hprintf("usb: no node tagged hypervisor-takeover, USB stays with the guest\n");
		return;
	}

	if (!usb_window(node, &usb.start, &usb.end)) {
		hprintf("usb: %s has no usable reg, not taking it over\n",
			ofnode_get_name(node));
		return;
	}

	/* The HS PHY the controller lists first as "usb2-phy", and the repeater behind it */
	if (ofnode_stringlist_search(node, "phy-names", "usb2-phy") == 0) {
		phy = usb_phandle_node(node, "phys", 0);
		if (ofnode_valid(phy)) {
			usb_window(phy, &usb.phy_start, &usb.phy_end);
			tcsr = usb_phandle_node(phy, "clocks", 0);
			if (ofnode_valid(tcsr))
				usb_window(tcsr, &usb.tcsr_start, &tcsr_end);
			usb_add_supplies(phy);
			rep = usb_phandle_node(phy, "phys", 0);
			if (ofnode_valid(rep))
				usb_add_supplies(rep);
		}
	}

	/* The clock controller: the provider of the controller's first clock */
	gcc = usb_phandle_node(node, "clocks", 0);
	if (ofnode_valid(gcc))
		usb_window(gcc, &usb.gcc_start, &usb.gcc_end);
	if (usb.gcc_end)
		usb.master_cfg = readl(usb.gcc_start + GCC_USB30_PRIM_MASTER_RCG + 4);

	/* The corner the controller's power domain needs on the logic rail: the
	 * OPP its required-opps names (opp-level, an RPMh vlvl).  The rail is CX, which
	 * is what the GCC power domains sit on (rpmhpd.c uses "cx.lvl" for them). */
	rep = usb_phandle_node(node, "required-opps", 0);
	if (ofnode_valid(rep)) {
		usb.rail_level = ofnode_read_u32_default(rep, "opp-level", 0);
		usb.rail_addr = cmd_db_read_addr("cx.lvl");
	}

	/* "dwc_usb3": an SPI, <phandle type number flags ...> in interrupts-extended */
	if (ofnode_stringlist_search(node, "interrupt-names", "dwc_usb3") == 0) {
		u32 type, num;

		if (!ofnode_read_u32_index(node, "interrupts-extended", 1, &type) && type == 0 &&
		    !ofnode_read_u32_index(node, "interrupts-extended", 2, &num))
			usb.intid = 32 + num;
	}

	usb.found = true;
	hyp_clean_range(&usb, sizeof(usb));

	hprintf("usb: %s [%#llx-%#llx], hs phy [%#llx-%#llx], clocks [%#llx-%#llx], %d supplies; taken over when the guest releases it\n",
		ofnode_get_name(node), usb.start, usb.end, usb.phy_start, usb.phy_end,
		usb.gcc_start, usb.gcc_end, usb.nvreg);
	for (int i = 0; i < usb.nvreg; i++)
		hprintf("usb:   supply %s at %#x, %u mV\n", usb.vreg[i].name,
			usb.vreg[i].addr, usb.vreg[i].mv);
}

/*
 * Guest stores to the controller's reset, power domain and clock gates are
 * rewritten (usb_gates).  Everything else in the clock controller passes
 * through the replay untouched.
 */
static void __hyp usb_gcc_hook(int cpu, u64 far, bool store, u32 size, u64 *val,
			       bool from_guest)
{
	u64 off = far - usb.gcc_start;
	int i;

	if (!store || size != 4)
		return;

	/*
	 * The master clock's RCG (clk_rcg2_shared_ops in the guest) is parked on the 19.2 MHz
	 * crystal whenever the guest gates the clock.  The controller then still enumerates and
	 * moves control transfers, but never sends a bulk IN packet; the configuration U-Boot
	 * left (GPLL0 / 2.5) stays.
	 */
	if (off == GCC_USB30_PRIM_MASTER_RCG + 4 && usb.master_cfg) {
		*val = usb.master_cfg;
		return;
	}

	for (i = 0; i < ARRAY_SIZE(usb_gates); i++)
		if (usb_gates[i].off == off) {
			*val = (*val & ~(u64)usb_gates[i].clear) | usb_gates[i].set;
			return;
		}
}

/* Vote one supply on EL2's DRV: its voltage, then its enable */
static void __hyp usb_vote(const struct usb_vreg *v)
{
	u32 en = ~0U;
	int ret_v = 0, ret_e, ret_r;

	if (v->mv)
		ret_v = hyp_rpmh_write(v->addr + VRM_REG_VOLTAGE, v->mv);
	ret_e = hyp_rpmh_write(v->addr + VRM_REG_ENABLE, 1);
	ret_r = hyp_rpmh_read(v->addr + VRM_REG_ENABLE, &en);

	hprintf("usb: vote %s %u mV: voltage %d, enable %d, readback %d (enable=%#x)\n",
		v->name, v->mv, ret_v, ret_e, ret_r, en);
}

/*
 * Bringing the controller up for EL2's own use.  The guest released its drivers
 * first (hyp-usb.c), which gated the clocks and reset the controller, so this
 * starts from nothing: power domain, clock gates, controller reset, the PHY
 * (reset pulse, then the register sequence of phy-snps-eusb2.c's Qualcomm
 * path), and the glue's VBUS override that makes the controller see a session
 * with no Type-C stack.  Offsets are from gcc-sm8650.c / tcsrcc-sm8650.c; the
 * bases come from the DT.  EL2's own accesses are not subject to stage 2.
 */
#define GCC_USB30_PRIM_BCR		0x39000
#define GCC_USB30_PRIM_GDSCR		0x39004
#define GCC_QUSB2PHY_PRIM_BCR		0x12000
#define TCSR_USB3_CLKREF_EN		0x31108
#define TCSR_USB2_CLKREF_EN		0x31118
#define GCC_USB30_PRIM_MOCK_UTMI_RCG	0x39044

#define GDSCR_SW_COLLAPSE		BIT(0)
/* USB30_PRIM_GDSC has POLL_CFG_GDSCR (gcc-sm8650.c): its status is CFG_GDSCR, not GDSCR[31] */
#define CFG_GDSCR_POWER_UP_COMPLETE	BIT(16)
#define CBCR_ENABLE			BIT(0)
#define CBCR_OFF			BIT(31)

static const u32 __hyp_rodata usb_gcc_gates[] = {
	0x39018,	/* GCC_USB30_PRIM_MASTER_CLK */
	0x39028,	/* GCC_USB30_PRIM_MOCK_UTMI_CLK */
	0x39024,	/* GCC_USB30_PRIM_SLEEP_CLK */
	0x3908c,	/* GCC_AGGRE_USB3_PRIM_AXI_CLK */
	0x39088,	/* GCC_CFG_NOC_USB3_PRIM_AXI_CLK */
};

/* qscratch glue, inside the controller's window (dwc3-qcom.c) */
#define QSCRATCH_OFFSET			0xf8800
#define QSCRATCH_HS_PHY_CTRL		0x10
#define QSCRATCH_SS_PHY_CTRL		0x30
#define QSCRATCH_GENERAL_CFG		0x08
#define PIPE_UTMI_CLK_SEL		BIT(0)
#define PIPE3_PHYSTATUS_SW		BIT(3)
#define PIPE_UTMI_CLK_DIS		BIT(8)
#define UTMI_OTG_VBUS_VALID		BIT(20)
#define SW_SESSVLD_SEL			BIT(28)
#define LANE0_PWR_PRESENT		BIT(24)

#define DWC3_GSNPSID			0xc120

static bool __hyp usb_wait(u64 reg, u32 mask, u32 want, u32 us)
{
	u64 t0 = hyp_ticks();

	while ((readl(reg) & mask) != want)
		if (hyp_ticks_elapsed(t0, us))
			return false;

	return true;
}

static void __hyp usb_maskw(u64 reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
	readl(reg);		/* make sure the write has landed */
}

/* The Qualcomm eUSB2 PHY init of phy-snps-eusb2.c, 19.2 MHz reference */
static void __hyp usb_phy_init(u64 base)
{
	usb_maskw(base + 0x94, BIT(1), BIT(1));		/* CFG0: CMN_CTRL_OVERRIDE_EN */
	usb_maskw(base + 0x50, BIT(1), BIT(1));		/* UTMI_CTRL5: POR */
	usb_maskw(base + 0x54, BIT(0) | BIT(3), BIT(0) | BIT(3));	/* PHY_ENABLE | RETENABLEN */
	usb_maskw(base + 0x130, BIT(2), BIT(2));	/* APB_LOGIC_RESET */
	usb_maskw(base + 0x98, BIT(6), 0);		/* CMN_CTRL0: TESTBURNIN */
	usb_maskw(base + 0xb8, BIT(0), BIT(0));		/* FSEL_SEL */

	/* 38.4 MHz reference: what U-Boot's PHY driver programs on this board, read back
	 * from its working state (FSEL 4, FB_DIV 0xc8) */
	usb_maskw(base + 0x54, GENMASK(6, 4), 0x4 << 4);	/* FSEL 38.4 MHz */
	usb_maskw(base + 0x5c, GENMASK(7, 0), 0xc8);	/* PLL FB_DIV[7:0] */
	usb_maskw(base + 0x60, GENMASK(3, 0), 0x0);	/* PLL FB_DIV[11:8] */
	usb_maskw(base + 0x60, GENMASK(7, 4), 0);	/* PLL REF_DIV */

	usb_maskw(base + 0x58, GENMASK(7, 1), 0x1 << 1);	/* CPBIAS */
	usb_maskw(base + 0x68, GENMASK(7, 2), 0x8 << 2);	/* INT_CNTRL */
	usb_maskw(base + 0x68, GENMASK(1, 0), 0x1);	/* GMP_CNTRL */
	usb_maskw(base + 0x6c, GENMASK(4, 0), 0x10);	/* PROP_CNTRL */
	usb_maskw(base + 0x70, GENMASK(2, 0), 0);	/* VCO_CNTRL */
	usb_maskw(base + 0x6c, GENMASK(7, 6), 0x1 << 6);	/* VREF_TUNE */
	usb_maskw(base + 0x64, BIT(4), BIT(4));		/* VBUS_DET_EXT_SEL */

	usb_maskw(base + 0x7c, GENMASK(2, 0), 0);	/* TX pre-emphasis */
	usb_maskw(base + 0x7c, GENMASK(6, 5), 0x2 << 5);	/* TX rise/fall */
	usb_maskw(base + 0x7c, GENMASK(4, 3), 0x1 << 3);	/* source impedance */
	usb_maskw(base + 0x78, GENMASK(5, 3), 0x3 << 3);	/* DC level */
	usb_maskw(base + 0x78, GENMASK(7, 6), 0);	/* HS crossover */

	usb_maskw(base + 0x64, BIT(3) | BIT(2), BIT(3) | BIT(2));	/* SUSPEND_N_SEL | SUSPEND_N */
	usb_maskw(base + 0x3c, BIT(0), BIT(0));		/* UTMI_CTRL0: SLEEPM */
	usb_maskw(base + 0x54, BIT(1), BIT(1));		/* SIDDQ_SEL */
	usb_maskw(base + 0x54, BIT(2), 0);		/* SIDDQ */
	usb_maskw(base + 0x50, BIT(1), 0);		/* UTMI_CTRL5: POR */
	usb_maskw(base + 0x64, BIT(3), 0);		/* SUSPEND_N_SEL */
	/* CFG0's CMN_CTRL_OVERRIDE_EN stays set, as in U-Boot's working state */
}

static int __hyp usb_bringup(void)
{
	u64 gcc = usb.gcc_start, tcsr = usb.tcsr_start;
	int i, bad = 0;

	if (!gcc || !tcsr || !usb.phy_end) {
		hprintf("usb: no clock controller, TCSR or PHY window, cannot bring the controller up\n");
		return -ENODEV;
	}

	/* Power domain: out of collapse, wait for it to report on */
	if (usb.rail_addr && usb.rail_level) {
		u32 lvl = ~0U;
		int w = hyp_rpmh_write(usb.rail_addr, usb.rail_level);
		int r = hyp_rpmh_read(usb.rail_addr, &lvl);

		hprintf("usb: vote cx.lvl %u: write %d, readback %d (%#x)\n",
			usb.rail_level, w, r, lvl);
	}

	/*
	 * The guest's driver removal left the controller held in reset
	 * (GCC_USB30_PRIM_BCR set).  With the reset still asserted the power domain
	 * never finishes powering up (GDSCR stuck at state 6, run 160), so the
	 * reset is released first; the real reset pulse comes after the clocks.
	 */
	writel(0, gcc + GCC_USB30_PRIM_BCR);
	hyp_udelay(50);

	usb_maskw(gcc + GCC_USB30_PRIM_GDSCR, GDSCR_SW_COLLAPSE, 0);
	if (!usb_wait(gcc + GCC_USB30_PRIM_GDSCR + 4, CFG_GDSCR_POWER_UP_COMPLETE,
		      CFG_GDSCR_POWER_UP_COMPLETE, 20000)) {
		hprintf("usb: USB30_PRIM_GDSC did not come on (GDSCR %#x, CFG_GDSCR %#x)\n",
			readl(gcc + GCC_USB30_PRIM_GDSCR),
			readl(gcc + GCC_USB30_PRIM_GDSCR + 4));
		bad++;
	}

	/* The PHY's reference clock, then the controller's branch clocks */
	usb_maskw(tcsr + TCSR_USB2_CLKREF_EN, CBCR_ENABLE, CBCR_ENABLE);
	usb_maskw(tcsr + TCSR_USB3_CLKREF_EN, CBCR_ENABLE, CBCR_ENABLE);
	hyp_udelay(10);

	/* The guest parked the master clock's RCG when it released the controller */
	if (usb.master_cfg && readl(gcc + GCC_USB30_PRIM_MASTER_RCG + 4) != usb.master_cfg) {
		writel(usb.master_cfg, gcc + GCC_USB30_PRIM_MASTER_RCG + 4);
		usb_maskw(gcc + GCC_USB30_PRIM_MASTER_RCG, RCG_UPDATE, RCG_UPDATE);
		if (!usb_wait(gcc + GCC_USB30_PRIM_MASTER_RCG, RCG_UPDATE, 0, 1000)) {
			hprintf("usb: master clock RCG does not update (%#x)\n",
				readl(gcc + GCC_USB30_PRIM_MASTER_RCG));
			bad++;
		}
	}

	for (i = 0; i < ARRAY_SIZE(usb_gcc_gates); i++) {
		usb_maskw(gcc + usb_gcc_gates[i], CBCR_ENABLE, CBCR_ENABLE);
		if (!usb_wait(gcc + usb_gcc_gates[i], CBCR_OFF, 0, 1000)) {
			hprintf("usb: clock gate %#x stays off (%#x)\n", usb_gcc_gates[i],
				readl(gcc + usb_gcc_gates[i]));
			bad++;
		}
	}

	hprintf("usb: GDSCR %#x, master RCG cmd/cfg %#x/%#x, mock_utmi RCG cmd/cfg %#x/%#x\n",
		readl(gcc + GCC_USB30_PRIM_GDSCR),
		readl(gcc + GCC_USB30_PRIM_MASTER_RCG),
		readl(gcc + GCC_USB30_PRIM_MASTER_RCG + 4),
		readl(gcc + GCC_USB30_PRIM_MOCK_UTMI_RCG),
		readl(gcc + GCC_USB30_PRIM_MOCK_UTMI_RCG + 4));

	/* The controller and the PHY each get a reset pulse */
	writel(1, gcc + GCC_USB30_PRIM_BCR);
	hyp_udelay(50);
	writel(0, gcc + GCC_USB30_PRIM_BCR);
	hyp_udelay(50);

	writel(1, gcc + GCC_QUSB2PHY_PRIM_BCR);
	hyp_udelay(150);
	writel(0, gcc + GCC_QUSB2PHY_PRIM_BCR);
	hyp_udelay(50);

	usb_phy_init(usb.phy_start);

	/* No SS PHY runs, so there is no pipe clock: the controller uses the UTMI clock
	 * for it, as dwc3-qcom.c does for `qcom,select-utmi-as-pipe-clk`. */
	usb_maskw(usb.start + QSCRATCH_OFFSET + QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_DIS,
		  PIPE_UTMI_CLK_DIS);
	hyp_udelay(100);
	usb_maskw(usb.start + QSCRATCH_OFFSET + QSCRATCH_GENERAL_CFG,
		  PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW, PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW);
	hyp_udelay(100);
	usb_maskw(usb.start + QSCRATCH_OFFSET + QSCRATCH_GENERAL_CFG, PIPE_UTMI_CLK_DIS, 0);

	/* A session without a Type-C stack: the VBUS override of dwc3-qcom.c */
	usb_maskw(usb.start + QSCRATCH_OFFSET + QSCRATCH_SS_PHY_CTRL,
		  LANE0_PWR_PRESENT, LANE0_PWR_PRESENT);
	usb_maskw(usb.start + QSCRATCH_OFFSET + QSCRATCH_HS_PHY_CTRL,
		  UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL,
		  UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);

	hprintf("usb: PHY COMMON0 %#x CTRL2 %#x UTMI_CTRL5 %#x; GENERAL_CFG %#x\n",
		readl(usb.phy_start + 0x54), readl(usb.phy_start + 0x64),
		readl(usb.phy_start + 0x50), readl(usb.start + QSCRATCH_OFFSET + QSCRATCH_GENERAL_CFG));
	hprintf("usb: controller GSNPSID %#x, %d resource problems\n",
		readl(usb.start + DWC3_GSNPSID), bad);

	return bad ? -EIO : 0;
}

/**
 * hyp_usb_takeover() - the guest says it is done with the USB controller
 * @pcpu: the core that took the HVC
 *
 * Return: 0 on success, -ENODEV when the DT names no controller, -EALREADY when
 * it was taken before.
 */
long __hyp hyp_usb_takeover(struct hyp_percpu *pcpu)
{
	int i;

	if (!usb.found)
		return -ENODEV;
	if (usb.taken)
		return -EALREADY;

	if (hyp_rpmh_ready())
		for (i = 0; i < usb.nvreg; i++)
			usb_vote(&usb.vreg[i]);
	else
		hprintf("usb: no EL2 RPMH voter, the supplies are left to the guest\n");

	if (!usb_bringup() && !hyp_ncm_init(usb.start, usb.intid))
		usb.link = true;

	hyp_mmio_deny(usb.start, usb.end);
	if (usb.phy_end)
		hyp_mmio_deny(usb.phy_start, usb.phy_end);
	if (usb.gcc_end)
		hyp_mmio_watch(usb.gcc_start, usb.gcc_end, usb_gcc_hook);

	usb.taken = true;
	hyp_clean_range(&usb, sizeof(usb));

	hprintf("usb: cpu%u took over [%#llx-%#llx]; guest accesses there read as zero\n",
		(unsigned)pcpu->id, usb.start, usb.end);

	/*
	 * The guest stops here, inside its own HVC, until `boot` is typed on the hypervisor's
	 * console (hyp/telnet.c): the debug link is up and nothing of the OS has started, so
	 * whatever has to be set up on the host first (a GDB session, a console logger) can be.
	 * The controller is polled meanwhile; this core has its interrupts masked.
	 */
	if (IS_ENABLED(CONFIG_HYP_USB_HALT) && usb.link) {
		hprintf("usb: guest halted, waiting for `boot` on the console (telnet 192.168.77.2 2323)\n");
		usb.waiting = true;
		while (usb.waiting)
			hyp_dwc3_poll();
		hprintf("usb: boot requested\n");
	}

	return 0;
}

/* Is the guest halted in its takeover HVC, waiting for `boot`? */
bool __hyp hyp_usb_boot_waiting(void)
{
	return usb.waiting;
}

/* `boot`: let the halted guest go on.  False when it was not waiting. */
bool __hyp hyp_usb_boot_release(void)
{
	bool was = usb.waiting;

	usb.waiting = false;

	return was;
}
