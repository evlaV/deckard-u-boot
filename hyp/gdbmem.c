// SPDX-License-Identifier: GPL-2.0+
/*
 * GDB servers for address spaces that are not a CPU's, on the USB link (scripts/USB_GADGET.md):
 *
 *   TCP 2346  physical memory: DRAM and device registers, through hyp/phys.c, so a register window
 *             that is not answering is an error reply and not a dead hypervisor.
 *   TCP 2347  the PMICs' SPMI address space, through the PMIC arbiter's debug bus (hyp/spmi.c), a
 *             second arbiter with a channel of its own, so the guest's arbiter is never touched.
 *             Only present when the board's device tree describes that bus.
 *
 * Both are a debugger's view of a machine that is not there: `m` and `M` read and write the
 * space, and the CPU behind it is a stub that is always stopped.  The guest keeps running:
 * nothing here stops it, a Ctrl-C is answered at once with a stop reply, `c` returns at once
 * with no reply (the stub "runs" until the next Ctrl-C), a step stops again at once.  That is
 * exactly what a register-poking tool wants (clockvis halts the target for the length of a batch
 * and resumes it), and it works with a GDB too: `target remote ...:2346`, `x/4wx 0x100000`,
 * `set *(int *)0x100000 = 1`.
 *
 * Packets are served from the TCP callback, in the controller's poll context, one at a time;
 * the other server (hyp/gdb.c) is the one that owns the guest's CPU.
 *
 * Supported packets: ? g G p P m M c s vCont D k H T qSupported qC qfThreadInfo qsThreadInfo
 * qAttached QStartNoAckMode and the Ctrl-C byte.  The registers read as zero and ignore
 * writes.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hv.h"
#include "net.h"
#include "rsp.h"

#define MEM_PORT	2346
#define SPMI_PORT	2347
#define PKT_MAX		2048		/* longest packet payload we take or send */
#define MEM_MAX		(PKT_MAX / 2)
#define NREGS_BYTES	(33 * 8 + 4)	/* x0-x30, sp, pc, cpsr as the other server lays them out */

/* What a server's address space is made of */
struct gdbmem_ops {
	/* @len bytes at @addr into @buf: 0, or a negative errno below 100 (the reply is "E" + it) */
	int (*read)(u64 addr, void *buf, u32 len);
	int (*write)(u64 addr, const void *buf, u32 len);
};

enum mem_rx { RX_IDLE, RX_DATA, RX_CK1, RX_CK2 };

struct gdbmem {
	const struct gdbmem_ops *ops;
	struct tcp_conn *conn;
	enum mem_rx rx;
	u32 len, sum, ck;
	bool noack;		/* QStartNoAckMode is in force */
	bool noack_next;	/* ... from the reply being sent on */
	u8 pkt[PKT_MAX + 8];
	u8 out[PKT_MAX + 8];	/* the last reply, framed, for a retransmission request */
	u32 out_len;
	u8 mem[MEM_MAX];
	char hex[PKT_MAX + 8];
};

static int __hyp phys_read(u64 addr, void *buf, u32 len)
{
	/*
	 * What another bus master (the DSPs) wrote bypassed this core's caches, and a line EL2 or the
	 * guest holds is stale: clean and invalidate DRAM first.  Not the register windows below DRAM.
	 */
	if (addr >= 0x80000000UL) {
		for (u64 a = addr & ~63UL; a < addr + len; a += 64)
			asm volatile("dc civac, %0" :: "r"(a) : "memory");
		asm volatile("dsb sy" ::: "memory");
	}

	return hyp_phys_read(addr, buf, len);
}

static const struct gdbmem_ops __hyp_rodata phys_ops = {
	.read = phys_read,
	.write = hyp_phys_write,
};

static const struct gdbmem_ops __hyp_rodata spmi_ops = {
	.read = hyp_spmi_read,
	.write = hyp_spmi_write,
};

enum { SRV_PHYS, SRV_SPMI, SRV_NR };

static __hyp_data struct gdbmem gm[SRV_NR] = {
	[SRV_PHYS] = { .ops = &phys_ops },
	[SRV_SPMI] = { .ops = &spmi_ops },
};

static void __hyp mem_send(struct gdbmem *g, const void *data, u32 len)
{
	if (!g->conn)
		return;

	if (hyp_tcp_room(g->conn) < len) {
		/* the client sent requests faster than it read the answers: nothing sane to do */
		hyp_tcp_close(g->conn);
		return;
	}
	hyp_tcp_write(g->conn, data, len);
}

