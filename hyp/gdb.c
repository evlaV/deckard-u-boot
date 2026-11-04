// SPDX-License-Identifier: GPL-2.0+
/*
 * A GDB server for the EL1 guest, on TCP port 2345 of the USB link (scripts/USB_GADGET.md 2.9).
 *
 * The guest stops when the client connects, when it presses Ctrl-C, when a breakpoint the
 * client planted (a BRK with our immediate) is hit, and after each step.  A stop is
 * all-stop: the core that takes it (the owner) freezes every other running vCPU with the
 * doorbell SGI and serves the client from inside its trap handler, polling the USB controller
 * itself, until the client resumes.  Every vCPU is a GDB thread (id = slot + 1); the frozen
 * ones are read from the exception frame they stopped with.
 *
 * Breakpoints are software ones (BRK #GDB_BRK_IMM, planted through the guest's own page tables,
 * so the guest's translation registers are the owner's: kernel addresses are the same on every
 * core, a user address is the owner's process').  A step arms MDSCR_EL1.SS and SPSR.SS and
 * masks the guest's interrupts for the one instruction.  MDCR_EL2.TDE (hv.c) routes every
 * guest debug exception here; anything that is not the stub's is reflected to the guest.
 *
 * Supported packets: ? g G p P m M c s vCont Z0 z0 D k H T qSupported qC qfThreadInfo
 * qsThreadInfo qThreadExtraInfo qAttached qXfer:features:read (target.xml).  Hardware
 * breakpoints and watchpoints, and the FP/SIMD registers, are not offered.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <asm/ptrace.h>
#include <hypervisor.h>
#include <hyp/spinlock.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hv.h"
#include "net.h"
#include "rsp.h"

#define GDB_PORT		2345
#define GDB_PKT_MAX		2048		/* longest packet payload we accept or send */
#define GDB_MEM_MAX		(GDB_PKT_MAX / 2)
#define GDB_MAX_BP		32
#define GDB_BRK_IMM		0x5db
#define GDB_BRK_INSN		(0xd4200000U | (GDB_BRK_IMM << 5))	/* BRK #GDB_BRK_IMM */

#define GDB_SIGINT		2
#define GDB_SIGTRAP		5

#define MDSCR_SS		BIT(0)
#define SPSR_SS			BIT(21)

#define GDB_NREGS		34		/* x0-x30, sp, pc, cpsr */
#define GDB_REG_SP		31
#define GDB_REG_PC		32
#define GDB_REG_CPSR		33

enum gdb_rx_state { RX_IDLE, RX_DATA, RX_CK1, RX_CK2 };
enum gdb_resume { GDB_HALTED, GDB_RESUME, GDB_DETACH };

/*
 * What every core looks at on every exit, alone in its cache lines (hyp_clean_range() works a
 * line at a time).  Written under gdb.stub, read by anyone.
 */
struct gdb_sync {
	u32 halted;		/* all-stop in force */
	u32 owner;		/* slot of the core serving the client, while halted */
	u32 stop_req;		/* a stop was asked for: Ctrl-C, a packet, a connection change */
	u32 report;		/* the stop is to be answered with a stop reply */
} __aligned(64);

/* One per vCPU, written by that core while it is held and by the owner before it is released */
struct gdb_cpu {
	u32 parked;		/* the core is held at EL2 until the stop ends */
	u32 step_req;		/* step one instruction when released */
	u32 stepping;		/* a step is armed: MDSCR_EL1.SS and PSTATE.SS are set */
	u32 sp_dirty;		/* sp_el0/sp_el1 below changed and are to be written back */
	struct pt_regs *regs;	/* the exception frame the vCPU stopped with */
	u64 sp_el0, sp_el1;
	u64 mdscr, daif;	/* the guest's, saved while a step is armed */
} __aligned(64);

/* What the TCP callbacks write, from whichever core polls the USB controller */
struct gdb_rx {
	struct tcp_conn *conn;
	bool attached;				/* a client is connected */
	enum gdb_rx_state state;
	u32 len, sum, ck;
	bool pkt_ready;				/* pkt[0..pkt_len) is a verified packet not yet served */
	u32 pkt_len;
	u8 pkt[GDB_PKT_MAX + 8];
} __aligned(64);

