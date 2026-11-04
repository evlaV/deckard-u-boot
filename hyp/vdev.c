// SPDX-License-Identifier: GPL-2.0+
/*
 * The guest's virtual devices (include/hyp/vdev.h, scripts/USB_GADGET.md "guest proxy"):
 *
 *  - a virtio-net NIC (virtio-mmio, version 2, split virtqueues) whose frames are bridged to
 *    the USB link: what the guest sends goes out to the host (or to the hypervisor's own stack,
 *    when it is addressed to it), what the host sends to the guest's MAC - and every broadcast -
 *    is put into the guest's receive buffers.  The guest is 192.168.77.3 on the same /24 as the
 *    host and the hypervisor.
 *  - a PL011 UART, the guest's console (`earlycon=pl011,...`, `console=ttyAMA0`).  Its output
 *    goes to a ring that the telnet service on :2222 drains (and keeps for the first client
 *    when there is none yet); what a client types goes into the UART's receive FIFO.
 *
 * Neither has hardware behind it: the guest's accesses to the window are trapped by stage 2
 * (hyp_mmio_emulate()) and answered here, and the interrupts are virtual SPIs injected into
 * the guest's list registers.  Everything below runs under the controller's lock - the same
 * lock the network stack runs under - so the USB side and the guest side never interleave:
 * the guest's accesses take it in the trap handler, the host's frames already hold it.
 *
 * virtio features offered: VERSION_1 and ACCESS_PLATFORM - the latter makes the guest's
 * driver go through the DMA API, whose cache maintenance (the device is not described as
 * coherent) is what makes the guest's buffers visible to EL2's uncached accesses - and
 * the net device's MAC and STATUS.  Guest memory is reached by physical address (hyp/phys.c):
 * stage 2 is an identity map.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <asm/unaligned.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <stdio.h>
#include <string.h>
#include <hyp/spinlock.h>

#include "hv.h"
#include "net.h"
#if CONFIG_IS_ENABLED(HYP_USB)
#include "dwc3.h"
#endif

/* ------------------------------------------------------------------------- */
/* virtio-mmio                                                                */
/* ------------------------------------------------------------------------- */

#define VIRTIO_MAGIC		0x74726976	/* "virt" */
#define VIRTIO_ID_NET		1
#define VIRTIO_VENDOR		0x68797021	/* "hyp!" */

#define VIRTIO_F_VERSION_1	BIT_ULL(32)
#define VIRTIO_F_ACCESS_PLATFORM BIT_ULL(33)
#define VIRTIO_NET_F_MAC	BIT_ULL(5)
#define VIRTIO_NET_F_STATUS	BIT_ULL(16)
#define VIRTIO_NET_S_LINK_UP	1

#define VIRTIO_INT_VRING	BIT(0)

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2

#define VQ_NUM_MAX		128
#define VQ_MAX_SEGS		16
#define NET_HDR_LEN		12	/* virtio_net_hdr_v1 */
#define FRAME_MAX		1536

/* virtio-mmio registers */
#define VM_MAGIC		0x000
#define VM_VERSION		0x004
#define VM_DEVICE_ID		0x008
#define VM_VENDOR_ID		0x00c
#define VM_DEVICE_FEATURES	0x010
#define VM_DEVICE_FEATURES_SEL	0x014
#define VM_DRIVER_FEATURES	0x020
#define VM_DRIVER_FEATURES_SEL	0x024
#define VM_QUEUE_SEL		0x030
#define VM_QUEUE_NUM_MAX	0x034
#define VM_QUEUE_NUM		0x038
#define VM_QUEUE_READY		0x044
#define VM_QUEUE_NOTIFY		0x050
#define VM_INTERRUPT_STATUS	0x060
#define VM_INTERRUPT_ACK	0x064
#define VM_STATUS		0x070
#define VM_QUEUE_DESC_LOW	0x080
#define VM_QUEUE_DESC_HIGH	0x084
#define VM_QUEUE_DRIVER_LOW	0x090
#define VM_QUEUE_DRIVER_HIGH	0x094
#define VM_QUEUE_DEVICE_LOW	0x0a0
#define VM_QUEUE_DEVICE_HIGH	0x0a4
#define VM_CONFIG_GENERATION	0x0fc
#define VM_CONFIG		0x100

