// SPDX-License-Identifier: GPL-2.0+
/*
 * A small TCP server for EL2's debug link (scripts/USB_GADGET.md).
 *
 * One connection per registered service, in-order receive only, one unacknowledged
 * data segment in flight with a doubling retransmit timer, FIN/RST handling.
 * Service callbacks run from the controller's poll context, synchronously, so the
 * advertised window is a constant: whatever arrives has been consumed by the time
 * the ACK goes out.  A new SYN for a port whose connection is still up replaces it
 * (a debugger that vanished without a FIN must not lock the port).
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <asm/unaligned.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <string.h>

#include "hv.h"
#include "net.h"

#define TCP_MAX_SERVICES	8
#define TCP_TXBUF		4096
#define TCP_MSS			1400
#define TCP_WINDOW		8192
#define TCP_HLEN		20
#define TCP_RTO_US		200000U
#define TCP_RTO_MAX_US		2000000U
#define TCP_MAX_RETRIES		8

#define TH_FIN	0x01
#define TH_SYN	0x02
#define TH_RST	0x04
#define TH_PSH	0x08
#define TH_ACK	0x10

enum tcp_state {
	TCP_CLOSED,
	TCP_SYN_RCVD,
	TCP_ESTAB,
	TCP_FIN_WAIT1,		/* we sent FIN, not yet acknowledged */
	TCP_FIN_WAIT2,		/* our FIN acknowledged, waiting for theirs */
	TCP_CLOSE_WAIT,		/* they sent FIN, we have not closed yet */
	TCP_LAST_ACK,		/* they sent FIN and we sent ours */
};

struct tcp_conn {
	const struct tcp_service *svc;
	enum tcp_state state;
	bool v6;
	u8 peer_ip[16];
	u16 peer_port;
	u8 peer_mac[6];
	u32 snd_una, snd_nxt, rcv_nxt;
	u16 peer_mss;
	u16 peer_win;
	u8 *txbuf;			/* unacknowledged and unsent data, from snd_una */
	u32 tx_len;
	u32 tx_sent;			/* of tx_len, in flight */
	bool fin_pending, fin_sent;
	bool rto_armed;
	u64 rto_start;
	u32 rto_us;
	u8 retries;
};

struct hyp_tcp {
	struct tcp_conn conn[TCP_MAX_SERVICES];
	int nsvc;
	u8 seg[TCP_HLEN + 8 + TCP_MSS];
	u32 isn_salt;
};

static __hyp_data struct hyp_tcp tcp = { 0 };

void __hyp hyp_tcp_init(void)
{
	u8 *mem = hyp_mem_alloc(TCP_MAX_SERVICES * TCP_TXBUF);

	for (int i = 0; i < TCP_MAX_SERVICES; i++)
		tcp.conn[i].txbuf = mem + i * TCP_TXBUF;
	hyp_clean_range(mem, TCP_MAX_SERVICES * TCP_TXBUF);
}

int __hyp hyp_tcp_listen(const struct tcp_service *svc)
{
	if (tcp.nsvc == TCP_MAX_SERVICES)
		return -ENOSPC;

	tcp.conn[tcp.nsvc++].svc = svc;

	return 0;
}

const struct tcp_service __hyp *hyp_tcp_service(int i, bool *up)
{
	if (i < 0 || i >= tcp.nsvc)
		return NULL;

	if (up)
		*up = tcp.conn[i].state == TCP_ESTAB;

	return tcp.conn[i].svc;
}

void __hyp *hyp_tcp_priv(const struct tcp_conn *c)
{
	return c->svc->priv;
}

static struct tcp_conn *__hyp conn_for_port(u16 port)
{
	for (int i = 0; i < tcp.nsvc; i++)
		if (tcp.conn[i].svc->port == port)
			return &tcp.conn[i];

	return NULL;
}

static bool __hyp same_peer(const struct tcp_conn *c, bool v6, const u8 *ip, u16 port)
{
	return c->v6 == v6 && c->peer_port == port && !memcmp(c->peer_ip, ip, v6 ? 16 : 4);
}

/* ------------------------------------------------------------------------- */
/* Segments out                                                               */
/* ------------------------------------------------------------------------- */

