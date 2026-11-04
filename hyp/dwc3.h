/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * EL2's DesignWare USB3 controller in device mode (hyp/dwc3.c), and the USB
 * function it serves (hyp/ncm.c).  High speed only; one outstanding transfer per
 * endpoint.  scripts/USB_GADGET.md.
 */
#ifndef __HYP_DWC3_H__
#define __HYP_DWC3_H__

#include <linux/types.h>

/* USB 2.0 chapter 9 */
#define USB_DIR_IN		0x80
#define USB_TYPE_MASK		0x60
#define USB_TYPE_STANDARD	0x00
#define USB_TYPE_CLASS		0x20
#define USB_RECIP_MASK		0x1f

#define USB_REQ_GET_STATUS	0x00
#define USB_REQ_CLEAR_FEATURE	0x01
#define USB_REQ_SET_FEATURE	0x03
#define USB_REQ_SET_ADDRESS	0x05
#define USB_REQ_GET_DESCRIPTOR	0x06
#define USB_REQ_GET_CONFIG	0x08
#define USB_REQ_SET_CONFIG	0x09
#define USB_REQ_GET_INTERFACE	0x0a
#define USB_REQ_SET_INTERFACE	0x0b

#define USB_EP_XFER_CONTROL	0
#define USB_EP_XFER_BULK	2
#define USB_EP_XFER_INT		3

struct usb_ctrl_req {
	u8 bRequestType;
	u8 bRequest;
	__le16 wValue;
	__le16 wIndex;
	__le16 wLength;
} __packed;

/*
 * Physical endpoint numbers: (USB endpoint number << 1) | 1 for IN.  ep0 OUT is
 * 0, ep0 IN is 1, bulk OUT 1 is 2, bulk IN 1 is 3, interrupt IN 2 is 5.
 */
#define DWC3_EP(num, in)	(((num) << 1) | !!(in))

/*
 * Bring the controller up in device mode and run it.  @regs is the controller's
 * register window, @intid the INTID its interrupt arrives on (0 = polled only).
 */
int hyp_dwc3_init(u64 regs, int intid);

/* Process pending controller events and timers: for callers that wait on the controller */
void hyp_dwc3_poll(void);

/* The text of the console's `stats` line for the controller */
int hyp_dwc3_describe(char *buf, int size);

/* The lock everything on this link runs under (recursive for one core): see hyp/vdev.c */
unsigned long hyp_dwc3_lock(void);
void hyp_dwc3_unlock(unsigned long flags);

/* Endpoint control for the function layer */
int hyp_dwc3_ep_enable(u8 ep, u8 type, u16 maxp, u8 interval);
void hyp_dwc3_ep_disable(u8 ep);
bool hyp_dwc3_ep_busy(u8 ep);
/* Start one transfer; hyp_usb_fn_xfer_done() runs when it has finished */
int hyp_dwc3_ep_submit(u8 ep, void *buf, u32 len);

/* What the function layer provides */
/*
 * A SETUP packet that is not SET_ADDRESS.  Return < 0 to stall.  For an IN data
 * stage return 0 and set *data and *len; for an OUT data stage return 1 and set *len
 * to the bytes expected (hyp_usb_fn_ep0_out() runs with them); with no data
 * stage return 0 and *len = 0.
 */
int hyp_usb_fn_setup(const struct usb_ctrl_req *req, const void **data, u16 *len);
void hyp_usb_fn_ep0_out(const struct usb_ctrl_req *req, const void *data, u16 len);
void hyp_usb_fn_xfer_done(u8 ep, u32 actual);
/* Bus reset: every endpoint but ep0 has been disabled */
void hyp_usb_fn_reset(void);

#endif /* __HYP_DWC3_H__ */