struct vq {
	u32 num;
	bool ready;
	u64 desc, avail, used;		/* guest physical addresses */
	u16 last_avail;			/* next avail entry to take */
	u16 used_idx;			/* our copy of used->idx */
};

struct vdesc {
	u64 addr;
	u32 len;
	u16 flags, next;
};

struct vseg {
	u64 addr;
	u32 len;
};

struct vnet {
	u32 status, features_sel, driver_sel, queue_sel, int_status;
	u64 driver_features;
	struct vq q[2];			/* 0 receive, 1 transmit */
	u8 mac[6];
	u32 rx_frames, tx_frames, rx_dropped, tx_dropped;
	u8 txbuf[FRAME_MAX + NET_HDR_LEN + 64];
};

/* ------------------------------------------------------------------------- */
/* PL011                                                                      */
/* ------------------------------------------------------------------------- */

#define UART_DR		0x000
#define UART_RSR	0x004
#define UART_FR		0x018
#define UART_ILPR	0x020
#define UART_IBRD	0x024
#define UART_FBRD	0x028
#define UART_LCRH	0x02c
#define UART_CR		0x030
#define UART_IFLS	0x034
#define UART_IMSC	0x038
#define UART_RIS	0x03c
#define UART_MIS	0x040
#define UART_ICR	0x044
#define UART_DMACR	0x048
#define UART_PERIPH_ID	0xfe0		/* four bytes, one per word */
#define UART_CELL_ID	0xff0

#define UART_FR_RXFE	BIT(4)
#define UART_FR_TXFE	BIT(7)
#define UART_INT_RX	BIT(4)
#define UART_INT_TX	BIT(5)
#define UART_INT_RT	BIT(6)

#define OUT_RING	(64 * 1024)
#define IN_RING		1024

struct vuart {
	u32 cr, lcrh, ibrd, fbrd, ifls, imsc, ris, ilpr, dmacr;
	u8 out[OUT_RING];		/* console output nobody has taken yet */
	u32 out_head, out_tail;		/* free-running; head - tail <= OUT_RING */
	u8 in[IN_RING];			/* what a client typed, for the guest to read */
	u32 in_head, in_tail;
	struct tcp_conn *conn;		/* the telnet client, when there is one */
	u8 iac;				/* telnet escape state: 0 text, 1 after IAC, 2 option, 3 in SB */
	bool saw_cr;
	u32 bytes_out, bytes_in, dropped;
	bool at_bol;			/* the last byte the guest wrote ended a line (or none yet) */
	u64 last_out_us;		/* when the guest last wrote one */
};

static __hyp_data struct vnet vnet = {
	.mac = HYP_GUEST_MAC,
};
static __hyp_data struct vuart vuart = { .cr = 0x300, .at_bol = true };
/* hyp_vdev_init() has run: before that the guest's exits have no network to tick */
static __hyp_data bool vdev_ready;

/* How the guest is told: the test suite swaps this for a counter */
static __hyp_data void (*vspi_raise)(int intid);

/* ------------------------------------------------------------------------- */
/* Guest memory and virtqueues                                                */
/* ------------------------------------------------------------------------- */

static int __hyp gread(u64 gpa, void *buf, u32 len)
{
	return hyp_phys_read(gpa, buf, len);
}

static int __hyp gwrite(u64 gpa, const void *buf, u32 len)
{
	return hyp_phys_write(gpa, buf, len);
}

static u16 __hyp vq_avail_idx(struct vq *q)
{
	__le16 v = 0;

	gread(q->avail + 2, &v, 2);

	return le16_to_cpu(v);
}

/*
 * The descriptor chain behind the avail entry @last_avail points at: its segments in @segs,
 * the number in *@n, the head in *@head; @writable is what is asked for (receive buffers
 * are device-writable, transmit buffers device-readable).  -EINVAL for a chain that is not
 * what the device takes - nothing is consumed then, the caller drops the entry.
 */