struct gdb_bp {
	u64 va;
	u32 insn;		/* what the BRK replaced */
};

/* Touched by the owner only (and by it alone at a time, under stub) */
struct gdb {
	struct spinlock stub;
	int owner;		/* slot of the core serving the client */
	int sel;		/* slot whose registers g, p, G and P use */
	int hc;			/* slot a plain `s` steps */
	int sig;		/* the signal of the stop being served */
	enum gdb_resume resume;
	u32 step_set;		/* slots that step one instruction when released */
	u8 out[GDB_PKT_MAX + 8];
	u8 mem[GDB_MEM_MAX];
	char hex[GDB_PKT_MAX + 8];
	char xml[1600];
	u32 xml_len;
	struct gdb_bp bp[GDB_MAX_BP];
} __aligned(64);

static __hyp_data struct gdb gdb = { .stub = __SPINLOCK_UNLOCKED };
static __hyp_data struct gdb_rx rx = { 0 };
static __hyp_data struct gdb_sync sync = { 0 };
static __hyp_data struct gdb_cpu cpu[HYP_MAX_CPUS] = { { 0 } };

/* Each of these is read after a refresh and written before a publish, so another core sees it */
static void __hyp gdb_refresh(void)
{
	hyp_clean_range(&gdb, sizeof(gdb));
}

static void __hyp rx_refresh(void)
{
	hyp_clean_range(&rx, sizeof(rx));
}

static struct gdb_sync __hyp *sync_get(void)
{
	hyp_clean_range(&sync, sizeof(sync));

	return &sync;
}

static struct gdb_cpu __hyp *cpu_get(int slot)
{
	hyp_clean_range(&cpu[slot], sizeof(cpu[slot]));

	return &cpu[slot];
}

static void __hyp cpu_put(int slot)
{
	hyp_clean_range(&cpu[slot], sizeof(cpu[slot]));
}

/* A thread id as GDB writes it: hex, or -1 for all; -1 here for "all" and "none given" */
static int __hyp parse_tid(const char **p)
{
	u64 v;

	if (**p == '-' && (*p)[1] == '1') {
		*p += 2;
		return -1;
	}

	return rsp_parse_hex(p, &v) ? (int)v : -1;
}

/* ------------------------------------------------------------------------- */
/* Output                                                                     */
/* ------------------------------------------------------------------------- */

/* Queue bytes for the client, waiting for room: only from the serving loop, never from a callback */
static bool __hyp gdb_send(const void *data, u32 len)
{
	const u8 *p = data;
	u64 t0 = hyp_ticks();

	while (len) {
		u32 n;

		rx_refresh();
		if (!rx.conn || !hyp_tcp_established(rx.conn))
			return false;

		n = hyp_tcp_write(rx.conn, p, len);
		p += n;
		len -= n;
		if (!len)
			break;

		hyp_dwc3_poll();
		if (hyp_ticks_elapsed(t0, 5000000))
			return false;
	}

	return true;
}

/* One packet: $payload#checksum */
static void __hyp gdb_reply_bytes(const char *payload, u32 len)
{
	u8 sum = 0;
	u32 n = 0;

	gdb.out[n++] = '$';
	for (u32 i = 0; i < len && n < GDB_PKT_MAX; i++) {
		gdb.out[n++] = payload[i];
		sum += payload[i];
	}
	gdb.out[n++] = '#';
	gdb.out[n++] = rsp_hexc(sum >> 4);
	gdb.out[n++] = rsp_hexc(sum);

	gdb_send(gdb.out, n);
}

static void __hyp gdb_reply(const char *s)
{
	gdb_reply_bytes(s, strlen(s));
}

static void __hyp gdb_replyf(const char *fmt, ...)
{
	char buf[96];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		gdb_reply_bytes(buf, min_t(int, n, sizeof(buf) - 1));
}

static void __hyp gdb_stop_reply(void)
{
	gdb_replyf("T%02xthread:%x;core:%x;", gdb.sig, gdb.owner + 1, gdb.owner);
}

