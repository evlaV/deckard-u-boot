// SPDX-License-Identifier: GPL-2.0+
/*
 * Network behind EL2's USB link (scripts/USB_GADGET.md) or, without USB, behind the guest's
 * virtio NIC alone: Ethernet, ARP, IPv4 and IPv6 link-local, ICMP/ICMPv6 echo, neighbour
 * discovery, a DHCP server for the peer, and the demux into the TCP server (hyp/tcp.c).
 *
 * Addresses: with USB the hypervisor is IPv4 192.168.77.2 and the host gets 192.168.77.1/24
 * from the DHCP server below; without it the hypervisor is 192.168.77.1 and the guest's NIC
 * is leased 192.168.77.2.  IPv6 is fe80::68:79ff:fe70:2, the link-local address the MAC
 * gives (EUI-64), which needs nothing on the peer beyond the interface being up.  Everything
 * runs from the controller's poll context (or a guest exit, without USB), one frame at a
 * time, on buffers in __hyp_data.
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
#include "net.h"
#include <stdio.h>

#define ETH_ALEN	6
#define ETH_HLEN	14
#define ETH_P_IP	0x0800
#define ETH_P_ARP	0x0806
#define ETH_P_IPV6	0x86dd

#define ARP_HLEN	28
#define ARP_REQUEST	1
#define ARP_REPLY	2

#define IP_HLEN		20
#define IP6_HLEN	40
#define IPPROTO_ICMP	1
#define IPPROTO_TCP	6
#define IPPROTO_ICMPV6	58
#define ICMP_ECHO	8
#define ICMP_ECHOREPLY	0

#define ICMP6_ECHO_REQUEST	128
#define ICMP6_ECHO_REPLY	129
#define ND_NEIGHBOR_SOLICIT	135
#define ND_NEIGHBOR_ADVERT	136

#define FRAME_MAX	1536

#if CONFIG_IS_ENABLED(HYP_USB)
/* 192.168.77.2 for us, 192.168.77.1 for the host */
#define NET_IP		0xc0a84d02
#define PEER_IP		0xc0a84d01
#else
/* 192.168.77.1 for us, 192.168.77.2 for the guest's NIC */
#define NET_IP		0xc0a84d01
#define PEER_IP		0xc0a84d02
#endif

/* The device's MAC; the host's is 02:68:79:70:00:01 (the ECM descriptor's string) */
const u8 __hyp_rodata hyp_net_mac[NET_MAC_LEN] = { 0x02, 0x68, 0x79, 0x70, 0x00, 0x02 };

/* fe80::0068:79ff:fe70:0002 - EUI-64 of 02:68:79:70:00:02 (universal/local bit flipped) */
static const u8 __hyp_rodata our_ip6[16] = {
	0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x00, 0x68, 0x79, 0xff, 0xfe, 0x70, 0x00, 0x02,
};

struct hyp_net {
	bool up;
	u8 host_mac[ETH_ALEN];
	bool host_mac_known;
	hyp_net_tx_fn tx;
	bool tx_pending;		/* a frame found the USB function busy */
	u8 frame[FRAME_MAX];		/* the frame being built */
	u32 rx_frames, tx_frames, tx_busy, icmp_echoes, arp_replies, nd_adverts;
	u32 icmp6_echoes, tcp_segments, dropped, dhcp_offers, dhcp_acks, dhcp_naks;
};

static __hyp_data struct hyp_net net = { 0 };

void __hyp hyp_net_set_tx(hyp_net_tx_fn fn)
{
#if CONFIG_IS_ENABLED(HYP_USB)
	net.tx = fn ? fn : hyp_ncm_send;
#endif
}

/* The text of the console's `stats` line for the network */
int __hyp hyp_net_describe(char *buf, int size)
{
	return snprintf(buf, size,
			"net: %u frames in, %u out (%u busy); arp %u, icmp %u, icmp6 %u, nd %u, tcp %u, "
			"dhcp offer/ack/nak %u/%u/%u",
			net.rx_frames, net.tx_frames, net.tx_busy, net.arp_replies, net.icmp_echoes,
			net.icmp6_echoes, net.nd_adverts, net.tcp_segments, net.dhcp_offers,
			net.dhcp_acks, net.dhcp_naks);
}