static int __hyp vq_chain(struct vq *q, bool writable, struct vseg *segs, int *n, u16 *head)
{
	__le16 h;
	u16 idx;
	int i = 0;

	if (gread(q->avail + 4 + 2 * (q->last_avail % q->num), &h, 2))
		return -EFAULT;
	idx = *head = le16_to_cpu(h);

	for (;;) {
		struct vdesc d;

		if (idx >= q->num || i == VQ_MAX_SEGS || gread(q->desc + 16 * idx, &d, sizeof(d)))
			return -EINVAL;
		if (!!(le16_to_cpu(d.flags) & VRING_DESC_F_WRITE) != writable)
			return -EINVAL;

		segs[i].addr = le64_to_cpu(d.addr);
		segs[i].len = le32_to_cpu(d.len);
		i++;
		if (!(le16_to_cpu(d.flags) & VRING_DESC_F_NEXT))
			break;
		idx = le16_to_cpu(d.next);
	}

	*n = i;

	return 0;
}

/* The avail entry has been served: give the chain back with @len bytes written */
static void __hyp vq_done(struct vq *q, u16 head, u32 len)
{
	struct { __le32 id, len; } e = { cpu_to_le32(head), cpu_to_le32(len) };
	__le16 idx;

	gwrite(q->used + 4 + 8 * (q->used_idx % q->num), &e, sizeof(e));
	q->used_idx++;
	idx = cpu_to_le16(q->used_idx);
	gwrite(q->used + 2, &idx, 2);
	q->last_avail++;
}

/* A device-readable chain, gathered into @buf; the bytes, or -EMSGSIZE */
static int __hyp seg_gather(const struct vseg *segs, int n, u8 *buf, u32 size)
{
	u32 total = 0;

	for (int i = 0; i < n; i++) {
		if (total + segs[i].len > size)
			return -EMSGSIZE;
		if (gread(segs[i].addr, buf + total, segs[i].len))
			return -EFAULT;
		total += segs[i].len;
	}

	return total;
}

static void __hyp raise(int intid)
{
	if (vspi_raise)
		vspi_raise(intid);
	else
		hyp_gic_vspi_raise(intid);
}

/* ------------------------------------------------------------------------- */
/* virtio-net                                                                 */
/* ------------------------------------------------------------------------- */

static void __hyp vnet_interrupt(void)
{
	vnet.int_status |= VIRTIO_INT_VRING;
	raise(HYP_VSPI_NET);
}

static void __hyp vnet_reset(void)
{
	memset(vnet.q, 0, sizeof(vnet.q));
	vnet.status = vnet.int_status = 0;
	vnet.driver_features = 0;
}

/*
 * A frame for the guest, from the link.  False when the guest has no receive buffer to take it
 * (the frame is lost, as on a NIC that is out of descriptors).
 */
static bool __hyp vnet_deliver(const u8 *frame, u32 len)
{
	struct vq *q = &vnet.q[0];
	struct vseg segs[VQ_MAX_SEGS];
	u8 hdr[NET_HDR_LEN] = { 0 };
	u32 cap = 0, off = 0, done = 0;
	int n, i;
	u16 head;

	if (!q->ready || len > FRAME_MAX || !(vnet.status & 4) /* DRIVER_OK */ ||
	    vq_avail_idx(q) == q->last_avail ||
	    vq_chain(q, true, segs, &n, &head)) {
		vnet.rx_dropped++;
		return false;
	}

	for (i = 0; i < n; i++)
		cap += segs[i].len;
	if (cap < NET_HDR_LEN + len) {
		vnet.rx_dropped++;
		vq_done(q, head, 0);
		return false;
	}

	put_unaligned_le16(1, hdr + 10);		/* num_buffers */
	for (i = 0; i < n && done < NET_HDR_LEN + len; i++) {
		u32 take = min(segs[i].len, NET_HDR_LEN + len - done), o = 0;

		if (done < NET_HDR_LEN) {
			u32 h = min(take, NET_HDR_LEN - done);

			gwrite(segs[i].addr, hdr + done, h);
			o = h;
			done += h;
			off = 0;
		}
		if (take > o) {
			gwrite(segs[i].addr + o, frame + off, take - o);
			off += take - o;
			done += take - o;
		}
	}

	vq_done(q, head, NET_HDR_LEN + len);
	vnet.rx_frames++;
	vnet_interrupt();

	return true;
}

