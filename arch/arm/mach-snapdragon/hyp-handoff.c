// SPDX-License-Identifier: GPL-2.0+
/*
 * EL1 half of the USB hand-over to the hypervisor (hyp/usb.c,
 * scripts/USB_GADGET.md).
 *
 * U-Boot uses the controller for fastboot, so it is the guest's until the OS is
 * about to start.  The device-tree fixup that runs for every EFI boot is the
 * last moment this code runs before that, which makes it the hand-over point:
 *
 *  1. remove the DM devices bound to the controller, so the clocks, PHY and
 *     reset are released here and not by the exit-boot-services cleanup after
 *     the hypervisor owns the hardware;
 *  2. ask the hypervisor to take the controller (HV_USB_TAKEOVER);
 *  3. delete the controller from the OS's device tree and disable its eUSB2 PHY
 *     and repeater, which have no use without it.
 *
 * The controller is the node the board DT tags `hypervisor-takeover`.
 */
#define LOG_CATEGORY LOGC_BOARD
#define pr_fmt(fmt) "QCOM: " fmt

#include <dm.h>
#include <dm/device-internal.h>
#include <dm/ofnode.h>
#include <fdt_support.h>
#include <hypervisor.h>
#include <hyp/usb.h>
#include <hyp/vdev.h>
#include <malloc.h>
#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/libfdt.h>
#include <log.h>
#include <string.h>

/* The first phandle of a phandle list property, or 0 */
static u32 usb_phandle(const void *blob, int node, const char *prop)
{
	const fdt32_t *cells;
	int len;

	cells = fdt_getprop(blob, node, prop, &len);
	if (!cells || len < (int)sizeof(*cells))
		return 0;

	return fdt32_to_cpu(cells[0]);
}

static void usb_disable(void *blob, int node)
{
	if (node >= 0 && fdt_setprop_string(blob, node, "status", "disabled"))
		log_warning("cannot disable %s in the OS device tree\n",
			    fdt_get_name(blob, node, NULL));
}

/*
 * Find the controller in the OS device tree: same compatible and the same node
 * name (name@unit-address) as the control DT's tagged node.
 */
static int usb_find_in_blob(void *blob, ofnode node)
{
	const char *compat = ofnode_read_string(node, "compatible");
	const char *name = ofnode_get_name(node);
	int off = -1;

	if (!compat)
		return -FDT_ERR_NOTFOUND;

	while ((off = fdt_node_offset_by_compatible(blob, off, compat)) >= 0) {
		if (!strcmp(fdt_get_name(blob, off, NULL), name))
			return off;
	}

	return off;
}

/* A virtual device node at the root: reg and one edge-triggered SPI */
static int vdev_node(void *blob, u32 gic, const char *name, const char *compat, int compat_len,
		     u64 base, u64 size, u32 intid)
{
	fdt32_t reg[4] = { cpu_to_fdt32(base >> 32), cpu_to_fdt32(base),
			   cpu_to_fdt32(size >> 32), cpu_to_fdt32(size) };
	/* this GIC has #interrupt-cells = <4>: type, number, flags, partition (none) */
	fdt32_t irq[4] = { cpu_to_fdt32(0 /* SPI */), cpu_to_fdt32(intid - 32),
			   cpu_to_fdt32(1 /* edge rising */), cpu_to_fdt32(0) };
	int off = fdt_add_subnode(blob, 0, name);

	if (off < 0)
		return off;

	if (fdt_setprop(blob, off, "compatible", compat, compat_len) ||
	    fdt_setprop(blob, off, "reg", reg, sizeof(reg)) ||
	    fdt_setprop(blob, off, "interrupts", irq, sizeof(irq)) ||
	    fdt_setprop_u32(blob, off, "interrupt-parent", gic))
		return -FDT_ERR_NOSPACE;

	return off;
}

/*
 * The hypervisor's virtual NIC (include/hyp/vdev.h), described to the OS: a virtio-mmio net
 * device on a virtual SPI, with nothing behind it but the hypervisor's own network (192.168.77.1
 * without the USB link, which leases the guest 192.168.77.2).  Not in the OS's own device tree
 * sources.
 */
static void vdev_net_dt(void *blob)
{
	u32 gic = fdt_create_phandle_by_compatible(blob, "arm,gic-v3");
	int off;

	if (!gic || fdt_address_cells(blob, 0) != 2 || fdt_size_cells(blob, 0) != 2) {
		log_warning("cannot add the virtio-net node: no GIC or unexpected root cells\n");
		return;
	}

	off = vdev_node(blob, gic, "virtio@40000000", "virtio,mmio", sizeof("virtio,mmio"),
			HYP_VIRTIO_NET_BASE, HYP_VIRTIO_NET_SIZE, HYP_VSPI_NET);
	if (off < 0)
		log_warning("cannot add the virtio-net node: %s\n", fdt_strerror(off));
	else
		log_info("virtio-net added to the OS device tree\n");
}