static int __hyp seg_send(const u8 *mac, bool v6, const u8 *ip, u16 sport, u16 dport, u32 seq,
			  u32 ack, u8 flags, const void *payload, u32 plen, u16 mss_opt)
{
	u8 *s = tcp.seg;
	u32 hlen = TCP_HLEN + (mss_opt ? 4 : 0);

	if (plen > TCP_MSS)
		return -EMSGSIZE;

	memset(s, 0, hlen);
	put_unaligned_be16(sport, s);
	put_unaligned_be16(dport, s + 2);
	put_unaligned_be32(seq, s + 4);
	put_unaligned_be32(ack, s + 8);
	s[12] = (hlen / 4) << 4;
	s[13] = flags;
	put_unaligned_be16(TCP_WINDOW, s + 14);
	if (mss_opt) {
		s[20] = 2;
		s[21] = 4;
		put_unaligned_be16(mss_opt, s + 22);
	}
	if (plen)
		memcpy(s + hlen, payload, plen);

	return hyp_net_send_tcp(mac, v6, ip, s, hlen + plen);
}

static int __hyp conn_send(struct tcp_conn *c, u32 seq, u8 flags, const void *payload, u32 plen)
{
	return seg_send(c->peer_mac, c->v6, c->peer_ip, c->svc->port, c->peer_port, seq,
			c->rcv_nxt, flags, payload, plen, 0);
}

static void __hyp send_rst(const u8 *mac, bool v6, const u8 *ip, u16 sport, u16 dport,
			   u32 seq, u32 ack, u8 flags, u32 seglen)
{
	if (flags & TH_ACK)
		seg_send(mac, v6, ip, dport, sport, ack, 0, TH_RST, NULL, 0, 0);
	else
		seg_send(mac, v6, ip, dport, sport, 0, seq + seglen, TH_RST | TH_ACK, NULL, 0, 0);
}

static void __hyp rto_arm(struct tcp_conn *c)
{
	c->rto_armed = true;
	c->rto_start = hyp_ticks();
	if (!c->rto_us)
		c->rto_us = TCP_RTO_US;
}

static void __hyp conn_reset(struct tcp_conn *c, bool notify)
{
	bool was_up = c->state != TCP_CLOSED;

	c->state = TCP_CLOSED;
	c->tx_len = c->tx_sent = 0;
	c->fin_pending = c->fin_sent = false;
	c->rto_armed = false;
	c->rto_us = 0;
	c->retries = 0;

	if (was_up && notify && c->svc->on_close)
		c->svc->on_close(c);
}

/* The payload of one data segment: our limit, or the peer's if it is smaller */
static u32 __hyp conn_mss(const struct tcp_conn *c)
{
	return min_t(u32, TCP_MSS, c->peer_mss ? c->peer_mss : 536);
}

/*
 * Send what can be sent: every unsent data segment the peer's window takes (the peer
 * would otherwise sit on its delayed ACK for the first one while the second waits), else
 * our FIN once the data is acknowledged.  A segment that finds the USB function busy is
 * tried again when the previous frame is out (hyp_tcp_flush()).
 */
static void __hyp conn_push(struct tcp_conn *c)
{
	u32 n;

	if (c->state != TCP_ESTAB && c->state != TCP_CLOSE_WAIT && c->state != TCP_FIN_WAIT1)
		return;

	if (c->tx_sent < c->tx_len) {
		while (c->tx_sent < c->tx_len) {
			n = min(c->tx_len - c->tx_sent, conn_mss(c));
			if (c->peer_win) {
				if (c->tx_sent >= c->peer_win)
					break;
				n = min(n, c->peer_win - c->tx_sent);
			}
			if (conn_send(c, c->snd_una + c->tx_sent, TH_ACK | TH_PSH, c->txbuf + c->tx_sent, n))
				return;
			c->tx_sent += n;
			c->snd_nxt = c->snd_una + c->tx_sent;
			if (!c->rto_armed)
				rto_arm(c);
		}
		return;
	}

	if (!c->tx_len && c->fin_pending && !c->fin_sent) {
		if (conn_send(c, c->snd_nxt, TH_ACK | TH_FIN, NULL, 0))
			return;
		c->fin_sent = true;
		c->snd_nxt++;
		c->state = c->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT1;
		rto_arm(c);
	}
}

/* ------------------------------------------------------------------------- */
/* API for services                                                           */
/* ------------------------------------------------------------------------- */

u32 __hyp hyp_tcp_write(struct tcp_conn *c, const void *data, u32 len)
{
	u32 room;

	if (c->state != TCP_ESTAB && c->state != TCP_CLOSE_WAIT)
		return 0;

	room = TCP_TXBUF - c->tx_len;
	if (len > room)
		len = room;
	memcpy(c->txbuf + c->tx_len, data, len);
	c->tx_len += len;
	conn_push(c);

	return len;
}

