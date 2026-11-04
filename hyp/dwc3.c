// SPDX-License-Identifier: GPL-2.0+
/*
 * DesignWare USB3 controller, device mode, high speed, for EL2
 * (scripts/USB_GADGET.md).
 *
 * The sequences are the ones U-Boot's drivers/usb/dwc3 uses on this board
 * (core.c, gadget.c, ep0.c), reduced to what EL2 needs: one event buffer, one
 * TRB per endpoint, one transfer in flight per endpoint, the control state
 * machine for ep0.  Everything lives in hyp_mem and __hyp_data because ordinary
 * U-Boot data is not safe at EL2 once the guest runs.  The controller is interrupt
 * driven: its SPI is owned by EL2 (hyp_gic_own_irq()), the handler masks the controller's
 * event interrupt and hyp_dwc3_irq_work() drains the events once the IRQ exit path has
 * dropped gic.lock.  Without an INTID in the DT it is polled from every guest exit, and
 * the stop loop of the GDB server polls it as well.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <asm/io.h>
#include <errno.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <string.h>
#include <stdio.h>
#include <hyp/spinlock.h>

#include "hv.h"
#include "dwc3.h"
#include "net.h"

/* Global registers */
#define GCTL			0xc110
#define GSNPSID			0xc120
#define GUSB2PHYCFG		0xc200
#define GUSB3PIPECTL		0xc2c0
#define GEVNTADRLO		0xc400
#define GEVNTADRHI		0xc404
#define GEVNTSIZ		0xc408
#define GEVNTCOUNT		0xc40c
#define GEVNTSIZ_INTMASK	BIT(31)

#define GCTL_DSBLCLKGTNG	BIT(0)
#define GCTL_SCALEDOWN_MASK	GENMASK(5, 4)
#define GCTL_PRTCAPDIR_MASK	GENMASK(13, 12)
#define GCTL_PRTCAP_DEVICE	2
#define GUSB2PHYCFG_SUSPHY	BIT(6)
#define GUSB2PHYCFG_ENBLSLPM	BIT(8)
#define GUSB3PIPECTL_SUSPHY	BIT(17)

/* Device registers */
#define DCFG			0xc700
#define DCTL			0xc704
#define DEVTEN			0xc708
#define DSTS			0xc70c
#define DALEPENA		0xc720
#define DEPCMDPAR2(n)		(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)		(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)		(0xc808 + 0x10 * (n))
#define DEPCMD(n)		(0xc80c + 0x10 * (n))

#define DCFG_SPEED_MASK		GENMASK(2, 0)
#define DCFG_SPEED_HIGH		0
#define DCFG_SPEED_SUPER	4
#define DCFG_DEVADDR_MASK	GENMASK(9, 3)
#define DCFG_DEVADDR(a)		((a) << 3)
#define DCFG_LPM_CAP		BIT(22)

#define DCTL_RUN_STOP		BIT(31)
#define DCTL_CSFTRST		BIT(30)
#define DCTL_KEEP_CONNECT	BIT(19)
#define DCTL_HIRD_THRES_MASK	GENMASK(28, 24)
#define DCTL_L1_HIBER_EN	BIT(18)
#define DCTL_TSTCTRL_MASK	GENMASK(4, 1)

#define DSTS_CONNECTSPD		GENMASK(2, 0)
#define DSTS_DEVCTRLHLT		BIT(22)

#define DEVTEN_DISCONNEVTEN	BIT(0)
#define DEVTEN_USBRSTEN		BIT(1)
#define DEVTEN_CONNECTDONEEN	BIT(2)
#define DEVTEN_ULSTCNGEN	BIT(3)
#define DEVTEN_WKUPEVTEN	BIT(4)
#define DEVTEN_ERRTICERREN	BIT(9)
#define DEVTEN_CMDCMPLTEN	BIT(10)
#define DEVTEN_EVNTOVERFLOWEN	BIT(11)
#define DEVTEN_VNDRDEVTSTRCVEDEN BIT(12)