/*
 * The guest's transmit queue: each chain is a virtio_net_hdr and a frame.  A frame the link
 * cannot take yet (the USB function is busy) stays in the queue, and this runs again when the
 * previous frame has left (hyp_vdev_tx_resume()): the guest is held back instead of dropped.
 */
static void __hyp vnet_tx_pump(void)
{
	struct vq *q = &vnet.q[1];
	bool did = false;

	while (q->ready && (vnet.status & 4) && vq_avail_idx(q) != q->last_avail) {
		struct vseg segs[VQ_MAX_SEGS];
		int n, total;
		u16 head;

		if (vq_chain(q, false, segs, &n, &head)) {
			/* not a chain a NIC takes: skip the entry */
			vnet.tx_dropped++;
			q->last_avail++;
			continue;
		}

		total = seg_gather(segs, n, vnet.txbuf, sizeof(vnet.txbuf));
		if (total >= NET_HDR_LEN + 14 && total <= NET_HDR_LEN + FRAME_MAX) {
			if (hyp_net_from_guest(vnet.txbuf + NET_HDR_LEN, total - NET_HDR_LEN) == -EBUSY)
				break;
			vnet.tx_frames++;
		} else {
			vnet.tx_dropped++;
		}

		vq_done(q, head, 0);
		did = true;
	}

	if (did)
		vnet_interrupt();
}

static bool __hyp vnet_mmio(u32 off, bool store, u32 size, u64 *val)
{
	struct vq *q = &vnet.q[vnet.queue_sel & 1];
	u32 v = *val;

	if (off >= VM_CONFIG) {
		/* config space: the MAC, then the 16-bit link status */
		u8 cfg[8];

		memcpy(cfg, vnet.mac, 6);
		put_unaligned_le16(VIRTIO_NET_S_LINK_UP, cfg + 6);
		if (store || off - VM_CONFIG + size > sizeof(cfg))
			return false;
		*val = 0;
		for (u32 i = 0; i < size; i++)
			*val |= (u64)cfg[off - VM_CONFIG + i] << (8 * i);

		return true;
	}

	if (size != 4)
		return false;

	if (!store) {
		switch (off) {
		case VM_MAGIC:
			v = VIRTIO_MAGIC;
			break;
		case VM_VERSION:
			v = 2;
			break;
		case VM_DEVICE_ID:
			v = VIRTIO_ID_NET;
			break;
		case VM_VENDOR_ID:
			v = VIRTIO_VENDOR;
			break;
		case VM_DEVICE_FEATURES: {
			u64 f = VIRTIO_F_VERSION_1 | VIRTIO_F_ACCESS_PLATFORM | VIRTIO_NET_F_MAC |
				VIRTIO_NET_F_STATUS;

			v = vnet.features_sel ? f >> 32 : (u32)f;
			break;
		}
		case VM_QUEUE_NUM_MAX:
			v = vnet.queue_sel < 2 ? VQ_NUM_MAX : 0;
			break;
		case VM_QUEUE_READY:
			v = q->ready;
			break;
		case VM_INTERRUPT_STATUS:
			v = vnet.int_status;
			break;
		case VM_STATUS:
			v = vnet.status;
			break;
		case VM_CONFIG_GENERATION:
			v = 0;
			break;
		default:
			v = 0;
		}
		*val = v;

		return true;
	}

	switch (off) {
	case VM_DEVICE_FEATURES_SEL:
		vnet.features_sel = v;
		break;
	case VM_DRIVER_FEATURES_SEL:
		vnet.driver_sel = v;
		break;
	case VM_DRIVER_FEATURES:
		if (vnet.driver_sel)
			vnet.driver_features = (vnet.driver_features & 0xffffffffULL) | (u64)v << 32;
		else
			vnet.driver_features = (vnet.driver_features & ~0xffffffffULL) | v;
		break;
	case VM_QUEUE_SEL:
		vnet.queue_sel = v;
		break;
	case VM_QUEUE_NUM:
		q->num = min_t(u32, v, VQ_NUM_MAX);
		break;
	case VM_QUEUE_READY:
		if (vnet.queue_sel < 2) {
			q->ready = v & 1 && q->num;
			if (q->ready) {
				__le16 idx = 0;

				q->last_avail = 0;
				gread(q->used + 2, &idx, 2);
				q->used_idx = le16_to_cpu(idx);
			}
		}
		break;
	case VM_QUEUE_NOTIFY:
		if (v == 1)
			vnet_tx_pump();
		break;
	case VM_INTERRUPT_ACK:
		vnet.int_status &= ~v;
		break;
	case VM_STATUS:
		if (!v)
			vnet_reset();
		else
			vnet.status = v;
		break;
	case VM_QUEUE_DESC_LOW:
		q->desc = (q->desc & ~0xffffffffULL) | v;
		break;
	case VM_QUEUE_DESC_HIGH:
		q->desc = (q->desc & 0xffffffffULL) | (u64)v << 32;
		break;
	case VM_QUEUE_DRIVER_LOW:
		q->avail = (q->avail & ~0xffffffffULL) | v;
		break;
	case VM_QUEUE_DRIVER_HIGH:
		q->avail = (q->avail & 0xffffffffULL) | (u64)v << 32;
		break;
	case VM_QUEUE_DEVICE_LOW:
		q->used = (q->used & ~0xffffffffULL) | v;
		break;
	case VM_QUEUE_DEVICE_HIGH:
		q->used = (q->used & 0xffffffffULL) | (u64)v << 32;
		break;
	}

	return true;
}