void __hyp hyp_net_our_ip(bool v6, u8 *out)
{
	if (v6) {
		memcpy(out, our_ip6, 16);
	} else {
		put_unaligned_be32(NET_IP, out);
	}
}

/* The Internet checksum accumulator: add @len bytes to @sum (16-bit big-endian words) */
static u32 __hyp csum_add(u32 sum, const void *data, u32 len)
{
	const u8 *p = data;

	for (; len > 1; len -= 2, p += 2)
		sum += (p[0] << 8) | p[1];
	if (len)
		sum += p[0] << 8;

	return sum;
}

static u16 __hyp csum_fold(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (u16)~sum;
}

/* The checksum of an upper-layer message including the IPv4/IPv6 pseudo-header */
static u16 __hyp l4_checksum(bool v6, u8 proto, const u8 *src, const u8 *dst, const u8 *msg, u32 len)
{
	u32 sum = 0;

	if (v6) {
		sum = csum_add(sum, src, 16);
		sum = csum_add(sum, dst, 16);
		sum += len >> 16;
		sum += len & 0xffff;
		sum += proto;
	} else {
		sum = csum_add(sum, src, 4);
		sum = csum_add(sum, dst, 4);
		sum += proto;
		sum += len;
	}
	sum = csum_add(sum, msg, len);

	return csum_fold(sum);
}

u16 __hyp hyp_net_tcp_checksum(bool v6, const u8 *src_ip, const u8 *dst_ip, const u8 *seg, u32 len)
{
	return l4_checksum(v6, IPPROTO_TCP, src_ip, dst_ip, seg, len);
}

static int __hyp net_tx(u32 len)
{
	int ret = 0;

	/* a reply to the guest's NIC goes into its receive queue, not out on the link */
	if (hyp_vdev_net_from_stack(net.frame, len))
		return 0;

#if CONFIG_IS_ENABLED(HYP_USB)
	if (!net.tx)
		net.tx = hyp_ncm_send;

	net.tx_frames++;
	ret = net.tx(net.frame, len);
	if (ret == -EBUSY) {
		net.tx_busy++;
		net.tx_pending = true;
	}
#endif

	return ret;
}

static void __hyp eth_header(const u8 *dst, u16 type)
{
	memcpy(net.frame, dst, ETH_ALEN);
	memcpy(net.frame + ETH_ALEN, hyp_net_mac, ETH_ALEN);
	put_unaligned_be16(type, net.frame + 12);
}

/* ------------------------------------------------------------------------- */
/* ARP                                                                        */
/* ------------------------------------------------------------------------- */

static void __hyp arp_rx(const u8 *src_mac, const u8 *arp, u32 len)
{
	u8 *p = net.frame + ETH_HLEN;

	if (len < ARP_HLEN || get_unaligned_be16(arp) != 1 ||
	    get_unaligned_be16(arp + 2) != ETH_P_IP || arp[4] != ETH_ALEN || arp[5] != 4)
		return;

	if (get_unaligned_be16(arp + 6) != ARP_REQUEST || get_unaligned_be32(arp + 24) != NET_IP)
		return;

	eth_header(src_mac, ETH_P_ARP);
	put_unaligned_be16(1, p);
	put_unaligned_be16(ETH_P_IP, p + 2);
	p[4] = ETH_ALEN;
	p[5] = 4;
	put_unaligned_be16(ARP_REPLY, p + 6);
	memcpy(p + 8, hyp_net_mac, ETH_ALEN);
	put_unaligned_be32(NET_IP, p + 14);
	memcpy(p + 18, arp + 8, ETH_ALEN);
	memcpy(p + 24, arp + 14, 4);
	net.arp_replies++;
	net_tx(ETH_HLEN + ARP_HLEN);
}

/* ------------------------------------------------------------------------- */
/* IPv4                                                                       */
/* ------------------------------------------------------------------------- */