/* Endpoint commands */
#define DEPCMD_TYPE_MASK	GENMASK(3, 0)
#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_CLEARSTALL	5
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_ENDTRANSFER	8
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_CMDIOC		BIT(8)
#define DEPCMD_CMDACT		BIT(10)
#define DEPCMD_HIPRI_FORCERM	BIT(11)
#define DEPCMD_PARAM(p)		((u32)(p) << 16)
#define DEPCMD_STATUS(r)	(((r) >> 12) & 0xf)
#define DEPCMD_RSC_IDX(r)	(((r) >> 16) & 0x7f)

#define DEPCFG_EP_TYPE(t)	((u32)(t) << 1)
#define DEPCFG_MAXPACKET(m)	((u32)(m) << 3)
/* Bit 31 of parameter 0 (the older-core flag, which is the MODIFY action of newer ones).  Bit 8
 * is inside the max-packet field: using it made ep0's packet size 96 (run 183). */
#define DEPCFG_IGN_SEQ_NUM	BIT(31)
#define DEPCFG_FIFO_NUMBER(n)	((u32)(n) << 17)
#define DEPCFG_XFER_COMPLETE_EN	BIT(8)
#define DEPCFG_XFER_IN_PROGRESS_EN BIT(9)
#define DEPCFG_XFER_NOT_READY_EN BIT(10)
#define DEPCFG_BINTERVAL_M1(n)	((u32)(n) << 16)
#define DEPCFG_EP_NUMBER(n)	((u32)(n) << 25)

/* Transfer request blocks */
struct dwc3_trb {
	u32 bpl;
	u32 bph;
	u32 size;
	u32 ctrl;
};

#define TRB_CTRL_HWO		BIT(0)
#define TRB_CTRL_LST		BIT(1)
#define TRB_CTRL_CHN		BIT(2)
#define TRB_CTRL_ISP_IMI	BIT(10)
#define TRB_CTRL_IOC		BIT(11)
#define TRBCTL(n)		((u32)(n) << 4)
#define TRBCTL_NORMAL		TRBCTL(1)
#define TRBCTL_CONTROL_SETUP	TRBCTL(2)
#define TRBCTL_CONTROL_STATUS2	TRBCTL(3)
#define TRBCTL_CONTROL_STATUS3	TRBCTL(4)
#define TRBCTL_CONTROL_DATA	TRBCTL(5)
#define TRB_SIZE_LENGTH(n)	((n) & 0x00ffffff)

/* Events */
#define EVT_DEVSPEC		BIT(0)
#define DEVT_DISCONNECT		0
#define DEVT_RESET		1
#define DEVT_CONNECTDONE	2
#define DEVT_LINK_STATUS_CHANGE	3
#define DEVT_WAKEUP		4

#define DEPEVT_XFERCOMPLETE	1
#define DEPEVT_XFERNOTREADY	3

#define DEPEVT_STATUS_CTRL_PHASE(s)	((s) & 3)
#define DEPEVT_STATUS_CONTROL_DATA	1
#define DEPEVT_STATUS_CONTROL_STATUS	2

#define EVENT_BUF_SIZE		4096
#define EP0_BUF_SIZE		512
#define DWC3_NUM_EPS		8
#define DMA_SIZE		0x2000

enum ep0_state {
	EP0_SETUP,		/* waiting for a SETUP packet */
	EP0_DATA,		/* data stage in flight */
	EP0_STATUS_WAIT,	/* no data stage: waiting for XferNotReady(STATUS) */
	EP0_STATUS,		/* status stage in flight */
};

struct dwc3_ep {
	bool enabled;
	bool busy;
	u8 rsc_idx;
	u32 len;			/* length of the transfer in flight */
};

struct dwc3_state {
	bool ready;
	u64 regs;
	u8 *dma;			/* hyp_mem block, below */
	u32 *evt;			/* event buffer */
	u32 evt_lpos;			/* read position, bytes */
	struct dwc3_trb *trb;		/* one per physical endpoint */
	struct usb_ctrl_req *setup;	/* 8 bytes the SETUP TRB lands in */
	u8 *ep0buf;			/* ep0 data stage buffer */
	struct dwc3_ep ep[DWC3_NUM_EPS];
	bool start_config_issued;
	bool irq_masked;		/* the interrupt fired and its work is pending */
	bool irq_driven;		/* the controller's SPI is owned: events come from it */
	u32 irqs, events;		/* interrupts taken, events processed */
	enum ep0_state ep0;
	bool three_stage;
	bool expect_in;
	struct usb_ctrl_req req;	/* the SETUP packet being served */
	u8 speed;
	struct spinlock lock;
	int owner;			/* core holding the lock, -1 = none */
	int depth;
};

