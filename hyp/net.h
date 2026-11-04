/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * The network behind EL2's USB link: Ethernet, ARP, IPv4 (static address), IPv6
 * link-local (derived from the MAC, nothing to configure on the host), ICMP/ICMPv6
 * echo, neighbour discovery and a small TCP server (hyp/net.c, hyp/tcp.c).
 * scripts/USB_GADGET.md.
 */
#ifndef __HYP_NET_H__
#define __HYP_NET_H__

#include <linux/types.h>

/* From the USB function layer (hyp/ncm.c) */
int hyp_ncm_send(const void *frame, u32 len);
int hyp_ncm_init(u64 regs, int intid);

/* To the network, from the USB function layer */
void hyp_net_link(bool up);			/* the host enabled/disabled the data interface */
void hyp_net_rx(const u8 *frame, u32 len);	/* one Ethernet frame from the host */
void hyp_net_tx_done(void);			/* the previous frame has left */

/* Called often (every controller poll): retransmit timers */
void hyp_net_tick(void);

/* Counters, as one line of text */
int hyp_net_describe(char *buf, int size);


/* ---- the guest's NIC, bridged onto this link (hyp/vdev.c) ---- */
bool hyp_vdev_net_rx(const u8 *frame, u32 len);		/* host -> guest; true: the guest's alone */
bool hyp_vdev_net_from_stack(const u8 *frame, u32 len);	/* the stack's reply is the guest's */
void hyp_vdev_tx_resume(void);				/* the link is free: held-back guest frames */
void hyp_vdev_tick(void);				/* the guest console's output goes out */
int hyp_vdev_describe(char *buf, int size);
void hyp_vdev_init(void);
int hyp_net_from_guest(const u8 *frame, u32 len);	/* guest -> link / stack; -EBUSY: try later */
/* the self-tests' handles on the devices */
bool hyp_vdev_test_access(u64 pa, bool store, u32 size, u64 *val);
void hyp_vdev_test_irq(void (*fn)(int intid));
u32 hyp_vdev_test_console(u8 *out, u32 size, const u8 *in, u32 in_len);
/*
 * Where frames go.  The default is the USB function (hyp_ncm_send()); the self-tests
 * point it at a capture buffer.
 */
typedef int (*hyp_net_tx_fn)(const void *frame, u32 len);
void hyp_net_set_tx(hyp_net_tx_fn fn);

/* ---- addresses ---- */
#define NET_MAC_LEN	6
extern const u8 hyp_net_mac[NET_MAC_LEN];

/* ---- TCP server (hyp/tcp.c) ---- */
struct tcp_conn;

struct tcp_service {
	u16 port;
	const char *name;		/* one word, for the console's `info` */
	const char *description;	/* one line */
	void (*on_connect)(struct tcp_conn *c);
	void (*on_data)(struct tcp_conn *c, const u8 *data, u32 len);
	void (*on_close)(struct tcp_conn *c);
	void *priv;			/* the service's own state, for hyp_tcp_priv() */
};

/* Register a listener; returns 0, or -ENOSPC when the table is full */
int hyp_tcp_listen(const struct tcp_service *svc);

/* The @i'th registered service, or NULL; @up is set when a client is connected to it */
const struct tcp_service *hyp_tcp_service(int i, bool *up);

/* The state a connection's service registered in `priv` */
void *hyp_tcp_priv(const struct tcp_conn *c);

/* Queue bytes for the peer; returns how many fit (the rest is dropped) */
u32 hyp_tcp_write(struct tcp_conn *c, const void *data, u32 len);
/* Close once everything queued has been acknowledged */
void hyp_tcp_close(struct tcp_conn *c);
bool hyp_tcp_established(const struct tcp_conn *c);
/* Bytes queued but not yet acknowledged by the peer */
u32 hyp_tcp_pending(const struct tcp_conn *c);
/* Bytes hyp_tcp_write() would take right now */
u32 hyp_tcp_room(const struct tcp_conn *c);

/* Used by net.c */
void hyp_tcp_init(void);
void hyp_tcp_rx(const u8 *src_mac, bool v6, const u8 *src_ip, const u8 *dst_ip,
		const u8 *seg, u32 len);
void hyp_tcp_reset_all(void);
void hyp_tcp_tick(void);
void hyp_tcp_flush(void);

/* net.c services for tcp.c */
int hyp_net_send_tcp(const u8 *dst_mac, bool v6, const u8 *dst_ip, const u8 *seg, u32 len);
u16 hyp_net_tcp_checksum(bool v6, const u8 *src_ip, const u8 *dst_ip, const u8 *seg, u32 len);
void hyp_net_our_ip(bool v6, u8 *out);

/* telnet console (hyp/telnet.c) */
void hyp_telnet_init(void);
void hyp_telnet_tick(void);			/* more of the listing the console client asked for */

/* GDB server for the guest (hyp/gdb.c) */
void hyp_gdb_init(void);
const char *hyp_gdb_target_xml(u32 *len);	/* its register description, for hyp/gdbmem.c */

/* GDB server for physical memory (hyp/gdbmem.c) */
void hyp_gdbmem_init(void);

#endif /* __HYP_NET_H__ */