static void __hyp icmp_rx(const u8 *src_mac, const u8 *ip, u32 hlen, u32 len)
{
	const u8 *icmp = ip + hlen;
	u32 ilen = len - hlen;
	u8 *p = net.frame + ETH_HLEN;

	if (ilen < 8 || icmp[0] != ICMP_ECHO || ilen + IP_HLEN > FRAME_MAX - ETH_HLEN)
		return;
	if (csum_fold(csum_add(0, icmp, ilen)))
		return;

	/* The echo reply: our IP header around the request's ICMP payload */
	eth_header(src_mac, ETH_P_IP);
	memset(p, 0, IP_HLEN);
	p[0] = 0x45;
	put_unaligned_be16(IP_HLEN + ilen, p + 2);
	p[8] = 64;
	p[9] = IPPROTO_ICMP;
	put_unaligned_be32(NET_IP, p + 12);
	memcpy(p + 16, ip + 12, 4);
	put_unaligned_be16(csum_fold(csum_add(0, p, IP_HLEN)), p + 10);

	memcpy(p + IP_HLEN, icmp, ilen);
	p[IP_HLEN] = ICMP_ECHOREPLY;
	p[IP_HLEN + 2] = p[IP_HLEN + 3] = 0;
	put_unaligned_be16(csum_fold(csum_add(0, p + IP_HLEN, ilen)), p + IP_HLEN + 2);

	net.icmp_echoes++;
	net_tx(ETH_HLEN + IP_HLEN + ilen);
}

/* ------------------------------------------------------------------------- */
/* DHCP: the host's address                                                   */
/* ------------------------------------------------------------------------- */

/*
 * A one-client DHCP server: whoever asks gets PEER_IP/24, so the host's NetworkManager (or
 * networkd, or dhclient) configures usb0 on its own and keeps the address.  The offer has
 * no router and no DNS: the link is a debug link, and a default route over it would
 * take the host's traffic away from its real network.  The /24 on-link route comes with
 * the address.  Replies are broadcast (the client has no address yet), except to a client
 * renewing the address it already has.
 */
#define IPPROTO_UDP		17
#define DHCP_SERVER_PORT	67
#define DHCP_CLIENT_PORT	68
#define DHCP_FIXED_LEN		236		/* BOOTP header up to the options */
#define DHCP_COOKIE		0x63825363
#define DHCP_LEASE_SECS		86400

#define DHCPDISCOVER	1
#define DHCPOFFER	2
#define DHCPREQUEST	3
#define DHCPACK		5
#define DHCPNAK		6

#define DHCP_OPT_MASK		1
#define DHCP_OPT_BROADCAST	28
#define DHCP_OPT_REQUESTED	50
#define DHCP_OPT_LEASE		51
#define DHCP_OPT_TYPE		53
#define DHCP_OPT_SERVER		54
#define DHCP_OPT_END		255

/* What the request's options say; 0 / false for an option that is absent */
struct dhcp_opts {
	u8 type;
	bool has_requested, has_server;
	u32 requested, server;
};

static void __hyp dhcp_parse_options(const u8 *o, u32 len, struct dhcp_opts *opt)
{
	memset(opt, 0, sizeof(*opt));

	for (u32 i = 0; i < len && o[i] != DHCP_OPT_END;) {
		u8 code = o[i], n;

		if (code == 0) {		/* pad */
			i++;
			continue;
		}
		if (i + 1 >= len)
			return;
		n = o[i + 1];
		if (i + 2 + n > len)
			return;

		if (code == DHCP_OPT_TYPE && n == 1) {
			opt->type = o[i + 2];
		} else if (code == DHCP_OPT_REQUESTED && n == 4) {
			opt->requested = get_unaligned_be32(o + i + 2);
			opt->has_requested = true;
		} else if (code == DHCP_OPT_SERVER && n == 4) {
			opt->server = get_unaligned_be32(o + i + 2);
			opt->has_server = true;
		}
		i += 2 + n;
	}
}

static u8 __hyp *dhcp_put_opt(u8 *o, u8 code, u8 len, u32 val)
{
	*o++ = code;
	*o++ = len;
	if (len == 1)
		*o++ = val;
	else
		put_unaligned_be32(val, o), o += 4;

	return o;
}