static __hyp_data struct dwc3_state dwc3 = { .lock = __SPINLOCK_UNLOCKED, .owner = -1 };

/*
 * The controller lock is recursive for the core that holds it: the function layer
 * (hyp/ncm.c) is called from inside the event handler and enables endpoints and
 * submits transfers from there, under the lock hyp_dwc3_poll() already took.
 */
static unsigned long __hyp dwc3_lock(void)
{
	unsigned long flags;
	int me = hyp_cpu_num();

	local_irq_save(flags);
	if (READ_ONCE(dwc3.owner) == me) {
		dwc3.depth++;
		return flags;
	}
	spin_lock(&dwc3.lock);
	dwc3.owner = me;
	dwc3.depth = 1;

	return flags;
}

static void __hyp dwc3_unlock(unsigned long flags)
{
	if (--dwc3.depth == 0) {
		dwc3.owner = -1;
		spin_unlock(&dwc3.lock);
	}
	local_irq_restore(flags);
}

/* The same lock for the virtual devices, which share state with the network stack */
unsigned long __hyp hyp_dwc3_lock(void)
{
	return dwc3_lock();
}

void __hyp hyp_dwc3_unlock(unsigned long flags)
{
	dwc3_unlock(flags);
}

static inline u32 __hyp rd(u32 off)
{
	return readl(dwc3.regs + off);
}

static inline void __hyp wr(u32 off, u32 val)
{
	writel(val, dwc3.regs + off);
}

static inline void __hyp dma_wmb(void)
{
	asm volatile("dsb sy" ::: "memory");
}

static u64 __hyp trb_dma(u8 ep)
{
	return (u64)&dwc3.trb[ep];
}

/* ------------------------------------------------------------------------- */
/* Endpoint commands                                                          */
/* ------------------------------------------------------------------------- */

static int __hyp ep_cmd(u8 ep, u32 cmd, u32 par0, u32 par1, u32 par2)
{
	u64 t0 = hyp_ticks();
	u32 reg;

	wr(DEPCMDPAR0(ep), par0);
	wr(DEPCMDPAR1(ep), par1);
	wr(DEPCMDPAR2(ep), par2);
	wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);

	do {
		reg = rd(DEPCMD(ep));
		if (!(reg & DEPCMD_CMDACT))
			return DEPCMD_STATUS(reg) ? -EIO : 0;
	} while (!hyp_ticks_elapsed(t0, 5000));

	hprintf("dwc3: endpoint %u command %#x timed out\n", ep, cmd);
	return -ETIMEDOUT;
}

static int __hyp ep_start_config(u8 ep)
{
	u32 cmd = DEPCMD_DEPSTARTCFG;

	/* ep0 OUT and ep0 IN share the control configuration: ep0 IN must not
	 * start a new one and drop what ep0 OUT just got */
	if (ep == 1)
		return 0;

	/* XferRscIdx is 0 for ep0 and 2 for the rest */
	if (ep > 1) {
		if (dwc3.start_config_issued)
			return 0;
		dwc3.start_config_issued = true;
		cmd |= DEPCMD_PARAM(2);
	}

	return ep_cmd(0, cmd, 0, 0, 0);
}