void __hyp hyp_tcp_close(struct tcp_conn *c)
{
	c->fin_pending = true;
	conn_push(c);
}

bool __hyp hyp_tcp_established(const struct tcp_conn *c)
{
	return c->state == TCP_ESTAB;
}

u32 __hyp hyp_tcp_pending(const struct tcp_conn *c)
{
	return c->tx_len;
}

u32 __hyp hyp_tcp_room(const struct tcp_conn *c)
{
	return TCP_TXBUF - c->tx_len;
}

/* ------------------------------------------------------------------------- */
/* Segments in                                                                */
/* ------------------------------------------------------------------------- */

static u16 __hyp parse_mss(const u8 *seg, u32 hlen)
{
	for (u32 i = TCP_HLEN; i + 1 < hlen;) {
		u8 kind = seg[i];

		if (kind == 0)
			break;
		if (kind == 1) {
			i++;
			continue;
		}
		if (seg[i + 1] < 2 || i + seg[i + 1] > hlen)
			break;
		if (kind == 2 && seg[i + 1] == 4)
			return get_unaligned_be16(seg + i + 2);
		i += seg[i + 1];
	}

	return 0;
}

static void __hyp handle_ack(struct tcp_conn *c, u32 ack)
{
	u32 acked = ack - c->snd_una, inflight = c->snd_nxt - c->snd_una;
	bool fin_acked = false;

	if (!acked || acked > inflight)
		return;

	if (c->fin_sent && ack == c->snd_nxt) {
		fin_acked = true;
		acked--;
	}
	if (acked) {
		memmove(c->txbuf, c->txbuf + acked, c->tx_len - acked);
		c->tx_len -= acked;
		c->tx_sent -= min(c->tx_sent, acked);
	}

	c->snd_una = ack;
	c->retries = 0;
	c->rto_us = 0;
	c->rto_armed = c->tx_sent || (c->fin_sent && !fin_acked);
	if (c->rto_armed)
		rto_arm(c);		/* the next segment in flight is timed from now */

	if (fin_acked) {
		if (c->state == TCP_FIN_WAIT1)
			c->state = TCP_FIN_WAIT2;
		else if (c->state == TCP_LAST_ACK)
			conn_reset(c, false);
	}
}