/*
 * With the USB link the hypervisor also owns the console: a PL011 UART with the fixed clock its
 * driver wants, as stdout - the debug UART the OS used before is disabled.
 */
static void vdev_console_dt(void *blob)
{
	static const char pl011[] = "arm,pl011\0arm,primecell";
	u32 gic = fdt_create_phandle_by_compatible(blob, "arm,gic-v3"), clk_ph;
	int off, clk, chosen, uart;
	fdt32_t clks[2];

	if (!gic)
		return;

	clk = fdt_add_subnode(blob, 0, "vdev-clk");
	if (clk < 0 || fdt_setprop_string(blob, clk, "compatible", "fixed-clock") ||
	    fdt_setprop_u32(blob, clk, "#clock-cells", 0) ||
	    fdt_setprop_u32(blob, clk, "clock-frequency", 24000000)) {
		log_warning("cannot add the virtual devices' clock\n");
		return;
	}
	clk_ph = fdt_create_phandle(blob, clk);
	clks[0] = clks[1] = cpu_to_fdt32(clk_ph);

	uart = vdev_node(blob, gic, "serial@40001000", pl011, sizeof(pl011), HYP_PL011_BASE,
			 HYP_PL011_SIZE, HYP_VSPI_UART);
	if (uart < 0 ||
	    fdt_setprop(blob, uart, "clocks", clks, sizeof(clks)) ||
	    fdt_setprop(blob, uart, "clock-names", "uartclk\0apb_pclk", sizeof("uartclk\0apb_pclk"))) {
		log_warning("cannot add the PL011 node\n");
		return;
	}

	/* the console: our UART; the debug UART is the hypervisor's, off the OS's tree */
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0)
		fdt_setprop_string(blob, chosen, "stdout-path", "/serial@40001000:115200n8");
	off = fdt_node_offset_by_compatible(blob, -1, "qcom,geni-debug-uart");
	if (off >= 0)
		usb_disable(blob, off);

	log_info("PL011 console added to the OS device tree\n");
}

/*
 * The ADSP the hypervisor boots: when its board DT has a resource table for it (hypervisor-pas-rsctable)
 * EL2 does not map its SMMU streams, the OS does, from that table - and the OS only does that for a
 * remoteproc node that has `iommus`.  The OS's stock tree does not, so it gets the streams the board DT
 * lists, with the OS tree's own phandle for the SMMU.
 */
static void adsp_iommus_dt(void *blob)
{
	ofnode node = ofnode_by_compatible(ofnode_null(), "qcom,sm8650-adsp-pas");
	const fdt32_t *src;
	fdt32_t cells[3 * 8];
	int len, off, smmu, i;
	u32 ph;

	if (!ofnode_valid(node) || !ofnode_read_prop(node, "hypervisor-pas-rsctable", NULL))
		return;

	src = ofnode_read_prop(node, "iommus", &len);
	if (!src || len % 12 || len > sizeof(cells)) {
		log_warning("the ADSP's iommus are not three cells each\n");
		return;
	}

	fdt_addr_t smmu_addr;
	off = fdt_node_offset_by_compatible(blob, -1, "qcom,sm8650-adsp-pas");
	/* the apps SMMU: the one at the address the board DT's iommus point to (the GPU's has the same compatible) */
	smmu_addr = ofnode_get_addr(ofnode_get_by_phandle(fdt32_to_cpu(src[0])));
	for (smmu = fdt_node_offset_by_compatible(blob, -1, "qcom,sm8650-smmu-500"); smmu >= 0;
	     smmu = fdt_node_offset_by_compatible(blob, smmu, "qcom,sm8650-smmu-500")) {
		const char *at = strchr(fdt_get_name(blob, smmu, NULL), '@');

		if (at && simple_strtoul(at + 1, NULL, 16) == smmu_addr)
			break;
	}
	if (off < 0 || smmu < 0) {
		log_warning("no ADSP or SMMU node in the OS device tree\n");
		return;
	}

	ph = fdt_get_phandle(blob, smmu);
	if (!ph && (fdt_generate_phandle(blob, &ph) || fdt_setprop_u32(blob, smmu, "phandle", ph))) {
		log_warning("cannot give the SMMU a phandle\n");
		return;
	}

	for (i = 0; i < len / 12; i++) {
		cells[3 * i] = cpu_to_fdt32(ph);
		cells[3 * i + 1] = src[3 * i + 1];
		cells[3 * i + 2] = src[3 * i + 2];
	}
	if (fdt_setprop(blob, off, "iommus", cells, len))
		log_warning("cannot add iommus to the OS's ADSP node\n");
	else
		log_info("the OS's ADSP node maps its own SMMU streams (%d)\n", len / 12);
}

