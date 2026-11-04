// SPDX-License-Identifier: GPL-2.0+
/*
 * The USB device EL2 presents: one CDC NCM network function, high speed
 * (scripts/USB_GADGET.md).  NTB16 framing without CRC, one datagram per NTB on
 * transmit, any number on receive.  Chapter 9 requests, the NCM class
 * requests, descriptors and the interrupt-endpoint notifications live here; the
 * controller is hyp/dwc3.c and the network behind the frames is hyp/net.c.
 *
 * Reference: linux/drivers/usb/gadget/function/f_ncm.c and the USB CDC NCM 1.0
 * specification.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>

#include "hv.h"
#include "dwc3.h"
#include "net.h"

#define EP_NOTIFY		DWC3_EP(2, 1)	/* 0x82 */
#define EP_BULK_IN		DWC3_EP(1, 1)	/* 0x81 */
#define EP_BULK_OUT		DWC3_EP(1, 0)	/* 0x01 */

#define HS_BULK_MAXP		512
#define NOTIFY_MAXP		16
#define NTB_MAX_SIZE		4096

/* Descriptor types */
#define DT_DEVICE		1
#define DT_CONFIG		2
#define DT_STRING		3
#define DT_DEVICE_QUALIFIER	6

/* CDC NCM class requests */
#define NCM_GET_NTB_PARAMETERS	0x80
#define NCM_GET_NTB_FORMAT	0x83
#define NCM_SET_NTB_FORMAT	0x84
#define NCM_GET_NTB_INPUT_SIZE	0x85
#define NCM_SET_NTB_INPUT_SIZE	0x86
#define NCM_GET_CRC_MODE	0x89
#define NCM_SET_CRC_MODE	0x8a
#define CDC_SET_ETHERNET_PACKET_FILTER	0x43

/* CDC notifications */
#define NCM_NOTIFY_NETWORK_CONNECTION	0x00
#define NCM_NOTIFY_SPEED_CHANGE		0x2a

#define NTH16_SIGN		0x484d434e	/* "NCMH" */
#define NDP16_SIGN		0x304d434e	/* "NCM0" */
#define NTH16_LEN		12
#define NDP16_LEN		16		/* header 8 + one datagram + terminator */
#define NTB_HDR_LEN		(NTH16_LEN + NDP16_LEN)

struct ncm_ntb_parameters {
	__le16 wLength;
	__le16 bmNtbFormatsSupported;
	__le32 dwNtbInMaxSize;
	__le16 wNdpInDivisor;
	__le16 wNdpInPayloadRemainder;
	__le16 wNdpInAlignment;
	__le16 wPadding1;
	__le32 dwNtbOutMaxSize;
	__le16 wNdpOutDivisor;
	__le16 wNdpOutPayloadRemainder;
	__le16 wNdpOutAlignment;
	__le16 wNtbOutMaxDatagrams;
} __packed;

static const struct ncm_ntb_parameters __hyp_rodata ntb_parameters = {
	.wLength = cpu_to_le16(sizeof(struct ncm_ntb_parameters)),
	.bmNtbFormatsSupported = cpu_to_le16(1),		/* NTB16 */
	.dwNtbInMaxSize = cpu_to_le32(NTB_MAX_SIZE),
	.wNdpInDivisor = cpu_to_le16(4),
	.wNdpInPayloadRemainder = cpu_to_le16(0),
	.wNdpInAlignment = cpu_to_le16(4),
	.dwNtbOutMaxSize = cpu_to_le32(NTB_MAX_SIZE),
	.wNdpOutDivisor = cpu_to_le16(4),
	.wNdpOutPayloadRemainder = cpu_to_le16(0),
	.wNdpOutAlignment = cpu_to_le16(4),
	.wNtbOutMaxDatagrams = cpu_to_le16(32),
};

/* ------------------------------------------------------------------------- */
/* Descriptors                                                                */
/* ------------------------------------------------------------------------- */