/* Answer @req (the BOOTP message in the client's frame) with @type */
static void __hyp dhcp_reply(const u8 *src_mac, const u8 *req, u8 type)
{
	u8 *p = net.frame + ETH_HLEN, *udp = p + IP_HLEN, *b = udp + 8, *o;
	static const u8 __hyp_rodata bcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	bool renew = type != DHCPNAK && get_unaligned_be32(req + 12) != 0;	/* ciaddr */
	u32 ulen, dst = renew ? get_unaligned_be32(req + 12) : 0xffffffff;

	/* broadcast, as the client has no address yet - unless the guest's NIC is the only
	 * listener, which has the frame to itself by its MAC */
	eth_header(renew || !CONFIG_IS_ENABLED(HYP_USB) ? src_mac : bcast, ETH_P_IP);

	/* BOOTP: a reply carrying the request's xid, flags, giaddr and client hardware address */
	memset(b, 0, DHCP_FIXED_LEN + 4);
	b[0] = 2;
	b[1] = 1;
	b[2] = ETH_ALEN;
	memcpy(b + 4, req + 4, 4);			/* xid */
	memcpy(b + 10, req + 10, 2);			/* flags */
	if (type != DHCPNAK) {
		memcpy(b + 12, req + 12, 4);		/* ciaddr */
		put_unaligned_be32(PEER_IP, b + 16);	/* yiaddr */
		put_unaligned_be32(NET_IP, b + 20);	/* siaddr */
	}
	memcpy(b + 24, req + 24, 4);			/* giaddr */
	memcpy(b + 28, req + 28, 16);			/* chaddr */
	put_unaligned_be32(DHCP_COOKIE, b + DHCP_FIXED_LEN);

	o = b + DHCP_FIXED_LEN + 4;
	o = dhcp_put_opt(o, DHCP_OPT_TYPE, 1, type);
	o = dhcp_put_opt(o, DHCP_OPT_SERVER, 4, NET_IP);
	if (type != DHCPNAK) {
		o = dhcp_put_opt(o, DHCP_OPT_LEASE, 4, DHCP_LEASE_SECS);
		o = dhcp_put_opt(o, DHCP_OPT_MASK, 4, 0xffffff00);
		o = dhcp_put_opt(o, DHCP_OPT_BROADCAST, 4, 0xc0a84dff);
	}
	*o++ = DHCP_OPT_END;

	ulen = o - udp;
	put_unaligned_be16(DHCP_SERVER_PORT, udp);
	put_unaligned_be16(DHCP_CLIENT_PORT, udp + 2);
	put_unaligned_be16(ulen, udp + 4);

	memset(p, 0, IP_HLEN);
	p[0] = 0x45;
	put_unaligned_be16(IP_HLEN + ulen, p + 2);
	p[8] = 64;
	p[9] = IPPROTO_UDP;
	put_unaligned_be32(NET_IP, p + 12);
	put_unaligned_be32(dst, p + 16);
	put_unaligned_be16(csum_fold(csum_add(0, p, IP_HLEN)), p + 10);

	udp[6] = udp[7] = 0;
	{
		u16 sum = l4_checksum(false, IPPROTO_UDP, p + 12, p + 16, udp, ulen);

		put_unaligned_be16(sum ? sum : 0xffff, udp + 6);	/* 0 means "no checksum" */
	}

	if (type == DHCPOFFER)
		net.dhcp_offers++;
	else if (type == DHCPACK)
		net.dhcp_acks++;
	else
		net.dhcp_naks++;
	net_tx(ETH_HLEN + IP_HLEN + ulen);
}

