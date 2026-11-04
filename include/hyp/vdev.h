/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * The hypervisor's virtual devices, as the guest sees them (scripts/USB_GADGET.md, "guest
 * proxy"): a virtio-net NIC that is bridged to the USB link, and a PL011 UART that is the
 * guest's console, reachable on the link as a telnet service.  They have no hardware behind
 * them.  Shared by the hypervisor, which answers the guest's accesses to this window and
 * injects the interrupts, and by the board code (arch/arm/mach-snapdragon/hyp-usb.c), which
 * describes them to the OS in the device tree it hands over - nothing about them is in the
 * OS's own device tree source.
 */
#ifndef __HYP_VDEV_H__
#define __HYP_VDEV_H__

/* A 2 MiB-block-aligned hole in the physical map that no real device uses */
#define HYP_VDEV_BASE		0x40000000UL
#define HYP_VDEV_SIZE		0x2000UL

#define HYP_VIRTIO_NET_BASE	(HYP_VDEV_BASE + 0x0)
#define HYP_VIRTIO_NET_SIZE	0x200UL
#define HYP_PL011_BASE		(HYP_VDEV_BASE + 0x1000)
#define HYP_PL011_SIZE		0x1000UL

/*
 * Virtual SPIs: INTIDs the guest's GIC could route but no hardware raises (the HDK's device
 * tree uses SPIs up to 673; the distributor implements 960).  Edge triggered.
 */
#define HYP_VSPI_BASE		932
#define HYP_VSPIS		4
#define HYP_VSPI_NET		(HYP_VSPI_BASE + 0)
#define HYP_VSPI_UART		(HYP_VSPI_BASE + 1)

/* The guest NIC's MAC address; the hypervisor's own is 02:68:79:70:00:02, the host's ...:01 */
#define HYP_GUEST_MAC		{ 0x02, 0x68, 0x79, 0x70, 0x00, 0x03 }

/* The guest's address on the debug link: the hypervisor is .2, the host .1 */
#define HYP_GUEST_IP		"192.168.77.3"

#endif /* __HYP_VDEV_H__ */