static const u8 __hyp_rodata device_desc[18] = {
	18, DT_DEVICE,
	0x00, 0x02,		/* bcdUSB 2.00 */
	0xef, 0x02, 0x01,	/* miscellaneous / common class / interface association */
	64,			/* bMaxPacketSize0 */
	0x09, 0x12,		/* idVendor 0x1209 (pid.codes) */
	0x01, 0x00,		/* idProduct 0x0001 (test) */
	0x00, 0x01,		/* bcdDevice 1.00 */
	1, 2, 3,		/* manufacturer, product, serial strings */
	1,			/* one configuration */
};

static const u8 __hyp_rodata qualifier_desc[10] = {
	10, DT_DEVICE_QUALIFIER, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 1, 0,
};

#define MAC_STR_INDEX		4

static const u8 __hyp_rodata config_desc[] = {
	/* configuration */
	9, DT_CONFIG, 94, 0 /* wTotalLength */, 2, 1, 0, 0xc0, 0,
	/* interface association: the two interfaces are one NCM function */
	8, 0x0b, 0, 2, 0x02, 0x0d, 0x00, 0,
	/* interface 0: communications, NCM, one interrupt endpoint */
	9, 4, 0, 0, 1, 0x02, 0x0d, 0x00, 0,
	5, 0x24, 0x00, 0x10, 0x01,			/* CDC header, bcdCDC 1.10 */
	5, 0x24, 0x06, 0, 1,				/* union: master 0, slave 1 */
	13, 0x24, 0x0f, MAC_STR_INDEX, 0, 0, 0, 0,	/* ethernet: iMACAddress, no statistics */
	0xea, 0x05,					/* wMaxSegmentSize 1514 */
	0, 0, 0,					/* no multicast filters, no power filters */
	6, 0x24, 0x1a, 0x00, 0x01, 0x00,		/* NCM 1.00, no capabilities */
	7, 5, 0x82, 0x03, NOTIFY_MAXP, 0, 9,		/* interrupt IN ep2, interval 32 ms */
	/* interface 1, alt 0: data, no endpoints */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x01, 0,
	/* interface 1, alt 1: data, bulk in and out */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x01, 0,
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,		/* bulk IN ep1, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,		/* bulk OUT ep1, 512 */
};

/* String descriptors, UTF-16LE, built in ncm_string() */
static const char __hyp_rodata * const strings[] = {
	NULL,
	"u-boot-hyp",
	"SM8650 hypervisor debug link",
	"0001",
	"026879700001",		/* the host's end of the link: 02:68:79:70:00:01 */
};

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

struct ncm_state {
	bool ready;
	u8 config;		/* current configuration, 0 = none */
	u8 data_alt;		/* alt setting of the data interface */
	u32 ntb_in_size;	/* what the host set with SET_NTB_INPUT_SIZE */
	u16 tx_seq;
	u8 *rx;			/* NTB_MAX_SIZE */
	u8 *tx;			/* NTB_MAX_SIZE */
	u8 *notify;		/* two notifications back to back */
	u8 ctrl[64];		/* answer to a control request */
	bool notify_pending;
	u32 rx_ntbs, tx_ntbs, tx_drops;
};

static __hyp_data struct ncm_state ncm = { .ntb_in_size = NTB_MAX_SIZE };

static void __hyp rx_arm(void)
{
	if (ncm.data_alt == 1 && !hyp_dwc3_ep_busy(EP_BULK_OUT))
		hyp_dwc3_ep_submit(EP_BULK_OUT, ncm.rx, NTB_MAX_SIZE);
}

/* ------------------------------------------------------------------------- */
/* Notifications                                                              */
/* ------------------------------------------------------------------------- */