static void __hyp dhcp_rx(const u8 *src_mac, const u8 *ip, u32 hlen, u32 total)
{
	const u8 *udp = ip + hlen, *req = udp + 8;
	u32 ulen = total - hlen;
	struct dhcp_opts opt;
	u32 ci;

	if (ulen < 8 + DHCP_FIXED_LEN + 4 || get_unaligned_be16(udp + 2) != DHCP_SERVER_PORT)
		return;
	if (get_unaligned_be16(udp + 4) > ulen)
		return;
	if (req[0] != 1 || req[1] != 1 || req[2] != ETH_ALEN ||
	    get_unaligned_be32(req + DHCP_FIXED_LEN) != DHCP_COOKIE)
		return;

	dhcp_parse_options(req + DHCP_FIXED_LEN + 4, ulen - 8 - DHCP_FIXED_LEN - 4, &opt);

	/* a client that has picked another server is not ours to answer */
	if (opt.has_server && opt.server != NET_IP)
		return;

	if (opt.type == DHCPDISCOVER) {
		dhcp_reply(src_mac, req, DHCPOFFER);
	} else if (opt.type == DHCPREQUEST) {
		/* selecting (server id), init-reboot (requested address) or renewing (ciaddr) */
		ci = get_unaligned_be32(req + 12);
		if ((opt.has_requested ? opt.requested : ci) == PEER_IP)
			dhcp_reply(src_mac, req, DHCPACK);
		else
			dhcp_reply(src_mac, req, DHCPNAK);
	}
}

static void __hyp ip_rx(const u8 *src_mac, const u8 *ip, u32 len)
{
	u32 hlen, total, dst;

	if (len < IP_HLEN || (ip[0] >> 4) != 4)
		return;
	hlen = (ip[0] & 0xf) * 4;
	total = get_unaligned_be16(ip + 2);
	if (hlen < IP_HLEN || total < hlen || total > len)
		return;
	/* ours, or a broadcast that only the DHCP server wants */
	dst = get_unaligned_be32(ip + 16);
	if (dst != NET_IP && !(dst == 0xffffffff && ip[9] == IPPROTO_UDP))
		return;
	if (csum_fold(csum_add(0, ip, hlen)))
		return;
	/* no fragments */
	if (get_unaligned_be16(ip + 6) & 0x3fff)
		return;

	if (ip[9] == IPPROTO_ICMP)
		icmp_rx(src_mac, ip, hlen, total);
	else if (ip[9] == IPPROTO_TCP)
		hyp_tcp_rx(src_mac, false, ip + 12, ip + 16, ip + hlen, total - hlen);
	else if (ip[9] == IPPROTO_UDP)
		dhcp_rx(src_mac, ip, hlen, total);
}

/* ------------------------------------------------------------------------- */
/* IPv6                                                                       */
/* ------------------------------------------------------------------------- */

static bool __hyp ip6_is_ours(const u8 *dst)
{
	/* our unicast address, or its solicited-node multicast group ff02::1:ff70:2 */
	static const u8 __hyp_rodata sn[13] = {
		0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff,
	};

	return !memcmp(dst, our_ip6, 16) || (!memcmp(dst, sn, 13) && !memcmp(dst + 13, our_ip6 + 13, 3));
}

/* Send an ICMPv6 message: @msg is the ICMPv6 body with the checksum field zeroed */
static void __hyp icmp6_send(const u8 *dst_mac, const u8 *dst_ip, u8 *msg, u32 len)
{
	u8 *p = net.frame + ETH_HLEN;

	eth_header(dst_mac, ETH_P_IPV6);
	memset(p, 0, IP6_HLEN);
	p[0] = 0x60;
	put_unaligned_be16(len, p + 4);
	p[6] = IPPROTO_ICMPV6;
	p[7] = 255;
	memcpy(p + 8, our_ip6, 16);
	memcpy(p + 24, dst_ip, 16);
	/* msg may be inside net.frame already (built in place) */
	if (msg != p + IP6_HLEN)
		memmove(p + IP6_HLEN, msg, len);
	p[IP6_HLEN + 2] = p[IP6_HLEN + 3] = 0;
	put_unaligned_be16(l4_checksum(true, IPPROTO_ICMPV6, our_ip6, dst_ip, p + IP6_HLEN, len),
			   p + IP6_HLEN + 2);
	net_tx(ETH_HLEN + IP6_HLEN + len);
}