/* ------------------------------------------------------------------------- */
/* Registers and memory                                                       */
/* ------------------------------------------------------------------------- */

/* SP_EL0 for EL0t and EL1t, SP_EL1 for EL1h */
static bool __hyp sp_is_el0(const struct pt_regs *regs)
{
	return !(regs->spsr & 1);
}

static u64 __hyp reg_get(int n)
{
	struct gdb_cpu *c = cpu_get(gdb.sel);
	struct pt_regs *regs = c->regs;

	if (n < 31)
		return regs->regs[n];
	if (n == GDB_REG_SP)
		return sp_is_el0(regs) ? c->sp_el0 : c->sp_el1;
	if (n == GDB_REG_PC)
		return regs->elr;

	return (u32)regs->spsr;
}

static void __hyp reg_set(int n, u64 v)
{
	struct gdb_cpu *c = cpu_get(gdb.sel);
	struct pt_regs *regs = c->regs;

	if (n < 31) {
		regs->regs[n] = v;
	} else if (n == GDB_REG_SP) {
		if (sp_is_el0(regs))
			c->sp_el0 = v;
		else
			c->sp_el1 = v;
		c->sp_dirty = 1;
	} else if (n == GDB_REG_PC) {
		regs->elr = v;
	} else {
		regs->spsr = (regs->spsr & ~0xffffffffULL) | (u32)v;
	}

	cpu_put(gdb.sel);
}

static u32 __hyp reg_size(int n)
{
	return n == GDB_REG_CPSR ? 4 : 8;
}

/* The state a stopped core's debugger view is made of */
static void __hyp cpu_capture(struct hyp_percpu *pcpu)
{
	struct gdb_cpu *c = cpu_get(pcpu->id);

	c->regs = pcpu->vcpu.regs;
	c->sp_el0 = mrs(sp_el0);
	c->sp_el1 = mrs(sp_el1);
	c->sp_dirty = 0;
	cpu_put(pcpu->id);
}