static int __hyp ep_configure(u8 ep, u8 type, u16 maxp, u8 interval, bool ign)
{
	u32 par0 = DEPCFG_EP_TYPE(type) | DEPCFG_MAXPACKET(maxp);
	u32 par1 = DEPCFG_XFER_COMPLETE_EN | DEPCFG_XFER_NOT_READY_EN |
		   DEPCFG_EP_NUMBER(ep);

	if (ign)
		par0 |= DEPCFG_IGN_SEQ_NUM;
	if (type != USB_EP_XFER_CONTROL)
		par1 |= DEPCFG_XFER_IN_PROGRESS_EN;
	if (ep & 1)
		par0 |= DEPCFG_FIFO_NUMBER(ep >> 1);	/* the lower 16 TX FIFOs */
	if (interval)
		par1 |= DEPCFG_BINTERVAL_M1(interval - 1);

	return ep_cmd(ep, DEPCMD_SETEPCONFIG, par0, par1, 0);
}

static int __hyp ep_enable(u8 ep, u8 type, u16 maxp, u8 interval, bool ign)
{
	struct dwc3_ep *e = &dwc3.ep[ep];
	int ret;

	if (!e->enabled) {
		ret = ep_start_config(ep);
		if (ret)
			return ret;
	}

	ret = ep_configure(ep, type, maxp, interval, ign);
	if (ret)
		return ret;

	if (!e->enabled) {
		ret = ep_cmd(ep, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
		if (ret)
			return ret;

		e->enabled = true;
		e->busy = false;
		wr(DALEPENA, rd(DALEPENA) | BIT(ep));
	}

	return 0;
}

static void __hyp ep_end_transfer(u8 ep)
{
	struct dwc3_ep *e = &dwc3.ep[ep];

	if (!e->busy)
		return;

	ep_cmd(ep, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_CMDIOC |
	       DEPCMD_PARAM(e->rsc_idx), 0, 0, 0);
	e->busy = false;
	hyp_udelay(100);
}

static void __hyp ep_disable(u8 ep)
{
	struct dwc3_ep *e = &dwc3.ep[ep];

	if (!e->enabled)
		return;

	ep_end_transfer(ep);
	wr(DALEPENA, rd(DALEPENA) & ~BIT(ep));
	e->enabled = false;
}

/* One transfer on one endpoint: one TRB, then STARTTRANSFER */
static int __hyp ep_start(u8 ep, u64 dma, u32 len, u32 ctrl)
{
	struct dwc3_ep *e = &dwc3.ep[ep];
	struct dwc3_trb *trb = &dwc3.trb[ep];
	int ret;

	if (e->busy)
		return -EBUSY;

	trb->bpl = lower_32_bits(dma);
	trb->bph = upper_32_bits(dma);
	trb->size = TRB_SIZE_LENGTH(len);
	trb->ctrl = ctrl | TRB_CTRL_HWO | TRB_CTRL_LST | TRB_CTRL_IOC;
	dma_wmb();

	ret = ep_cmd(ep, DEPCMD_STARTTRANSFER, upper_32_bits(trb_dma(ep)),
		     lower_32_bits(trb_dma(ep)), 0);
	if (ret)
		return ret;

	e->busy = true;
	e->len = len;
	e->rsc_idx = DEPCMD_RSC_IDX(rd(DEPCMD(ep)));

	return 0;
}

/* Bytes the controller did not transfer, read back from the finished TRB */
static u32 __hyp ep_remaining(u8 ep)
{
	dma_wmb();

	return TRB_SIZE_LENGTH(dwc3.trb[ep].size);
}

/* ------------------------------------------------------------------------- */
/* Control endpoint                                                           */
/* ------------------------------------------------------------------------- */

static void __hyp ep0_setup_start(void)
{
	int ret;

	dwc3.ep0 = EP0_SETUP;
	ret = ep_start(0, (u64)dwc3.setup, sizeof(*dwc3.setup),
		       TRBCTL_CONTROL_SETUP | TRB_CTRL_ISP_IMI);
	if (ret)
		hprintf("dwc3: cannot start the SETUP transfer (%d)\n", ret);
}

/* Stall ep0 and go back to waiting for a SETUP packet */
static void __hyp ep0_stall_restart(void)
{
	dwc3.ep[1].busy = false;
	ep_cmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	dwc3.ep[0].busy = false;
	ep0_setup_start();
}

/* The status stage runs opposite to the data (or IN when there was none) */
static void __hyp ep0_status_start(void)
{
	u8 ep = dwc3.expect_in ? 0 : 1;

	dwc3.ep0 = EP0_STATUS;
	ep_start(ep, (u64)dwc3.setup, 0,
		 (dwc3.three_stage ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2) |
		 TRB_CTRL_ISP_IMI);
}

static void __hyp ep0_serve_setup(void)
{
	const struct usb_ctrl_req *req;
	const void *data = NULL;
	u16 len = 0, wlen;
	int ret;

	dma_wmb();
	dwc3.req = *dwc3.setup;
	req = &dwc3.req;
	wlen = le16_to_cpu(req->wLength);

	dwc3.three_stage = wlen != 0;
	dwc3.expect_in = !!(req->bRequestType & USB_DIR_IN);

	if ((req->bRequestType & (USB_TYPE_MASK | USB_RECIP_MASK | USB_DIR_IN)) == 0 &&
	    req->bRequest == USB_REQ_SET_ADDRESS) {
		u32 reg = rd(DCFG) & ~DCFG_DEVADDR_MASK;

		wr(DCFG, reg | DCFG_DEVADDR(le16_to_cpu(req->wValue) & 0x7f));
		ret = 0;
	} else {
		ret = hyp_usb_fn_setup(req, &data, &len);
	}

	if (ret < 0) {
		ep0_stall_restart();
		return;
	}

	if (!dwc3.three_stage) {
		dwc3.ep0 = EP0_STATUS_WAIT;
		return;
	}

	dwc3.ep0 = EP0_DATA;
	if (dwc3.expect_in) {
		u32 n = min_t(u32, min_t(u32, len, wlen), EP0_BUF_SIZE);

		if (data && n)
			memcpy(dwc3.ep0buf, data, n);
		if (ep_start(1, (u64)dwc3.ep0buf, n, TRBCTL_CONTROL_DATA | TRB_CTRL_ISP_IMI))
			ep0_stall_restart();
	} else {
		/* OUT data lands in whole packets: round up to ep0's maxpacket (64) */
		u32 n = ALIGN(min_t(u32, wlen, EP0_BUF_SIZE), 64);

		if (ep_start(0, (u64)dwc3.ep0buf, n, TRBCTL_CONTROL_DATA | TRB_CTRL_ISP_IMI))
			ep0_stall_restart();
	}
}

static void __hyp ep0_event(u8 ep, u32 type, u32 status)
{
	switch (type) {
	case DEPEVT_XFERCOMPLETE:
		dwc3.ep[ep].busy = false;

		switch (dwc3.ep0) {
		case EP0_SETUP:
			ep0_serve_setup();
			break;
		case EP0_DATA:
			if (!dwc3.expect_in) {
				u32 got = dwc3.req.wLength ? le16_to_cpu(dwc3.req.wLength) : 0;

				got = min_t(u32, got, EP0_BUF_SIZE);
				got -= min(got, ep_remaining(0));
				hyp_usb_fn_ep0_out(&dwc3.req, dwc3.ep0buf, got);
			}
			/* The status stage starts when the controller says it is ready for it
			 * (XferNotReady(STATUS)); starting it earlier is ignored. */
			dwc3.ep0 = EP0_STATUS_WAIT;
			break;
		case EP0_STATUS:
			ep0_setup_start();
			break;
		default:
			break;
		}
		break;

	case DEPEVT_XFERNOTREADY:
		if (DEPEVT_STATUS_CTRL_PHASE(status) == DEPEVT_STATUS_CONTROL_STATUS &&
		    dwc3.ep0 == EP0_STATUS_WAIT)
			ep0_status_start();
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* Device events                                                              */
/* ------------------------------------------------------------------------- */

static void __hyp dctl_write(u32 val)
{
	wr(DCTL, val);
}

static void __hyp ev_reset(void)
{
	u32 reg;

	/* Everything the function had running is gone */
	for (int ep = 2; ep < DWC3_NUM_EPS; ep++)
		ep_disable(ep);
	hyp_usb_fn_reset();

	dctl_write(rd(DCTL) & ~DCTL_TSTCTRL_MASK);

	dwc3.start_config_issued = false;
	dwc3.ep0 = EP0_SETUP;

	reg = rd(DCFG) & ~DCFG_DEVADDR_MASK;
	wr(DCFG, reg);
}

static void __hyp ev_connect_done(void)
{
	u32 reg = rd(DSTS);

	dwc3.speed = reg & DSTS_CONNECTSPD;

	/* The full-speed/high-speed ep0 packet size is 64 */
	/* snps,usb2-gadget-lpm-disable in this board's DT: LPM stays off (DCFG.LPMCap clear) */
	dctl_write(rd(DCTL) & ~(DCTL_HIRD_THRES_MASK | DCTL_L1_HIBER_EN));

	ep_enable(0, USB_EP_XFER_CONTROL, 64, 0, true);
	ep_enable(1, USB_EP_XFER_CONTROL, 64, 0, true);

	hprintf("dwc3: connected, speed %u; DSTS %#x DCFG %#x DCTL %#x DALEPENA %#x ep0 busy %d/%d\n",
		dwc3.speed, rd(DSTS), rd(DCFG), rd(DCTL), rd(DALEPENA),
		dwc3.ep[0].busy, dwc3.ep[1].busy);
}

static void __hyp ev_device(u32 ev)
{
	/* devt: one_bit[0], device_event[7:1] (0 = device), type[11:8], event_info[24:16] */
	switch ((ev >> 8) & 0xf) {
	case DEVT_DISCONNECT:
		hprintf("dwc3: disconnect\n");
		for (int ep = 2; ep < DWC3_NUM_EPS; ep++)
			ep_disable(ep);
		hyp_usb_fn_reset();
		dwc3.start_config_issued = false;
		break;
	case DEVT_RESET:
		ev_reset();
		break;
	case DEVT_CONNECTDONE:
		ev_connect_done();
		break;
	default:
		break;
	}
}

static void __hyp ev_endpoint(u32 ev)
{
	u8 ep = (ev >> 1) & 0x1f;
	u32 type = (ev >> 6) & 0xf;
	u32 status = (ev >> 12) & 0xf;

	if (ep >= DWC3_NUM_EPS)
		return;

	if (ep < 2) {
		ep0_event(ep, type, status);
		return;
	}

	if (type == DEPEVT_XFERCOMPLETE && dwc3.ep[ep].busy) {
		u32 actual = dwc3.ep[ep].len - min(dwc3.ep[ep].len, ep_remaining(ep));

		dwc3.ep[ep].busy = false;
		hyp_usb_fn_xfer_done(ep, actual);
	}
}

static void __hyp process_events(void)
{
	u32 count = rd(GEVNTCOUNT) & 0xfffc;

	while (count) {
		u32 ev;

		dma_wmb();
		ev = dwc3.evt[dwc3.evt_lpos / 4];

		if (ev & EVT_DEVSPEC)
			ev_device(ev);
		else
			ev_endpoint(ev);

		dwc3.evt_lpos = (dwc3.evt_lpos + 4) % EVENT_BUF_SIZE;
		wr(GEVNTCOUNT, 4);
		count -= 4;
		dwc3.events++;

		if (!count)
			count = rd(GEVNTCOUNT) & 0xfffc;
	}
}

void __hyp hyp_dwc3_poll(void)
{
	unsigned long flags;

	if (!dwc3.ready)
		return;

	flags = dwc3_lock();
	process_events();
	hyp_net_tick();
	dwc3_unlock(flags);
}

/**
 * hyp_dwc3_tick() - the guest exited to EL2: run the network's timers
 *
 * With an interrupt the events are the interrupt's business (hyp_dwc3_irq_work()), and
 * this only gives the TCP retransmit timers a chance to run, without touching the
 * controller.  A controller with no INTID in the DT has to be polled here instead.
 */
void __hyp hyp_dwc3_tick(void)
{
	unsigned long flags;

	if (!dwc3.ready)
		return;
	if (!dwc3.irq_driven) {
		hyp_dwc3_poll();
		return;
	}

	flags = dwc3_lock();
	hyp_net_tick();
	dwc3_unlock(flags);
}

/*
 * The controller's interrupt.  It runs under gic.lock with the line still asserted, so it
 * only quiesces the source - the event interrupt is masked in GEVNTSIZ - and leaves the
 * work to hyp_dwc3_irq_work(), which the IRQ exit path calls once the lock is dropped.
 */
static void __hyp dwc3_irq(int intid)
{
	wr(GEVNTSIZ, EVENT_BUF_SIZE | GEVNTSIZ_INTMASK);
	dwc3.irq_masked = true;
	dwc3.irqs++;
}

/**
 * hyp_dwc3_irq_work() - drain what the controller's interrupt announced
 *
 * Called by the IRQ exit path after hyp_gic_irq(), outside gic.lock.  The interrupt is
 * unmasked again once the event buffer is empty; an event that arrives in between holds
 * the (level) line up and the interrupt fires again.
 */
void __hyp hyp_dwc3_irq_work(void)
{
	unsigned long flags;

	if (!dwc3.ready || !dwc3.irq_masked)
		return;

	flags = dwc3_lock();
	process_events();
	hyp_net_tick();
	dwc3.irq_masked = false;
	wr(GEVNTSIZ, EVENT_BUF_SIZE);
	dwc3_unlock(flags);
}

/* What the controller has done since bring-up, for the console */
int __hyp hyp_dwc3_describe(char *buf, int size)
{
	return snprintf(buf, size, "usb: %u interrupts, %u events, speed %u, ep0 state %d",
			dwc3.irqs, dwc3.events, dwc3.speed, dwc3.ep0);
}

/* ------------------------------------------------------------------------- */
/* Function-layer endpoint API                                                */
/* ------------------------------------------------------------------------- */

int __hyp hyp_dwc3_ep_enable(u8 ep, u8 type, u16 maxp, u8 interval)
{
	unsigned long flags;
	int ret;

	if (ep < 2 || ep >= DWC3_NUM_EPS)
		return -EINVAL;

	flags = dwc3_lock();
	ret = ep_enable(ep, type, maxp, interval, false);
	dwc3_unlock(flags);

	return ret;
}

void __hyp hyp_dwc3_ep_disable(u8 ep)
{
	unsigned long flags;

	if (ep < 2 || ep >= DWC3_NUM_EPS)
		return;

	flags = dwc3_lock();
	ep_disable(ep);
	dwc3_unlock(flags);
}

bool __hyp hyp_dwc3_ep_busy(u8 ep)
{
	return ep < DWC3_NUM_EPS && dwc3.ep[ep].busy;
}

int __hyp hyp_dwc3_ep_submit(u8 ep, void *buf, u32 len)
{
	unsigned long flags;
	int ret;

	if (ep < 2 || ep >= DWC3_NUM_EPS || !dwc3.ep[ep].enabled)
		return -ENODEV;

	flags = dwc3_lock();
	ret = ep_start(ep, (u64)buf, len, TRBCTL_NORMAL);
	dwc3_unlock(flags);
	return ret;
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* ------------------------------------------------------------------------- */

static int __hyp core_soft_reset(void)
{
	u64 t0 = hyp_ticks();

	wr(DCTL, DCTL_CSFTRST);
	while (rd(DCTL) & DCTL_CSFTRST)
		if (hyp_ticks_elapsed(t0, 500000)) {
			hprintf("dwc3: DCTL %#x, GCTL %#x, GUSB2PHYCFG %#x, GUSB3PIPECTL %#x\n",
				rd(DCTL), rd(GCTL), rd(GUSB2PHYCFG), rd(GUSB3PIPECTL));
			return -ETIMEDOUT;
		}

	return 0;
}

int __hyp hyp_dwc3_init(u64 regs, int intid)
{
	u32 id, reg;
	int ret;

	dwc3.regs = regs;
	id = rd(GSNPSID);
	if ((id >> 16) != 0x3331 && (id >> 16) != 0x5533) {
		hprintf("dwc3: %#llx is not a DesignWare USB3 core (GSNPSID %#x)\n", regs, id);
		return -ENODEV;
	}

	dwc3.dma = hyp_mem_alloc(DMA_SIZE);
	memset(dwc3.dma, 0, DMA_SIZE);
	hyp_clean_range(dwc3.dma, DMA_SIZE);
	dwc3.evt = (u32 *)dwc3.dma;
	dwc3.setup = (struct usb_ctrl_req *)(dwc3.dma + EVENT_BUF_SIZE);
	dwc3.ep0buf = dwc3.dma + EVENT_BUF_SIZE + 64;
	dwc3.trb = (struct dwc3_trb *)(dwc3.dma + EVENT_BUF_SIZE + 64 + EP0_BUF_SIZE);

	/* The core's own reset first, then the PHY interface (core.c) */
	ret = core_soft_reset();
	if (ret) {
		hprintf("dwc3: device soft reset timed out\n");
		return ret;
	}

	/* This board's DT: dis_u2_susphy_quirk, dis_enblslpm_quirk */
	/* U-Boot's working state has GUSB3PIPECTL.SUSPHY clear (core revision compare), so no SUSPHY */
	hyp_udelay(100000);
	reg = rd(GUSB2PHYCFG) & ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM);
	wr(GUSB2PHYCFG, reg);
	hyp_udelay(100000);

	ret = core_soft_reset();
	if (ret) {
		hprintf("dwc3: second soft reset timed out\n");
		return ret;
	}
	hyp_udelay(100000);

	reg = rd(GCTL) & ~(GCTL_SCALEDOWN_MASK | GCTL_DSBLCLKGTNG | GCTL_PRTCAPDIR_MASK);
	reg |= (u32)GCTL_PRTCAP_DEVICE << 12;
	reg |= BIT(16);			/* U2RSTECN, as U-Boot's working state has it */
	wr(GCTL, reg);

	/* One event buffer */
	wr(GEVNTADRLO, lower_32_bits((u64)dwc3.evt));
	wr(GEVNTADRHI, upper_32_bits((u64)dwc3.evt));
	wr(GEVNTSIZ, EVENT_BUF_SIZE);
	wr(GEVNTCOUNT, 0);
	dwc3.evt_lpos = 0;

	/*
	 * DEVSPD is super speed even though no SS PHY runs: that is what U-Boot's
	 * working state has (the core falls back to high speed on its own), and forcing
	 * high speed here left ep0 deaf.  Address 0.
	 */
	wr(DCFG, (rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK)) | DCFG_SPEED_SUPER);

	/* ep0 both ways, then wait for the first SETUP packet */
	dwc3.start_config_issued = false;
	ret = ep_enable(0, USB_EP_XFER_CONTROL, 64, 0, false);
	if (!ret)
		ret = ep_enable(1, USB_EP_XFER_CONTROL, 64, 0, false);
	if (ret) {
		hprintf("dwc3: cannot enable ep0 (%d)\n", ret);
		return ret;
	}
	ep0_setup_start();

	wr(DEVTEN, DEVTEN_VNDRDEVTSTRCVEDEN | DEVTEN_EVNTOVERFLOWEN | DEVTEN_CMDCMPLTEN |
	   DEVTEN_ERRTICERREN | DEVTEN_WKUPEVTEN | DEVTEN_ULSTCNGEN |
	   DEVTEN_CONNECTDONEEN | DEVTEN_USBRSTEN | DEVTEN_DISCONNEVTEN);

	dwc3.ready = true;
	hyp_clean_range(&dwc3, sizeof(dwc3));

	if (intid > 0)
		dwc3.irq_driven = !hyp_gic_own_irq(intid, true, dwc3_irq);

	/* Connect: the pull-up goes on */
	reg = rd(DCTL) & ~DCTL_KEEP_CONNECT;
	wr(DCTL, reg | DCTL_RUN_STOP);

	{
		u64 t0 = hyp_ticks();

		while (rd(DSTS) & DSTS_DEVCTRLHLT)
			if (hyp_ticks_elapsed(t0, 100000)) {
				hprintf("dwc3: the controller did not leave the halted state\n");
				return -ETIMEDOUT;
			}
	}

	hprintf("dwc3: device mode, GSNPSID %#x, running; INTID %d\n", id, intid);

	return 0;
}