/* One packet: $payload#checksum */
static void __hyp mem_reply_bytes(struct gdbmem *g, const char *payload, u32 len)
{
	u8 sum = 0;
	u32 n = 0;

	g->out[n++] = '$';
	for (u32 i = 0; i < len && n < PKT_MAX; i++) {
		g->out[n++] = payload[i];
		sum += payload[i];
	}
	g->out[n++] = '#';
	g->out[n++] = rsp_hexc(sum >> 4);
	g->out[n++] = rsp_hexc(sum);
	g->out_len = n;

	mem_send(g, g->out, n);

	if (g->noack_next) {
		g->noack = true;
		g->noack_next = false;
	}
}

static void __hyp mem_reply(struct gdbmem *g, const char *s)
{
	mem_reply_bytes(g, s, strlen(s));
}

/* An error reply: "E" and the errno, two decimal digits (the ops return codes below 100) */
static void __hyp mem_error(struct gdbmem *g, int err)
{
	char buf[8];
	int n = snprintf(buf, sizeof(buf), "E%02d", err < 0 ? -err : err);

	mem_reply_bytes(g, buf, n);
}

/* The fixed stop reply: signal @sig, the one thread */
static void __hyp mem_stop(struct gdbmem *g, int sig)
{
	char buf[24];
	int n = snprintf(buf, sizeof(buf), "T%02xthread:1;", sig);

	mem_reply_bytes(g, buf, n);
}

/* m addr,length */
static void __hyp cmd_read(struct gdbmem *g, const char *p)
{
	u64 addr, len;
	u32 n;
	int ret;

	if (!rsp_parse_hex(&p, &addr) || *p++ != ',' || !rsp_parse_hex(&p, &len)) {
		mem_reply(g, "E01");
		return;
	}

	n = min_t(u64, len, MEM_MAX);
	ret = g->ops->read(addr, g->mem, n);
	if (ret) {
		mem_error(g, ret);
		return;
	}

	mem_reply_bytes(g, g->hex, rsp_put_hex(g->hex, g->mem, n));
}

/* M addr,length:data */
static void __hyp cmd_write(struct gdbmem *g, const char *p)
{
	u64 addr, len;
	int ret;

	if (!rsp_parse_hex(&p, &addr) || *p++ != ',' || !rsp_parse_hex(&p, &len) || *p++ != ':' ||
	    len > MEM_MAX || !rsp_get_hex(p, g->mem, len)) {
		mem_reply(g, "E01");
		return;
	}

	ret = g->ops->write(addr, g->mem, len);
	if (ret)
		mem_error(g, ret);
	else
		mem_reply(g, "OK");
}

/* qXfer:features:read:target.xml:offset,length - without it GDB assumes FP/SIMD registers */
static void __hyp cmd_xfer_features(struct gdbmem *g, const char *p)
{
	u64 off, len;
	u32 xml_len, n;
	const char *xml = hyp_gdb_target_xml(&xml_len);

	if (!rsp_parse_hex(&p, &off) || *p++ != ',' || !rsp_parse_hex(&p, &len)) {
		mem_reply(g, "E01");
		return;
	}

	if (off >= xml_len) {
		mem_reply(g, "l");
		return;
	}

	n = min_t(u64, len, min_t(u32, xml_len - off, PKT_MAX - 1));
	g->hex[0] = off + n >= xml_len ? 'l' : 'm';
	memcpy(g->hex + 1, xml + off, n);
	mem_reply_bytes(g, g->hex, n + 1);
}

static void __hyp cmd_regs(struct gdbmem *g)
{
	memset(g->mem, 0, NREGS_BYTES);
	mem_reply_bytes(g, g->hex, rsp_put_hex(g->hex, g->mem, NREGS_BYTES));
}