/*
 * The hypervisor votes the QDSS clock on at boot for its SPMI debug bus (hyp/spmi.c) and the AOSS
 * has no use count: the first consumer in the OS that prepares the clock and lets go again (the
 * coresight devices' `apb_pclk`, which the AMBA bus enables just to read an ID) turns it off for
 * everybody, and the next access to the debug bus resets the board.  Nothing on this board uses
 * coresight, so every node whose `clocks` is the AOSS node is disabled in the OS's device tree.
 */
static void qdss_users_dt(void *blob)
{
	ofnode dbg = ofnode_by_compatible(ofnode_null(), "qcom,spmi-pmic-arb-debug");
	int aoss, off, depth = 0, n = 0;
	u32 ph;

	if (!ofnode_valid(dbg) || !ofnode_is_enabled(dbg) || !ofnode_read_prop(dbg, "clocks", NULL))
		return;

	aoss = fdt_node_offset_by_compatible(blob, -1, "qcom,aoss-qmp");
	ph = aoss < 0 ? 0 : fdt_get_phandle(blob, aoss);
	if (!ph)
		return;

	for (off = fdt_next_node(blob, 0, &depth); off >= 0 && depth > 0;
	     off = fdt_next_node(blob, off, &depth)) {
		const fdt32_t *clocks = fdt_getprop(blob, off, "clocks", NULL);

		if (off == aoss || !clocks || fdt32_to_cpu(clocks[0]) != ph)
			continue;
		usb_disable(blob, off);
		n++;
	}
	if (n)
		log_info("%d users of the AOSS QDSS clock disabled in the OS device tree\n", n);
}

int qcom_ft_board_setup(void *blob, struct bd_info *bd)
{
	ofnode node = hyp_usb_node();
	struct arm_smccc_res res;
	struct udevice *dev;
	int off, phy, ret, idx;
	u32 phy_ph = 0, rep_ph = 0;

	/* Without the hypervisor the controller stays the OS's */
	arm_smccc_hvc(HV_GET_ID, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != HV_ID_MAGIC)
		return 0;

	adsp_iommus_dt(blob);
	qdss_users_dt(blob);

	vdev_net_dt(blob);

	if (ofnode_valid(node)) {
		if (!device_find_global_by_ofnode(node, &dev) && device_active(dev))
			device_remove(dev, DM_REMOVE_NORMAL);

		arm_smccc_hvc(HV_USB_TAKEOVER, 0, 0, 0, 0, 0, 0, 0, &res);
		if ((long)res.a0 && (long)res.a0 != -EALREADY) {
			log_warning("hypervisor did not take the USB controller (%ld), leaving it in the device tree\n",
				(long)res.a0);
			return 0;
		}

		vdev_console_dt(blob);
		off = usb_find_in_blob(blob, node);
		if (off < 0) {
			log_warning("%s is not in the OS device tree\n",
				ofnode_get_name(node));
			return 0;
		}

		/*
		* The PHY the controller lists first as "usb2-phy", and the repeater behind
		* it.  Every edit can move the nodes after it, so only phandles are kept
		* across edits and each node is looked up again.
		*/
		idx = fdt_stringlist_search(blob, off, "phy-names", "usb2-phy");
		if (idx == 0) {
			phy_ph = usb_phandle(blob, off, "phys");
			phy = phy_ph ? fdt_node_offset_by_phandle(blob, phy_ph) : -FDT_ERR_NOTFOUND;
			if (phy >= 0)
				rep_ph = usb_phandle(blob, phy, "phys");
		}
		if (rep_ph)
			usb_disable(blob, fdt_node_offset_by_phandle(blob, rep_ph));
		if (phy_ph)
			usb_disable(blob, fdt_node_offset_by_phandle(blob, phy_ph));

		off = usb_find_in_blob(blob, node);
		ret = off < 0 ? off : fdt_del_node(blob, off);
		if (ret)
			log_warning("cannot delete %s from the OS device tree: %s\n",
				ofnode_get_name(node), fdt_strerror(ret));
		else
			log_info("USB controller handed to the hypervisor, %s removed from the OS device tree\n",
				ofnode_get_name(node));
	}

	return 0;
}

void board_quiesce_devices(void)
{
	struct arm_smccc_res res;

	/* Without the hypervisor there is nothing to hand over */
	arm_smccc_hvc(HV_GET_ID, 0, 0, 0, 0, 0, 0, 0, &res);
	if (res.a0 != HV_ID_MAGIC)
		return;

	arm_smccc_hvc(HV_OS_HANDOFF, 0, 0, 0, 0, 0, 0, 0, &res);
	log_info("hypervisor removed %ld of U-Boot's SMMU stream mappings\n", (long)res.a0);
}