static void __hyp ndp_rx(const u8 *src_mac, const u8 *src_ip, const u8 *icmp, u32 len)
{
	u8 *m = net.frame + ETH_HLEN + IP6_HLEN;

	/* Neighbour solicitation for our address: answer with our MAC */
	if (len < 24 || memcmp(icmp + 8, our_ip6, 16))
		return;

	/* The solicitation's source link-layer option beats the frame's source MAC */
	if (len >= 32 && icmp[24] == 1 && icmp[25] == 1)
		src_mac = icmp + 26;

	if (!memcmp(src_ip, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16))
		return;			/* duplicate address detection by the host */

	m[0] = ND_NEIGHBOR_ADVERT;
	m[1] = 0;
	m[2] = m[3] = 0;
	m[4] = 0x60;			/* solicited, override */
	m[5] = m[6] = m[7] = 0;
	memcpy(m + 8, our_ip6, 16);
	m[24] = 2;			/* target link-layer address */
	m[25] = 1;
	memcpy(m + 26, hyp_net_mac, ETH_ALEN);
	net.nd_adverts++;
	icmp6_send(src_mac, src_ip, m, 32);
}

static void __hyp ip6_rx(const u8 *src_mac, const u8 *ip, u32 len)
{
	u32 plen;
	const u8 *icmp;

	if (len < IP6_HLEN || (ip[0] >> 4) != 6)
		return;
	plen = get_unaligned_be16(ip + 4);
	if (IP6_HLEN + plen > len)
		return;
	if (!ip6_is_ours(ip + 24))
		return;

	icmp = ip + IP6_HLEN;

	if (ip[6] == IPPROTO_TCP) {
		hyp_tcp_rx(src_mac, true, ip + 8, ip + 24, icmp, plen);
		return;
	}

	if (ip[6] != IPPROTO_ICMPV6 || plen < 8)
		return;
	if (l4_checksum(true, IPPROTO_ICMPV6, ip + 8, ip + 24, icmp, plen))
		return;

	if (icmp[0] == ND_NEIGHBOR_SOLICIT) {
		ndp_rx(src_mac, ip + 8, icmp, plen);
	} else if (icmp[0] == ICMP6_ECHO_REQUEST && !memcmp(ip + 24, our_ip6, 16) &&
		   plen + IP6_HLEN + ETH_HLEN <= FRAME_MAX) {
		u8 *m = net.frame + ETH_HLEN + IP6_HLEN;

		memcpy(m, icmp, plen);
		m[0] = ICMP6_ECHO_REPLY;
		net.icmp6_echoes++;
		icmp6_send(src_mac, ip + 8, m, plen);
	}
}

/* ------------------------------------------------------------------------- */
/* TCP out, receive path                                                      */
/* ------------------------------------------------------------------------- */

/* Send a TCP segment (header and payload in @seg, checksum field zero) to the peer */
int __hyp hyp_net_send_tcp(const u8 *dst_mac, bool v6, const u8 *dst_ip, const u8 *seg, u32 len)
{
	u8 *p = net.frame + ETH_HLEN;
	u8 *tcp;
	const u8 *src = v6 ? our_ip6 : (const u8 *)&(u32){ cpu_to_be32(NET_IP) };

	if (!net.up)
		return -ENOLINK;

	if (v6) {
		if (ETH_HLEN + IP6_HLEN + len > FRAME_MAX)
			return -EMSGSIZE;
		eth_header(dst_mac, ETH_P_IPV6);
		memset(p, 0, IP6_HLEN);
		p[0] = 0x60;
		put_unaligned_be16(len, p + 4);
		p[6] = IPPROTO_TCP;
		p[7] = 64;
		memcpy(p + 8, our_ip6, 16);
		memcpy(p + 24, dst_ip, 16);
		tcp = p + IP6_HLEN;
	} else {
		if (ETH_HLEN + IP_HLEN + len > FRAME_MAX)
			return -EMSGSIZE;
		eth_header(dst_mac, ETH_P_IP);
		memset(p, 0, IP_HLEN);
		p[0] = 0x45;
		put_unaligned_be16(IP_HLEN + len, p + 2);
		p[8] = 64;
		p[9] = IPPROTO_TCP;
		put_unaligned_be32(NET_IP, p + 12);
		memcpy(p + 16, dst_ip, 4);
		put_unaligned_be16(csum_fold(csum_add(0, p, IP_HLEN)), p + 10);
		tcp = p + IP_HLEN;
	}

	memcpy(tcp, seg, len);
	tcp[16] = tcp[17] = 0;
	put_unaligned_be16(l4_checksum(v6, IPPROTO_TCP, src, dst_ip, tcp, len), tcp + 16);
	net.tcp_segments++;

	return net_tx(ETH_HLEN + (v6 ? IP6_HLEN : IP_HLEN) + len);
}