void __hyp hyp_tcp_rx(const u8 *src_mac, bool v6, const u8 *src_ip, const u8 *dst_ip,
		      const u8 *seg, u32 len)
{
	u8 our[16];
	u16 sport, dport, win;
	u32 seq, ack, hlen, plen, seglen;
	u8 flags;
	struct tcp_conn *c;
	const u8 *payload;

	hyp_net_our_ip(v6, our);
	if (len < TCP_HLEN || memcmp(dst_ip, our, v6 ? 16 : 4))
		return;
	if (hyp_net_tcp_checksum(v6, src_ip, dst_ip, seg, len))
		return;

	sport = get_unaligned_be16(seg);
	dport = get_unaligned_be16(seg + 2);
	seq = get_unaligned_be32(seg + 4);
	ack = get_unaligned_be32(seg + 8);
	hlen = (seg[12] >> 4) * 4;
	flags = seg[13];
	win = get_unaligned_be16(seg + 14);
	if (hlen < TCP_HLEN || hlen > len)
		return;
	payload = seg + hlen;
	plen = len - hlen;
	seglen = plen + !!(flags & TH_SYN) + !!(flags & TH_FIN);

	c = conn_for_port(dport);
	if (!c) {
		if (!(flags & TH_RST))
			send_rst(src_mac, v6, src_ip, sport, dport, seq, ack, flags, seglen);
		return;
	}

	/* A SYN opens (or replaces) the connection */
	if ((flags & (TH_SYN | TH_ACK)) == TH_SYN) {
		if (c->state != TCP_CLOSED && same_peer(c, v6, src_ip, sport) && seq + 1 == c->rcv_nxt) {
			/* a retransmitted SYN: repeat the SYN-ACK */
			seg_send(c->peer_mac, v6, c->peer_ip, dport, sport, c->snd_una, c->rcv_nxt,
				 TH_SYN | TH_ACK, NULL, 0, TCP_MSS);
			return;
		}
		if (c->state != TCP_CLOSED)
			conn_reset(c, true);

		c->v6 = v6;
		memcpy(c->peer_ip, src_ip, v6 ? 16 : 4);
		c->peer_port = sport;
		memcpy(c->peer_mac, src_mac, 6);
		c->peer_mss = parse_mss(seg, hlen);
		c->peer_win = win;
		c->rcv_nxt = seq + 1;
		c->snd_una = c->snd_nxt = hyp_ticks() * 2654435761U + tcp.isn_salt++;
		c->state = TCP_SYN_RCVD;
		seg_send(c->peer_mac, v6, c->peer_ip, dport, sport, c->snd_una, c->rcv_nxt,
			 TH_SYN | TH_ACK, NULL, 0, TCP_MSS);
		c->snd_nxt = c->snd_una + 1;
		rto_arm(c);
		return;
	}

	if (c->state == TCP_CLOSED || !same_peer(c, v6, src_ip, sport)) {
		if (!(flags & TH_RST))
			send_rst(src_mac, v6, src_ip, sport, dport, seq, ack, flags, seglen);
		return;
	}

	if (flags & TH_RST) {
		conn_reset(c, true);
		return;
	}

	c->peer_win = win;

	if (c->state == TCP_SYN_RCVD) {
		if (!(flags & TH_ACK) || ack != c->snd_nxt) {
			send_rst(src_mac, v6, src_ip, sport, dport, seq, ack, flags, seglen);
			return;
		}
		c->snd_una = ack;
		c->state = TCP_ESTAB;
		c->rto_armed = false;
		c->rto_us = 0;
		if (c->svc->on_connect)
			c->svc->on_connect(c);
	}

	if (flags & TH_ACK)
		handle_ack(c, ack);

	/* data: only the next in-order byte is taken */
	if (plen && c->state != TCP_CLOSED) {
		if (seq == c->rcv_nxt && c->state != TCP_LAST_ACK) {
			c->rcv_nxt += plen;
			if (c->svc->on_data)
				c->svc->on_data(c, payload, plen);
		}
		conn_send(c, c->snd_nxt, TH_ACK, NULL, 0);
	} else if (!seglen && seq != c->rcv_nxt && c->state != TCP_CLOSED) {
		/* An empty segment just below the next expected byte is a keepalive probe
		 * (virt-console.py sets TCP_KEEPIDLE): without the ACK it declares an idle
		 * link dead after five seconds and reconnects. */
		conn_send(c, c->snd_nxt, TH_ACK, NULL, 0);
	}

	if ((flags & TH_FIN) && seq + plen == c->rcv_nxt && c->state != TCP_CLOSED) {
		c->rcv_nxt++;
		conn_send(c, c->snd_nxt, TH_ACK, NULL, 0);
		if (c->state == TCP_ESTAB) {
			c->state = TCP_CLOSE_WAIT;
			if (c->svc->on_close)
				c->svc->on_close(c);
			/* nothing more to say: close our side as well */
			hyp_tcp_close(c);
		} else if (c->state == TCP_FIN_WAIT1 || c->state == TCP_FIN_WAIT2) {
			conn_reset(c, true);
		}
	}

	conn_push(c);
}

/* ------------------------------------------------------------------------- */
/* Timers and link events                                                     */
/* ------------------------------------------------------------------------- */

void __hyp hyp_tcp_flush(void)
{
	for (int i = 0; i < tcp.nsvc; i++)
		if (tcp.conn[i].state != TCP_CLOSED)
			conn_push(&tcp.conn[i]);
}

void __hyp hyp_tcp_tick(void)
{
	for (int i = 0; i < tcp.nsvc; i++) {
		struct tcp_conn *c = &tcp.conn[i];

		if (c->state == TCP_CLOSED || !c->rto_armed || !hyp_ticks_elapsed(c->rto_start, c->rto_us))
			continue;

		if (++c->retries > TCP_MAX_RETRIES) {
			conn_reset(c, true);
			continue;
		}

		if (c->state == TCP_SYN_RCVD)
			seg_send(c->peer_mac, c->v6, c->peer_ip, c->svc->port, c->peer_port,
				 c->snd_una, c->rcv_nxt, TH_SYN | TH_ACK, NULL, 0, TCP_MSS);
		else if (c->tx_sent)
			conn_send(c, c->snd_una, TH_ACK | TH_PSH, c->txbuf, min(c->tx_sent, conn_mss(c)));
		else if (c->fin_sent)
			conn_send(c, c->snd_nxt - 1, TH_ACK | TH_FIN, NULL, 0);

		c->rto_us = min_t(u32, c->rto_us * 2, TCP_RTO_MAX_US);
		c->rto_start = hyp_ticks();
	}
}

void __hyp hyp_tcp_reset_all(void)
{
	for (int i = 0; i < tcp.nsvc; i++)
		if (tcp.conn[i].state != TCP_CLOSED)
			conn_reset(&tcp.conn[i], true);
}