/* ------------------------------------------------------------------------- */
/* The bridge, as net.c sees it                                               */
/* ------------------------------------------------------------------------- */

static bool __hyp is_group(const u8 *mac)
{
	return mac[0] & 1;		/* broadcast and multicast */
}

/**
 * hyp_vdev_net_rx() - a frame arrived from the host: does the guest want it?
 *
 * Return: true when it was for the guest alone (nothing else is to look at it); a broadcast
 * or multicast frame is also given to the guest but still has to be handled by the stack.
 */
bool __hyp hyp_vdev_net_rx(const u8 *frame, u32 len)
{
	if (!memcmp(frame, vnet.mac, 6)) {
		vnet_deliver(frame, len);

		return true;
	}

	if (is_group(frame))
		vnet_deliver(frame, len);

	return false;
}

/**
 * hyp_vdev_net_from_stack() - the hypervisor's stack sends a frame: is it for the guest?
 *
 * Return: true when it was delivered to the guest and is not to go out on the link.
 */
bool __hyp hyp_vdev_net_from_stack(const u8 *frame, u32 len)
{
	if (memcmp(frame, vnet.mac, 6))
		return false;

	vnet_deliver(frame, len);

	return true;
}

/* The link is free again: the guest's held-back frames can go */
void __hyp hyp_vdev_tx_resume(void)
{
	vnet_tx_pump();
}

/* ------------------------------------------------------------------------- */
/* PL011 and the console service                                              */
/* ------------------------------------------------------------------------- */

static void __hyp uart_update_irq(void)
{
	if (vuart.imsc & vuart.ris)
		raise(HYP_VSPI_UART);
}

static bool __hyp uart_rx_empty(void)
{
	return vuart.in_head == vuart.in_tail;
}

static void __hyp out_put(u8 c)
{
	if (vuart.out_head - vuart.out_tail == OUT_RING)
		vuart.out_tail++;			/* nobody is reading: the oldest goes */
	vuart.out[vuart.out_head++ % OUT_RING] = c;
}

/*
 * The hypervisor's own log lines (hyp/log.c) go out with the guest's console output: whole
 * lines, between two of the guest's.  A guest line that is still open holds them back, for at
 * most HLOG_HOLD_US (the guest has stopped in the middle of a line, or never ends one).
 */