/* NETWORK_CONNECTION (connected) then CONNECTION_SPEED_CHANGE (480 Mbit/s) */
static void __hyp notify_connected(void)
{
	u8 *n = ncm.notify;
	u32 rate = cpu_to_le32(425984000);

	if (hyp_dwc3_ep_busy(EP_NOTIFY)) {
		ncm.notify_pending = true;
		return;
	}

	memset(n, 0, 32);
	n[0] = 0xa1;
	n[1] = NCM_NOTIFY_NETWORK_CONNECTION;
	n[2] = 1;			/* wValue: connected */
	/* wIndex = interface 0, wLength = 0 */

	n[8] = 0xa1;
	n[9] = NCM_NOTIFY_SPEED_CHANGE;
	n[14] = 8;			/* wLength */
	memcpy(n + 16, &rate, 4);	/* DLBitRate, ULBitRate */
	memcpy(n + 20, &rate, 4);

	hyp_dwc3_ep_submit(EP_NOTIFY, n, 8);
	ncm.notify_pending = true;	/* the speed notification follows the first one */
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

static int __hyp ncm_string(u8 index, u16 maxlen, const void **data, u16 *len)
{
	u8 *b = ncm.ctrl;
	const char *s;
	u16 i, n;

	if (index == 0) {
		b[0] = 4;
		b[1] = DT_STRING;
		b[2] = 0x09;		/* English (US) */
		b[3] = 0x04;
		*data = b;
		*len = 4;
		return 0;
	}

	if (index >= ARRAY_SIZE(strings))
		return -EINVAL;

	s = strings[index];
	n = min_t(u16, strlen(s), (sizeof(ncm.ctrl) - 2) / 2);
	b[0] = 2 + 2 * n;
	b[1] = DT_STRING;
	for (i = 0; i < n; i++) {
		b[2 + 2 * i] = s[i];
		b[3 + 2 * i] = 0;
	}
	*data = b;
	*len = b[0];

	return 0;
}

static int __hyp ncm_get_descriptor(const struct usb_ctrl_req *r, const void **data, u16 *len)
{
	u8 type = le16_to_cpu(r->wValue) >> 8, index = le16_to_cpu(r->wValue) & 0xff;

	switch (type) {
	case DT_DEVICE:
		*data = device_desc;
		*len = sizeof(device_desc);
		return 0;
	case DT_DEVICE_QUALIFIER:
		*data = qualifier_desc;
		*len = sizeof(qualifier_desc);
		return 0;
	case DT_CONFIG:
		if (index)
			return -EINVAL;
		/* wTotalLength is fixed in config_desc; the controller layer sends what the host asked for */
		*data = config_desc;
		*len = sizeof(config_desc);
		return 0;
	case DT_STRING:
		return ncm_string(index, le16_to_cpu(r->wLength), data, len);
	}

	return -EINVAL;
}

/*
 * The endpoints are enabled together at SET_CONFIGURATION, behind one DEPSTARTCFG (what
 * U-Boot's gadget functions do).  The host only talks to the bulk pair once it selects
 * alternate setting 1 of the data interface; until then it sends no tokens for them.
 */
static void __hyp ncm_enable_endpoints(void)
{
	hyp_dwc3_ep_enable(EP_NOTIFY, USB_EP_XFER_INT, NOTIFY_MAXP, 9);
	hyp_dwc3_ep_enable(EP_BULK_IN, USB_EP_XFER_BULK, HS_BULK_MAXP, 0);
	hyp_dwc3_ep_enable(EP_BULK_OUT, USB_EP_XFER_BULK, HS_BULK_MAXP, 0);
}

static void __hyp ncm_set_alt(u8 alt)
{
	if (alt == ncm.data_alt)
		return;

	ncm.data_alt = alt;

	if (alt == 1) {
		rx_arm();
		notify_connected();
		hyp_net_link(true);
		hprintf("ncm: data interface up, UART disabled.\n");
		/* writing the UART is synchronous and slow, and the net console has the whole log in its ring */
		hyp_log_uart_set(false);
	} else {
		hyp_net_link(false);
		hyp_log_uart_set(true);
		hprintf("ncm: data interface down, the log goes to the UART again\n");
	}
}

static int __hyp ncm_class_request(const struct usb_ctrl_req *r, const void **data, u16 *len)
{
	u16 wlen = le16_to_cpu(r->wLength);
	u8 *b = ncm.ctrl;

	switch (r->bRequest) {
	case NCM_GET_NTB_PARAMETERS:
		*data = &ntb_parameters;
		*len = sizeof(ntb_parameters);
		return 0;
	case NCM_GET_NTB_FORMAT:
	case NCM_GET_CRC_MODE:
		memset(b, 0, 2);			/* NTB16, no CRC */
		*data = b;
		*len = 2;
		return 0;
	case NCM_SET_NTB_FORMAT:
	case NCM_SET_CRC_MODE:
		return le16_to_cpu(r->wValue) == 0 ? 0 : -EINVAL;
	case NCM_GET_NTB_INPUT_SIZE:
		put_unaligned_le32(ncm.ntb_in_size, b);
		*data = b;
		*len = 4;
		return 0;
	case NCM_SET_NTB_INPUT_SIZE:
		if (wlen != 4)
			return -EINVAL;
		*len = wlen;
		return 1;				/* the size arrives in the data stage */
	case CDC_SET_ETHERNET_PACKET_FILTER:
		return 0;				/* the link accepts everything */
	}

	return -EINVAL;
}

int __hyp hyp_usb_fn_setup(const struct usb_ctrl_req *r, const void **data, u16 *len)
{
	u8 type = r->bRequestType & USB_TYPE_MASK, recip = r->bRequestType & USB_RECIP_MASK;
	u16 value = le16_to_cpu(r->wValue), index = le16_to_cpu(r->wIndex);
	u8 *b = ncm.ctrl;

	*len = 0;

	if (type == USB_TYPE_CLASS) {
		if (recip != 1 /* interface */ || (index & 0xff) != 0)
			return -EINVAL;
		return ncm_class_request(r, data, len);
	}

	if (type != USB_TYPE_STANDARD)
		return -EINVAL;

	switch (r->bRequest) {
	case USB_REQ_GET_DESCRIPTOR:
		if (!(r->bRequestType & USB_DIR_IN))
			return -EINVAL;
		return ncm_get_descriptor(r, data, len);

	case USB_REQ_SET_CONFIG:
		if (value > 1)
			return -EINVAL;
		if (value == 1 && !ncm.config) {
			ncm_enable_endpoints();
			ncm.config = 1;
		} else if (value == 0 && ncm.config) {
			ncm_set_alt(0);
			hyp_dwc3_ep_disable(EP_NOTIFY);
			hyp_dwc3_ep_disable(EP_BULK_IN);
			hyp_dwc3_ep_disable(EP_BULK_OUT);
			ncm.config = 0;
		}
		return 0;

	case USB_REQ_GET_CONFIG:
		b[0] = ncm.config;
		*data = b;
		*len = 1;
		return 0;

	case USB_REQ_SET_INTERFACE:
		if (!ncm.config || recip != 1)
			return -EINVAL;
		if (index == 0)
			return value == 0 ? 0 : -EINVAL;
		if (index == 1 && value <= 1) {
			ncm_set_alt(value);
			return 0;
		}
		return -EINVAL;

	case USB_REQ_GET_INTERFACE:
		b[0] = index == 1 ? ncm.data_alt : 0;
		*data = b;
		*len = 1;
		return 0;

	case USB_REQ_GET_STATUS:
		memset(b, 0, 2);
		*data = b;
		*len = 2;
		return 0;

	case USB_REQ_CLEAR_FEATURE:
		return 0;		/* nothing here ever halts an endpoint */
	}

	return -EINVAL;
}

void __hyp hyp_usb_fn_ep0_out(const struct usb_ctrl_req *r, const void *data, u16 len)
{
	if ((r->bRequestType & USB_TYPE_MASK) == USB_TYPE_CLASS &&
	    r->bRequest == NCM_SET_NTB_INPUT_SIZE && len >= 4) {
		u32 size = get_unaligned_le32(data);

		/* The host may only shrink what the device offered */
		if (size >= 2048 && size <= NTB_MAX_SIZE)
			ncm.ntb_in_size = size;
	}
}

void __hyp hyp_usb_fn_reset(void)
{
	ncm.config = 0;
	ncm.data_alt = 0;
	ncm.notify_pending = false;
	hyp_net_link(false);
}

/* ------------------------------------------------------------------------- */
/* Datagrams                                                                  */
/* ------------------------------------------------------------------------- */

static void __hyp ncm_unwrap(const u8 *ntb, u32 len)
{
	u32 ndp, idx;

	if (len < NTH16_LEN || get_unaligned_le32(ntb) != NTH16_SIGN ||
	    get_unaligned_le16(ntb + 4) != NTH16_LEN)
		return;

	ndp = get_unaligned_le16(ntb + 10);
	if (ndp < NTH16_LEN || ndp + 8 > len || (ndp & 3) ||
	    get_unaligned_le32(ntb + ndp) != NDP16_SIGN)
		return;

	for (idx = ndp + 8; idx + 4 <= len; idx += 4) {
		u32 off = get_unaligned_le16(ntb + idx);
		u32 dlen = get_unaligned_le16(ntb + idx + 2);

		if (!off || !dlen)
			break;
		if (off + dlen > len)
			break;

		hyp_net_rx(ntb + off, dlen);
	}
}

/**
 * hyp_ncm_send() - transmit one Ethernet frame to the host
 * @frame: the frame, without CRC
 * @len: its length
 *
 * One NTB, one datagram.  A transfer that is an exact multiple of the bulk packet
 * size would need a zero-length packet to end it, so it is padded instead.
 *
 * Return: 0 when queued, -EBUSY when the previous NTB is still in flight, -ENOLINK
 * when the host has not enabled the data interface.
 */
int __hyp hyp_ncm_send(const void *frame, u32 len)
{
	u8 *tx = ncm.tx;
	u32 total = NTB_HDR_LEN + len;
	int ret;

	if (ncm.data_alt != 1)
		return -ENOLINK;
	if (len > 1514 || total > ncm.ntb_in_size)
		return -EMSGSIZE;
	if (hyp_dwc3_ep_busy(EP_BULK_IN)) {
		ncm.tx_drops++;
		return -EBUSY;
	}

	if (total % HS_BULK_MAXP == 0)
		total += 4;

	memset(tx, 0, NTB_HDR_LEN);
	put_unaligned_le32(NTH16_SIGN, tx);
	put_unaligned_le16(NTH16_LEN, tx + 4);
	put_unaligned_le16(ncm.tx_seq++, tx + 6);
	put_unaligned_le16(total, tx + 8);
	put_unaligned_le16(NTH16_LEN, tx + 10);			/* the NDP follows the NTH */

	put_unaligned_le32(NDP16_SIGN, tx + NTH16_LEN);
	put_unaligned_le16(NDP16_LEN, tx + NTH16_LEN + 4);
	put_unaligned_le16(NTB_HDR_LEN, tx + NTH16_LEN + 8);	/* datagram 0: index, length */
	put_unaligned_le16(len, tx + NTH16_LEN + 10);

	memcpy(tx + NTB_HDR_LEN, frame, len);
	if (total > NTB_HDR_LEN + len)
		memset(tx + NTB_HDR_LEN + len, 0, total - NTB_HDR_LEN - len);

	ret = hyp_dwc3_ep_submit(EP_BULK_IN, tx, total);
	if (!ret)
		ncm.tx_ntbs++;

	return ret;
}

void __hyp hyp_usb_fn_xfer_done(u8 ep, u32 actual)
{
	if (ep == EP_BULK_OUT) {
		ncm.rx_ntbs++;
		ncm_unwrap(ncm.rx, actual);
		rx_arm();
	} else if (ep == EP_BULK_IN) {
		hyp_net_tx_done();
	} else if (ep == EP_NOTIFY && ncm.notify_pending) {
		/* the speed change goes out right behind the connection notification */
		ncm.notify_pending = false;
		hyp_dwc3_ep_submit(EP_NOTIFY, ncm.notify + 8, 16);
	}
}

/* ------------------------------------------------------------------------- */

int __hyp hyp_ncm_init(u64 regs, int intid)
{
	u8 *mem = hyp_mem_alloc(2 * NTB_MAX_SIZE + 64);

	ncm.rx = mem;
	ncm.tx = mem + NTB_MAX_SIZE;
	ncm.notify = mem + 2 * NTB_MAX_SIZE;
	memset(mem, 0, 2 * NTB_MAX_SIZE + 64);
	hyp_clean_range(mem, 2 * NTB_MAX_SIZE + 64);
	ncm.ready = true;
	hyp_net_init();
	hyp_clean_range(&ncm, sizeof(ncm));

	return hyp_dwc3_init(regs, intid);
}