static void __hyp mem_command(struct gdbmem *g, char *p, u32 len)
{
	p[len] = '\0';

	switch (p[0]) {
	case '?':
		mem_stop(g, 5);
		break;
	case 'g':
		cmd_regs(g);
		break;
	case 'p':
		memset(g->mem, 0, 8);
		mem_reply_bytes(g, g->hex, rsp_put_hex(g->hex, g->mem, 8));
		break;
	case 'G':
	case 'P':
	case 'H':
	case 'T':
		mem_reply(g, "OK");
		break;
	case 'm':
		cmd_read(g, p + 1);
		break;
	case 'M':
		cmd_write(g, p + 1);
		break;
	case 'c':
		break;				/* "runs": no reply until the next Ctrl-C */
	case 's':
		mem_stop(g, 5);
		break;
	case 'v':
		if (!strcmp(p, "vCont?"))
			mem_reply(g, "vCont;c;s");
		else if (!strncmp(p, "vCont;", 6) && (p[6] == 's' || p[6] == 'S'))
			mem_stop(g, 5);
		else if (!strncmp(p, "vCont;", 6))
			break;
		else
			mem_reply(g, "");
		break;
	case 'q':
		if (!strncmp(p, "qSupported", 10))
			mem_reply(g, "PacketSize=800;QStartNoAckMode+;qXfer:features:read+");
		else if (!strncmp(p, "qXfer:features:read:target.xml:", 31))
			cmd_xfer_features(g, p + 31);
		else if (!strcmp(p, "qC"))
			mem_reply(g, "QC1");
		else if (!strcmp(p, "qfThreadInfo"))
			mem_reply(g, "m1");
		else if (!strcmp(p, "qsThreadInfo"))
			mem_reply(g, "l");
		else if (!strcmp(p, "qAttached"))
			mem_reply(g, "1");
		else
			mem_reply(g, "");
		break;
	case 'Q':
		if (!strcmp(p, "QStartNoAckMode")) {
			g->noack_next = true;
			mem_reply(g, "OK");
		} else {
			mem_reply(g, "");
		}
		break;
	case 'D':
		mem_reply(g, "OK");
		if (g->conn)
			hyp_tcp_close(g->conn);
		break;
	case 'k':
		if (g->conn)
			hyp_tcp_close(g->conn);
		break;
	default:
		mem_reply(g, "");			/* not supported */
	}
}

static void __hyp mem_ack(struct gdbmem *g, char c)
{
	if (!g->noack && g->conn)
		hyp_tcp_write(g->conn, &c, 1);
}

static void __hyp mem_rx_byte(struct gdbmem *g, u8 c)
{
	switch (g->rx) {
	case RX_IDLE:
		if (c == 0x03) {
			mem_stop(g, 2);		/* the stub is always stopped: a Ctrl-C is a no-op */
		} else if (c == '$') {
			g->rx = RX_DATA;
			g->len = 0;
			g->sum = 0;
		} else if (c == '-' && !g->noack && g->out_len) {
			hyp_tcp_write(g->conn, g->out, g->out_len);
		}
		break;
	case RX_DATA:
		if (c == '#') {
			g->rx = RX_CK1;
		} else {
			g->sum += c;
			if (g->len < PKT_MAX)
				g->pkt[g->len++] = c;
		}
		break;
	case RX_CK1:
		g->ck = (u32)(rsp_unhex(c) & 0xf) << 4;
		g->rx = RX_CK2;
		break;
	case RX_CK2:
		g->ck |= rsp_unhex(c) & 0xf;
		g->rx = RX_IDLE;
		if ((u8)g->sum == g->ck) {
			mem_ack(g, '+');
			mem_command(g, (char *)g->pkt, g->len);
		} else {
			mem_ack(g, '-');
		}
		break;
	}
}

static void __hyp mem_connect(struct tcp_conn *c)
{
	struct gdbmem *g = hyp_tcp_priv(c);
	const struct gdbmem_ops *ops = g->ops;

	memset(g, 0, sizeof(*g));
	g->ops = ops;
	g->conn = c;
	hprintf("gdbmem: client connected\n");
	if (ops == &spmi_ops)
		hyp_spmi_connect();
}

static void __hyp mem_data(struct tcp_conn *c, const u8 *data, u32 len)
{
	struct gdbmem *g = hyp_tcp_priv(c);

	for (u32 i = 0; i < len && g->conn; i++)
		mem_rx_byte(g, data[i]);
}

static void __hyp mem_close(struct tcp_conn *c)
{
	struct gdbmem *g = hyp_tcp_priv(c);

	g->conn = NULL;
	hprintf("gdbmem: client gone\n");
}

static const struct tcp_service __hyp_rodata gdbmem_services[SRV_NR] = {
	[SRV_PHYS] = {
		.port = MEM_PORT,
		.name = "gdb-phys",
		.description = "GDB server for physical memory (the guest keeps running)",
		.on_connect = mem_connect,
		.on_data = mem_data,
		.on_close = mem_close,
		.priv = &gm[SRV_PHYS],
	},
	[SRV_SPMI] = {
		.port = SPMI_PORT,
		.name = "gdb-spmi",
		.description = "GDB server for the PMICs' SPMI registers (the debug arbiter, not the guest's)",
		.on_connect = mem_connect,
		.on_data = mem_data,
		.on_close = mem_close,
		.priv = &gm[SRV_SPMI],
	},
};

void __hyp hyp_gdbmem_init(void)
{
	hyp_tcp_listen(&gdbmem_services[SRV_PHYS]);

	if (hyp_spmi_present())
		hyp_tcp_listen(&gdbmem_services[SRV_SPMI]);
}