#define HLOG_HOLD_US	50000

static void __hyp console_merge_log(void)
{
	char buf[256];
	u32 n, lost;

	if (!vuart.at_bol && hyp_ticks_us() - vuart.last_out_us < HLOG_HOLD_US)
		return;

	while ((n = hyp_log_take(buf, sizeof(buf)))) {
		if (!vuart.at_bol) {
			out_put('\r');
			out_put('\n');
			vuart.at_bol = true;
		}
		for (u32 i = 0; i < n; i++) {
			if (buf[i] == '\n')
				out_put('\r');
			out_put(buf[i]);
		}
	}

	lost = hyp_log_lost();
	if (lost) {
		static const char msg[] = "[HYP] log: lines lost: the ring was full\r\n";

		for (u32 i = 0; i < sizeof(msg) - 1; i++)
			out_put(msg[i]);
	}
}

/* Output the telnet client has not taken: to it, as far as its window goes */
static void __hyp console_pump(void)
{
	struct tcp_conn *c = vuart.conn;

	console_merge_log();

	while (c && vuart.out_tail != vuart.out_head) {
		u32 room = hyp_tcp_room(c), tail = vuart.out_tail % OUT_RING;
		u32 n = min3(vuart.out_head - vuart.out_tail, OUT_RING - tail, room);

		if (!n)
			return;
		n = hyp_tcp_write(c, vuart.out + tail, n);
		vuart.out_tail += n;
	}
}

static void __hyp uart_putc(u8 c)
{
	out_put(c);
	vuart.bytes_out++;
	vuart.at_bol = c == '\n';
	vuart.last_out_us = hyp_ticks_us();

	if (c == '\n' || vuart.out_head - vuart.out_tail >= 512)
		console_pump();
}

static u32 __hyp uart_fr(void)
{
	return UART_FR_TXFE | (uart_rx_empty() ? UART_FR_RXFE : 0);
}

static bool __hyp uart_mmio(u32 off, bool store, u32 size, u64 *val)
{
	u32 v = *val;

	if (store) {
		switch (off) {
		case UART_DR:
			uart_putc(v);
			vuart.ris |= UART_INT_TX;	/* the FIFO drains at once */
			uart_update_irq();
			break;
		case UART_ILPR:
			vuart.ilpr = v;
			break;
		case UART_IBRD:
			vuart.ibrd = v;
			break;
		case UART_FBRD:
			vuart.fbrd = v;
			break;
		case UART_LCRH:
			vuart.lcrh = v;
			break;
		case UART_CR:
			vuart.cr = v;
			break;
		case UART_IFLS:
			vuart.ifls = v;
			break;
		case UART_IMSC:
			vuart.imsc = v;
			uart_update_irq();
			break;
		case UART_ICR:
			vuart.ris &= ~v;
			if (!uart_rx_empty())
				vuart.ris |= UART_INT_RX;	/* level: still data */
			break;
		case UART_DMACR:
			vuart.dmacr = v;
			break;
		}

		return true;
	}

	switch (off) {
	case UART_DR:
		v = 0;
		if (!uart_rx_empty()) {
			v = vuart.in[vuart.in_tail++ % IN_RING];
			if (uart_rx_empty())
				vuart.ris &= ~(UART_INT_RX | UART_INT_RT);
		}
		break;
	case UART_FR:
		v = uart_fr();
		break;
	case UART_ILPR:
		v = vuart.ilpr;
		break;
	case UART_IBRD:
		v = vuart.ibrd;
		break;
	case UART_FBRD:
		v = vuart.fbrd;
		break;
	case UART_LCRH:
		v = vuart.lcrh;
		break;
	case UART_CR:
		v = vuart.cr;
		break;
	case UART_IFLS:
		v = vuart.ifls;
		break;
	case UART_IMSC:
		v = vuart.imsc;
		break;
	case UART_RIS:
		v = vuart.ris;
		break;
	case UART_MIS:
		v = vuart.ris & vuart.imsc;
		break;
	case UART_DMACR:
		v = vuart.dmacr;
		break;
	case UART_PERIPH_ID:
		v = 0x11;
		break;
	case UART_PERIPH_ID + 4:
		v = 0x10;
		break;
	case UART_PERIPH_ID + 8:
		v = 0x14;			/* revision 1: 16-entry FIFOs */
		break;
	case UART_PERIPH_ID + 12:
		v = 0x00;
		break;
	case UART_CELL_ID:
		v = 0x0d;
		break;
	case UART_CELL_ID + 4:
		v = 0xf0;
		break;
	case UART_CELL_ID + 8:
		v = 0x05;
		break;
	case UART_CELL_ID + 12:
		v = 0xb1;
		break;
	default:
		v = 0;
	}
	*val = v;

	return true;
}