/* ------------------------------------------------------------------------- */

/* One Ethernet frame into the stack; @guest: it came from the guest's NIC, not from the host */
static void __hyp net_input(const u8 *frame, u32 len, bool guest)
{
	u16 type;

	net.rx_frames++;

	/* Whoever sends to us is where answers go */
	if (!guest) {
		memcpy(net.host_mac, frame + ETH_ALEN, ETH_ALEN);
		net.host_mac_known = true;
	}

	type = get_unaligned_be16(frame + 12);

	if (type == ETH_P_ARP)
		arp_rx(frame + ETH_ALEN, frame + ETH_HLEN, len - ETH_HLEN);
	else if (type == ETH_P_IP)
		ip_rx(frame + ETH_ALEN, frame + ETH_HLEN, len - ETH_HLEN);
	else if (type == ETH_P_IPV6)
		ip6_rx(frame + ETH_ALEN, frame + ETH_HLEN, len - ETH_HLEN);
}

/* A frame from the host, off the USB link */
void __hyp hyp_net_rx(const u8 *frame, u32 len)
{
	if (!net.up || len < ETH_HLEN)
		return;

	/* the guest's NIC is on the same link: frames for it, and copies of broadcasts */
	if (hyp_vdev_net_rx(frame, len))
		return;

	net_input(frame, len, false);
}

/**
 * hyp_net_from_guest() - a frame the guest's NIC sent
 *
 * It goes out on the link unless it is addressed to the hypervisor's own MAC, and into the
 * stack when it is addressed to it or is a broadcast or multicast.
 *
 * Return: 0 when it was taken (or dropped for lack of a link), -EBUSY when the USB function
 * cannot take it yet: the guest's queue keeps it until hyp_net_tx_done().
 */
int __hyp hyp_net_from_guest(const u8 *frame, u32 len)
{
	bool group = frame[0] & 1, ours = !memcmp(frame, hyp_net_mac, ETH_ALEN);

	if (!net.up || len < ETH_HLEN)
		return 0;

#if CONFIG_IS_ENABLED(HYP_USB)
	if (!ours) {
		int ret;

		if (!net.tx)
			net.tx = hyp_ncm_send;
		ret = net.tx(frame, len);
		if (ret == -EBUSY) {
			net.tx_busy++;
			net.tx_pending = true;
			return -EBUSY;
		}
		net.tx_frames++;
	}
#endif

	if (ours || group)
		net_input(frame, len, true);

	return 0;
}

void __hyp hyp_net_link(bool up)
{
	if (net.up && !up)
		hyp_tcp_reset_all();

	net.up = up;
	net.host_mac_known = false;
	hprintf("net: link %s; %u.%u.%u.%u, fe80::68:79ff:fe70:2\n", up ? "up" : "down",
		NET_IP >> 24, (NET_IP >> 16) & 0xff, (NET_IP >> 8) & 0xff, NET_IP & 0xff);
}

void __hyp hyp_net_tx_done(void)
{
	if (net.tx_pending) {
		net.tx_pending = false;
		hyp_tcp_flush();
	}
	/* frames the guest's NIC could not hand over while the function was busy */
	hyp_vdev_tx_resume();
}

void __hyp hyp_net_init(void)
{
	static __hyp_data bool done;

	/* the USB function and the hand-over to the OS both ask */
	if (done)
		return;
	done = true;

	hyp_tcp_init();
	hyp_telnet_init();
	hyp_gdb_init();
	hyp_gdbmem_init();
	hyp_vdev_init();

	/* no USB link to wait for: the guest's NIC is the link, up from the start */
	if (!CONFIG_IS_ENABLED(HYP_USB))
		hyp_net_link(true);
}

void __hyp hyp_net_tick(void)
{
	if (net.up)
		hyp_tcp_tick();
	hyp_vdev_tick();
	hyp_telnet_tick();
}