/* Writes of SP from the debugger go to the core that owns the register, when it resumes */
static void __hyp cpu_apply_sp(struct gdb_cpu *c)
{
	if (c->sp_dirty) {
		msr(sp_el0, c->sp_el0);
		msr(sp_el1, c->sp_el1);
		c->sp_dirty = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* Breakpoints                                                                */
/* ------------------------------------------------------------------------- */

static struct gdb_bp __hyp *bp_find(u64 va)
{
	for (int i = 0; i < GDB_MAX_BP; i++)
		if (gdb.bp[i].va == va)
			return &gdb.bp[i];

	return NULL;
}

static bool __hyp bp_insert(u64 va)
{
	struct gdb_bp *bp = bp_find(va);
	u32 brk = GDB_BRK_INSN;

	if (bp)
		return true;
	bp = bp_find(0);
	if (!bp || (va & 3))
		return false;

	if (hyp_guest_read(va, &bp->insn, 4) || hyp_guest_write(va, &brk, 4))
		return false;

	bp->va = va;

	return true;
}

static bool __hyp bp_remove(u64 va)
{
	struct gdb_bp *bp = bp_find(va);

	if (!bp)
		return true;
	if (hyp_guest_write(va, &bp->insn, 4))
		return false;

	bp->va = 0;

	return true;
}

static void __hyp bp_remove_all(void)
{
	for (int i = 0; i < GDB_MAX_BP; i++)
		if (gdb.bp[i].va)
			bp_remove(gdb.bp[i].va);
}

/* ------------------------------------------------------------------------- */
/* Single step                                                                */
/* ------------------------------------------------------------------------- */

/*
 * One instruction on this core: software step enabled in MDSCR_EL1 and PSTATE.SS set in the
 * frame, with the guest's interrupts masked so that the step ends in the instruction and not
 * in a handler.
 */
static void __hyp step_arm(int slot)
{
	struct gdb_cpu *c = cpu_get(slot);
	struct pt_regs *regs = c->regs;

	c->mdscr = mrs(mdscr_el1);
	c->daif = regs->spsr & (PSR_I_BIT | PSR_F_BIT);
	msr(mdscr_el1, c->mdscr | MDSCR_SS);
	regs->spsr |= SPSR_SS | PSR_I_BIT | PSR_F_BIT;
	c->stepping = 1;
	cpu_put(slot);
}

static void __hyp step_end(struct gdb_cpu *c, struct pt_regs *regs)
{
	msr(mdscr_el1, c->mdscr);
	regs->spsr = (regs->spsr & ~(SPSR_SS | PSR_I_BIT | PSR_F_BIT)) | c->daif;
	c->stepping = 0;
}

/* ------------------------------------------------------------------------- */
/* Packets                                                                    */
/* ------------------------------------------------------------------------- */

static void __hyp gdb_target_xml(void)
{
	int n = snprintf(gdb.xml, sizeof(gdb.xml),
			 "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
			 "<target version=\"1.0\"><architecture>aarch64</architecture>"
			 "<feature name=\"org.gnu.gdb.aarch64.core\">");

	for (int i = 0; i < 31; i++)
		n += snprintf(gdb.xml + n, sizeof(gdb.xml) - n, "<reg name=\"x%d\" bitsize=\"64\"/>", i);

	n += snprintf(gdb.xml + n, sizeof(gdb.xml) - n,
		      "<reg name=\"sp\" bitsize=\"64\" type=\"data_ptr\"/>"
		      "<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>"
		      "<reg name=\"cpsr\" bitsize=\"32\"/></feature></target>");
	gdb.xml_len = n;
}

/* The same description for the other GDB servers (hyp/gdbmem.c) */
const char *__hyp hyp_gdb_target_xml(u32 *len)
{
	*len = gdb.xml_len;

	return gdb.xml;
}

/* qXfer:features:read:target.xml:offset,length */
static void __hyp cmd_xfer_features(const char *p)
{
	u64 off, len;
	u32 n;

	if (!rsp_parse_hex(&p, &off) || *p++ != ',' || !rsp_parse_hex(&p, &len)) {
		gdb_reply("E01");
		return;
	}

	if (off >= gdb.xml_len) {
		gdb_reply("l");
		return;
	}

	n = min_t(u64, len, min_t(u32, gdb.xml_len - off, GDB_PKT_MAX - 1));
	gdb.hex[0] = off + n >= gdb.xml_len ? 'l' : 'm';
	memcpy(gdb.hex + 1, gdb.xml + off, n);
	gdb_reply_bytes(gdb.hex, n + 1);
}

static void __hyp cmd_regs_read(void)
{
	u32 n = 0;

	for (int i = 0; i < GDB_NREGS; i++) {
		u64 v = reg_get(i);

		n += rsp_put_hex(gdb.hex + n, (u8 *)&v, reg_size(i));
	}
	gdb_reply_bytes(gdb.hex, n);
}

static void __hyp cmd_regs_write(const char *p)
{
	for (int i = 0; i < GDB_NREGS; i++) {
		u64 v = 0;

		if (!rsp_get_hex(p, (u8 *)&v, reg_size(i))) {
			gdb_reply("E01");
			return;
		}
		p += 2 * reg_size(i);
		reg_set(i, v);
	}
	gdb_reply("OK");
}

/* p n */
static void __hyp cmd_reg_read(const char *p)
{
	char buf[20];
	u64 n, v;

	if (!rsp_parse_hex(&p, &n) || n >= GDB_NREGS) {
		gdb_reply("E01");
		return;
	}

	v = reg_get(n);
	gdb_reply_bytes(buf, rsp_put_hex(buf, (u8 *)&v, reg_size(n)));
}

/* P n=value */
static void __hyp cmd_reg_write(const char *p)
{
	u64 n, v = 0;

	if (!rsp_parse_hex(&p, &n) || n >= GDB_NREGS || *p++ != '=' ||
	    !rsp_get_hex(p, (u8 *)&v, reg_size(n))) {
		gdb_reply("E01");
		return;
	}

	reg_set(n, v);
	gdb_reply("OK");
}

/* m addr,length */
static void __hyp cmd_mem_read(const char *p)
{
	u64 addr, len;
	u32 n;

	if (!rsp_parse_hex(&p, &addr) || *p++ != ',' || !rsp_parse_hex(&p, &len)) {
		gdb_reply("E01");
		return;
	}

	n = min_t(u64, len, GDB_MEM_MAX);
	if (hyp_guest_read(addr, gdb.mem, n)) {
		gdb_reply("E14");
		return;
	}

	gdb_reply_bytes(gdb.hex, rsp_put_hex(gdb.hex, gdb.mem, n));
}

/* M addr,length:data */
static void __hyp cmd_mem_write(const char *p)
{
	u64 addr, len;

	if (!rsp_parse_hex(&p, &addr) || *p++ != ',' || !rsp_parse_hex(&p, &len) || *p++ != ':' ||
	    len > GDB_MEM_MAX || !rsp_get_hex(p, gdb.mem, len)) {
		gdb_reply("E01");
		return;
	}

	gdb_reply(hyp_guest_write(addr, gdb.mem, len) ? "E14" : "OK");
}

/* Z0,addr,kind and z0,addr,kind */
static void __hyp cmd_breakpoint(const char *p, bool insert)
{
	u64 addr;

	if (*p++ != '0' || *p++ != ',' || !rsp_parse_hex(&p, &addr)) {
		gdb_reply("");		/* hardware breakpoints and watchpoints: not offered */
		return;
	}

	gdb_reply((insert ? bp_insert(addr) : bp_remove(addr)) ? "OK" : "E14");
}

/* H op thread-id: g selects the registers' thread, c the one a plain step steps */
static void __hyp cmd_thread(const char *p)
{
	char op = *p++;
	int tid = parse_tid(&p);
	int slot = tid > 0 ? tid - 1 : gdb.owner;

	if (tid > 0 && (slot >= hyp_cpu_count() || !(slot == gdb.owner || hyp_smp_vcpu_on(slot)))) {
		gdb_reply("E01");
		return;
	}

	if (op == 'g')
		gdb.sel = slot;
	else if (op == 'c')
		gdb.hc = slot;
	gdb_reply("OK");
}

/* Resume the guest; the slots in @steps run one instruction and stop again */
static void __hyp resume_with(u32 steps)
{
	gdb.step_set = steps;
	gdb.resume = GDB_RESUME;
}

/* c [addr] and s [addr] */
static void __hyp cmd_resume(char action, const char *p)
{
	u64 addr;

	if (rsp_parse_hex(&p, &addr)) {
		cpu_get(gdb.hc)->regs->elr = addr;
		cpu_put(gdb.hc);
	}

	resume_with(action == 's' ? BIT(gdb.hc) : 0);
}

/* vCont;action[:thread][;action[:thread]...] */
static void __hyp cmd_vcont(const char *p)
{
	u32 steps = 0;

	while (*p) {
		char a = *p++;
		int tid;

		if (a != 'c' && a != 'C' && a != 's' && a != 'S') {
			gdb_reply("");
			return;
		}
		if (a == 'C' || a == 'S') {
			u64 sig;

			rsp_parse_hex(&p, &sig);	/* the signal: the guest is not handed one */
		}
		tid = *p == ':' ? (p++, parse_tid(&p)) : -1;

		if ((a == 's' || a == 'S') && tid <= 0)
			steps |= BIT(gdb.hc);
		else if (a == 's' || a == 'S')
			steps |= BIT(tid - 1);

		if (*p == ';')
			p++;
	}

	resume_with(steps);
}

static void __hyp cmd_v(const char *p)
{
	if (!strcmp(p, "vCont?"))
		gdb_reply("vCont;c;C;s;S");
	else if (!strncmp(p, "vCont;", 6))
		cmd_vcont(p + 6);
	else
		gdb_reply("");
}

/* qfThreadInfo: the vCPUs the guest runs */
static void __hyp cmd_thread_list(void)
{
	int n = 0;

	gdb.hex[n++] = 'm';
	for (int s = 0; s < hyp_cpu_count(); s++) {
		if (s != gdb.owner && !hyp_smp_vcpu_on(s))
			continue;
		n += snprintf(gdb.hex + n, 16, "%s%x", n > 1 ? "," : "", s + 1);
	}
	gdb_reply_bytes(gdb.hex, n);
}

/* qThreadExtraInfo,tid: the text `info threads` shows */
static void __hyp cmd_thread_info(const char *p)
{
	char buf[16];
	int n = snprintf(buf, sizeof(buf), "cpu%d", parse_tid(&p) - 1);

	gdb_reply_bytes(gdb.hex, rsp_put_hex(gdb.hex, (u8 *)buf, n));
}

static void __hyp cmd_query(const char *p)
{
	static const char __hyp_rodata features[] = "qXfer:features:read:target.xml:";
	static const char __hyp_rodata extra[] = "qThreadExtraInfo,";

	if (!strncmp(p, "qSupported", 10))
		gdb_replyf("PacketSize=%x;qXfer:features:read+;vContSupported+", GDB_PKT_MAX);
	else if (!strcmp(p, "qC"))
		gdb_replyf("QC%x", gdb.owner + 1);
	else if (!strcmp(p, "qfThreadInfo"))
		cmd_thread_list();
	else if (!strcmp(p, "qsThreadInfo"))
		gdb_reply("l");
	else if (!strcmp(p, "qAttached"))
		gdb_reply("1");
	else if (!strncmp(p, features, sizeof(features) - 1))
		cmd_xfer_features(p + sizeof(features) - 1);
	else if (!strncmp(p, extra, sizeof(extra) - 1))
		cmd_thread_info(p + sizeof(extra) - 1);
	else
		gdb_reply("");
}

static void __hyp gdb_command(char *p, u32 len)
{
	p[len] = '\0';

	switch (p[0]) {
	case '?':
		gdb_stop_reply();
		break;
	case 'g':
		cmd_regs_read();
		break;
	case 'G':
		cmd_regs_write(p + 1);
		break;
	case 'p':
		cmd_reg_read(p + 1);
		break;
	case 'P':
		cmd_reg_write(p + 1);
		break;
	case 'm':
		cmd_mem_read(p + 1);
		break;
	case 'M':
		cmd_mem_write(p + 1);
		break;
	case 'c':
	case 's':
		cmd_resume(p[0], p + 1);
		break;
	case 'v':
		cmd_v(p);
		break;
	case 'Z':
		cmd_breakpoint(p + 1, true);
		break;
	case 'z':
		cmd_breakpoint(p + 1, false);
		break;
	case 'H':
		cmd_thread(p + 1);
		break;
	case 'T': {
		const char *q = p + 1;
		int tid = parse_tid(&q);

		gdb_reply(tid > 0 && tid <= hyp_cpu_count() &&
			  (tid - 1 == gdb.owner || hyp_smp_vcpu_on(tid - 1)) ? "OK" : "E01");
		break;
	}
	case 'q':
		cmd_query(p);
		break;
	case 'D':
		gdb_reply("OK");
		gdb.resume = GDB_DETACH;
		break;
	case 'k':
		gdb.resume = GDB_DETACH;
		break;
	default:
		gdb_reply("");		/* not supported */
	}
}

/* ------------------------------------------------------------------------- */
/* Receiving: the TCP callbacks, which only collect bytes                     */
/* ------------------------------------------------------------------------- */

static void __hyp gdb_ack(char c)
{
	if (rx.conn)
		hyp_tcp_write(rx.conn, &c, 1);
}

static void __hyp gdb_request_stop(bool report)
{
	struct gdb_sync *s = sync_get();

	s->stop_req = 1;
	s->report = report;
	hyp_clean_range(s, sizeof(*s));
}

/*
 * A packet is served by the stub, so one that arrives while the guest runs (a client that
 * sends before the stop it asked for has happened) is kept and makes the guest stop; Ctrl-C
 * only means something while it runs.
 */
static void __hyp gdb_rx_byte(u8 c, bool halted)
{
	switch (rx.state) {
	case RX_IDLE:
		if (c == 0x03 && !halted) {
			gdb_request_stop(true);
		} else if (c == '$' && !rx.pkt_ready) {
			rx.state = RX_DATA;
			rx.len = 0;
			rx.sum = 0;
		}
		break;
	case RX_DATA:
		if (c == '#') {
			rx.state = RX_CK1;
		} else {
			rx.sum += c;
			if (rx.len < GDB_PKT_MAX)
				rx.pkt[rx.len++] = c;
		}
		break;
	case RX_CK1:
		rx.ck = (u32)(rsp_unhex(c) & 0xf) << 4;
		rx.state = RX_CK2;
		break;
	case RX_CK2:
		rx.ck |= rsp_unhex(c) & 0xf;
		rx.state = RX_IDLE;
		if ((u8)rx.sum == rx.ck) {
			gdb_ack('+');
			rx.pkt_len = rx.len;
			rx.pkt_ready = true;
			if (!halted)
				gdb_request_stop(false);
		} else {
			gdb_ack('-');
		}
		break;
	}
}

static void __hyp gdb_connect(struct tcp_conn *c)
{
	rx_refresh();
	rx.conn = c;
	rx.attached = true;
	rx.state = RX_IDLE;
	rx.pkt_ready = false;
	rx_refresh();

	/* the client expects a stopped target to attach to */
	gdb_request_stop(false);
	hprintf("gdb: client connected\n");
}

static void __hyp gdb_data(struct tcp_conn *c, const u8 *data, u32 len)
{
	bool halted = sync_get()->halted;

	rx_refresh();
	for (u32 i = 0; i < len; i++)
		gdb_rx_byte(data[i], halted);
	rx_refresh();
}

static void __hyp gdb_close(struct tcp_conn *c)
{
	rx_refresh();
	rx.attached = false;
	rx.conn = NULL;
	rx_refresh();

	/* a stop clears the breakpoints out of the guest and lets it run */
	gdb_request_stop(false);
	hprintf("gdb: client gone\n");
}

/* ------------------------------------------------------------------------- */
/* Stopping the guest                                                         */
/* ------------------------------------------------------------------------- */

/* Every other vCPU the guest runs goes to EL2 on the doorbell and waits in hyp_gdb_park() */
static void __hyp gdb_halt_others(int me)
{
	u64 t0 = hyp_ticks();
	int n = hyp_cpu_count();

	for (int s = 0; s < n; s++)
		if (s != me && hyp_smp_vcpu_on(s))
			hyp_gic_doorbell(s);

	for (int s = 0; s < n; s++) {
		if (s == me || !hyp_smp_vcpu_on(s))
			continue;

		while (!cpu_get(s)->parked) {
			if (hyp_ticks_elapsed(t0, 500000)) {
				hprintf("gdb: cpu%d did not stop\n", s);
				break;
			}
		}
	}
}

/**
 * hyp_gdb_park() - hold this core while another one serves the debugger
 * @pcpu: the core that took the exit
 *
 * Called on the way out of every exit.  Does nothing unless a stop is in force.  The core
 * leaves its exception frame where the owner can read and change it, and carries out what the
 * owner decided for it (a step, a new stack pointer) when it is released.
 */
void __hyp hyp_gdb_park(struct hyp_percpu *pcpu)
{
	int me = pcpu->id;
	struct gdb_cpu *c;
	struct gdb_sync *s = sync_get();

	if (!s->halted || s->owner == me)
		return;

	cpu_capture(pcpu);
	c = cpu_get(me);
	c->parked = 1;
	c->step_req = 0;
	cpu_put(me);

	while (sync_get()->halted)
		;

	c = cpu_get(me);
	cpu_apply_sp(c);
	c->parked = 0;
	cpu_put(me);
	if (cpu_get(me)->step_req)
		step_arm(me);
}

/*
 * The guest has stopped on this core: stop the others, answer the client if it is waiting
 * for an answer, and serve it until it resumes the guest.
 */
static void __hyp gdb_stop(struct hyp_percpu *pcpu, int sig, bool report)
{
	struct gdb_sync *s;
	bool attached;
	int me = pcpu->id;

	if (!spin_trylock(&gdb.stub)) {
		hyp_gdb_park(pcpu);
		return;
	}

	s = sync_get();
	s->halted = 1;
	s->owner = me;
	s->stop_req = 0;
	hyp_clean_range(s, sizeof(*s));

	gdb_refresh();
	gdb.owner = gdb.sel = gdb.hc = me;
	gdb.sig = sig;
	gdb.resume = GDB_HALTED;
	cpu_capture(pcpu);

	gdb_halt_others(me);

	rx_refresh();
	if (rx.attached && report)
		gdb_stop_reply();

	for (;;) {
		hyp_dwc3_poll();

		rx_refresh();
		attached = rx.attached;
		if (!attached)
			break;
		if (rx.pkt_ready) {
			gdb_command((char *)rx.pkt, rx.pkt_len);
			rx_refresh();
			rx.pkt_ready = false;
			rx_refresh();
		}
		if (gdb.resume != GDB_HALTED)
			break;
	}

	if (!attached || gdb.resume == GDB_DETACH) {
		bp_remove_all();
		rx_refresh();
		if (rx.conn)
			hyp_tcp_close(rx.conn);
		rx.attached = false;
		rx.conn = NULL;
		rx_refresh();
		gdb.step_set = 0;
		hprintf("gdb: detached\n");
	}

	/* Everyone carries on; the ones asked to step arm it as they leave */
	cpu_apply_sp(cpu_get(me));
	cpu_put(me);
	for (int slot = 0; slot < hyp_cpu_count(); slot++) {
		if (!(gdb.step_set & BIT(slot)))
			continue;
		if (slot == me) {
			step_arm(me);
		} else {
			cpu_get(slot)->step_req = 1;
			cpu_put(slot);
		}
	}

	hyp_clean_range(&gdb, sizeof(gdb));
	s = sync_get();
	s->halted = 0;
	hyp_clean_range(s, sizeof(*s));
	spin_unlock(&gdb.stub);
}

/**
 * hyp_gdb_debug_exception() - a debug exception from the guest
 * @pcpu: the core that took it
 *
 * Return: true when it was the stub's (a breakpoint it planted, or the end of its step) and has
 * been served, false when it is the guest's own and has to be reflected.
 */
bool __hyp hyp_gdb_debug_exception(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	struct gdb_cpu *c = cpu_get(pcpu->id);
	u32 ec = (regs->esr >> 26) & 0x3f;

	rx_refresh();

	if (ec == 0x3c) {
		if (!rx.attached || (regs->esr & 0xffff) != GDB_BRK_IMM)
			return false;
	} else if (ec == 0x32 && c->stepping) {
		step_end(c, regs);
		cpu_put(pcpu->id);
		if (!rx.attached)
			return true;
	} else {
		return false;
	}

	gdb_stop(pcpu, GDB_SIGTRAP, true);

	return true;
}

/**
 * hyp_gdb_poll() - enter the stub when a stop was asked for
 * @pcpu: the core that is about to return to the guest
 *
 * Ctrl-C, a packet, a new connection and a lost one all arrive as bytes in the USB poll that
 * ran on some core's exit; the stop itself has to start from outside that poll, here.
 */
void __hyp hyp_gdb_poll(struct hyp_percpu *pcpu)
{
	struct pt_regs *regs = pcpu->vcpu.regs;
	struct gdb_sync *s;

	if ((regs->spsr & PSR_MODE_MASK) > PSR_MODE_EL1h)
		return;

	s = sync_get();
	if (!s->stop_req || s->halted)
		return;

	gdb_stop(pcpu, s->report ? GDB_SIGINT : GDB_SIGTRAP, s->report);
}

static const struct tcp_service __hyp_rodata gdb_service = {
	.port = GDB_PORT,
	.name = "gdb",
	.description = "GDB server for the guest CPU (all-stop, one thread per vCPU)",
	.on_connect = gdb_connect,
	.on_data = gdb_data,
	.on_close = gdb_close,
};

void __hyp hyp_gdb_init(void)
{
	gdb_target_xml();
	gdb_refresh();
	hyp_tcp_listen(&gdb_service);
}