/* What the telnet client typed: past its escapes, into the receive FIFO */
static void __hyp console_input(u8 b)
{
	if (vuart.in_head - vuart.in_tail == IN_RING)
		return;					/* the guest is not reading */
	vuart.in[vuart.in_head++ % IN_RING] = b;
	vuart.bytes_in++;
}

#define IAC	255
#define SB	250

static void __hyp console_data(struct tcp_conn *c, const u8 *data, u32 len)
{
	for (u32 i = 0; i < len; i++) {
		u8 b = data[i];

		switch (vuart.iac) {
		case 1:
			vuart.iac = b >= 251 && b <= 254 ? 2 : b == SB ? 3 : 0;
			continue;
		case 2:
			vuart.iac = 0;
			continue;
		case 3:
			if (b == IAC)
				vuart.iac = 1;
			continue;
		}

		if (b == IAC) {
			vuart.iac = 1;
			continue;
		}
		/* CR LF and CR NUL are one Enter */
		if (vuart.saw_cr && (b == '\n' || b == 0)) {
			vuart.saw_cr = false;
			continue;
		}
		vuart.saw_cr = b == '\r';
		console_input(b);
	}

	if (!uart_rx_empty()) {
		vuart.ris |= UART_INT_RX | UART_INT_RT;
		uart_update_irq();
	}
}

static void __hyp console_connect(struct tcp_conn *c)
{
	static const u8 __hyp_rodata negotiate[] = {
		IAC, 251, 1,		/* WILL ECHO: the guest's tty echoes */
		IAC, 251, 3,		/* WILL SUPPRESS-GO-AHEAD: character mode */
	};

	vuart.conn = c;
	vuart.iac = 0;
	vuart.saw_cr = false;
	hyp_tcp_write(c, negotiate, sizeof(negotiate));
	console_pump();
	hprintf("vdev: console client connected\n");
}

static void __hyp console_close(struct tcp_conn *c)
{
	vuart.conn = NULL;
	hprintf("vdev: console client gone\n");
}

static const struct tcp_service __hyp_rodata console_service = {
	.port = 2222,
	.name = "guest-uart",
	.description = "the guest's console (PL011, ttyAMA0); output is kept until a client connects",
	.on_connect = console_connect,
	.on_data = console_data,
	.on_close = console_close,
};

/*
 * What the network stack, the virtual devices and the guest's accesses to them share is
 * guarded by one lock, recursive for the core that holds it: the function layer (hyp/ncm.c)
 * calls into the stack from inside the controller's event handler.  With the USB link that
 * is the controller's lock; without it, a lock of our own.
 */
#if CONFIG_IS_ENABLED(HYP_USB)
static unsigned long __hyp vnet_lock(void)
{
	return hyp_dwc3_lock();
}

static void __hyp vnet_unlock(unsigned long flags)
{
	hyp_dwc3_unlock(flags);
}
#else
static __hyp_data struct {
	struct spinlock lock;
	u32 owner;			/* vCPU slot + 1, 0 when free */
	int depth;
} vlock = { .lock = { 0, 0, "vnet" } };

static unsigned long __hyp vnet_lock(void)
{
	unsigned long flags;
	int n = hyp_cpu_num();
	u32 me = (n < 0 ? 0xff : n) + 1;	/* a core with no context yet is slot 0xff */

	local_irq_save(flags);
	if (READ_ONCE(vlock.owner) == me) {
		vlock.depth++;
		return flags;
	}
	spin_lock(&vlock.lock);
	vlock.owner = me;
	vlock.depth = 1;

	return flags;
}

static void __hyp vnet_unlock(unsigned long flags)
{
	if (--vlock.depth == 0) {
		vlock.owner = 0;
		spin_unlock(&vlock.lock);
	}
	local_irq_restore(flags);
}

/*
 * A guest exit with no USB controller to poll: the stack's timers and the console's output.
 * The guest's NIC is the only link, so the TCP services (telnet, the console) are reached
 * from the guest only while it runs.
 */
void __hyp hyp_vdev_poll(void)
{
	unsigned long flags;

	if (!READ_ONCE(vdev_ready))
		return;

	flags = vnet_lock();

	hyp_net_tick();
	vnet_unlock(flags);
}
#endif

/* ------------------------------------------------------------------------- */
/* The window                                                                 */
/* ------------------------------------------------------------------------- */

static bool __hyp vdev_mmio(int cpu, u64 pa, bool store, u32 size, u64 *val)
{
	unsigned long flags = vnet_lock();
	bool ret = false;

	if (pa >= HYP_VIRTIO_NET_BASE && pa < HYP_VIRTIO_NET_BASE + HYP_VIRTIO_NET_SIZE)
		ret = vnet_mmio(pa - HYP_VIRTIO_NET_BASE, store, size, val);
	else if (pa >= HYP_PL011_BASE && pa < HYP_PL011_BASE + HYP_PL011_SIZE)
		ret = uart_mmio(pa - HYP_PL011_BASE, store, size, val);
	vnet_unlock(flags);

	return ret;
}

/* Called often (every controller poll, every guest exit): the console's output goes out */
void __hyp hyp_vdev_tick(void)
{
	console_pump();
}

int __hyp hyp_vdev_describe(char *buf, int size)
{
	return snprintf(buf, size,
			"vdev: net %u frames to the guest (%u lost), %u from it (%u dropped); "
			"uart %u bytes out (%u waiting), %u in",
			vnet.rx_frames, vnet.rx_dropped, vnet.tx_frames, vnet.tx_dropped,
			vuart.bytes_out, vuart.out_head - vuart.out_tail, vuart.bytes_in);
}

/* The self-tests' view of the devices: the MMIO handler and a replacement interrupt line */
bool __hyp hyp_vdev_test_access(u64 pa, bool store, u32 size, u64 *val)
{
	return vdev_mmio(0, pa, store, size, val);
}

void __hyp hyp_vdev_test_irq(void (*fn)(int intid))
{
	vspi_raise = fn;
}

/* The console's output and input as plain buffers, for the self-tests: the guest's bytes only,
 * so the hypervisor's own log lines that are waiting are thrown away first */
u32 __hyp hyp_vdev_test_console(u8 *out, u32 size, const u8 *in, u32 in_len)
{
	char drop[256];
	u32 n;

	while (hyp_log_take(drop, sizeof(drop)))
		;
	n = min(vuart.out_head - vuart.out_tail, size);

	for (u32 i = 0; i < n; i++)
		out[i] = vuart.out[(vuart.out_tail + i) % OUT_RING];
	vuart.out_tail += n;
	if (in)
		console_data(NULL, in, in_len);

	return n;
}

void __hyp hyp_vdev_init(void)
{
	hyp_mmio_emulate(HYP_VDEV_BASE, HYP_VDEV_BASE + HYP_VDEV_SIZE, vdev_mmio);
	hyp_tcp_listen(&console_service);
	hprintf("vdev: virtio-net at %#lx (MAC %pM), PL011 at %#lx, virtual SPIs %d/%d\n",
		(unsigned long)HYP_VIRTIO_NET_BASE, vnet.mac, (unsigned long)HYP_PL011_BASE,
		HYP_VSPI_NET - 32, HYP_VSPI_UART - 32);
	WRITE_ONCE(vdev_ready, true);
}
