// SPDX-License-Identifier: GPL-2.0+
/*
 * A line console on TCP port 2323 (scripts/USB_GADGET.md): the hypervisor's own
 * commands, reachable over the USB link without a serial adapter.  Telnet option
 * negotiation is ignored (the client stays in line mode with local echo).
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hv.h"
#include "net.h"
#include "dwc3.h"

#define TELNET_PORT	2323
#define LINE_MAX	96

#define IAC	255
#define SB	250
#define SE	240

enum { JOB_NONE, JOB_LIST, JOB_GET, JOB_DIFF };

struct tn {
	char line[LINE_MAX];
	u32 len;
	u8 iac;			/* 0 normal, 1 after IAC, 2 after IAC WILL/WONT/DO/DONT, 3 in SB */
	bool saw_cr;
	/* a listing that is longer than the connection's window is sent as it goes: see hyp_telnet_tick() */
	struct tcp_conn *conn;
	u8 job;
	bool force;
	u32 cursor;
	char part[40];
};

static __hyp_data struct tn tn = { { 0 } };

static void __hyp tn_puts(struct tcp_conn *c, const char *s)
{
	hyp_tcp_write(c, s, strlen(s));
}

static void __hyp tn_prompt(struct tcp_conn *c)
{
	tn_puts(c, "hyp> ");
}

static void __hyp tn_printf(struct tcp_conn *c, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		hyp_tcp_write(c, buf, min_t(int, n, sizeof(buf) - 1));
}

/* Split @s at its spaces, in place; returns how many words */
static int __hyp tn_words(char *s, char *words[], int max)
{
	int n = 0;

	while (*s && n < max) {
		while (*s == ' ')
			*s++ = '\0';
		if (!*s)
			break;
		words[n++] = s;
		while (*s && *s != ' ')
			s++;
	}

	return n;
}

static bool __hyp tn_job_run(struct tcp_conn *c, int max);

static void __hyp tn_res_error(struct tcp_conn *c, int err)
{
	if (err == -ENOENT)
		tn_puts(c, "no resource has that name (res list PART)\r\n");
	else if (err == -EEXIST)
		tn_puts(c, "more than one resource has that in its name (res list PART)\r\n");
	else
		tn_printf(c, "failed: %d\r\n", err);
}

/* `res ...`: the system resources (hyp/res.c, scripts/RESOURCES.md) */
static void __hyp tn_res(struct tcp_conn *c, char *args)
{
	static const char __hyp_rodata usage[] =
		"res list [PART]            the resources (whose name has PART in it)\r\n"
		"res get PART               the state of those\r\n"
		"res on|off NAME            switch a branch clock, power domain or regulator\r\n"
		"res set NAME [FIELD] VALUE write a register, an ARC level (an index into its levels, or `max`),\r\n"
		"                           a BCM vote word; a regulator's FIELD is voltage (mV), enable or mode\r\n"
		"res snap | res diff        remember every register's value / show what changed since\r\n"
		"a register in the LPASS is not touched while the Hexagon runs, or one behind a power domain\r\n"
		"that is off: add `force` to do it anyway (the board may reset)\r\n";
	char *w[6], buf[160];
	int argc = tn_words(args, w, ARRAY_SIZE(w)), idx, ret;
	bool force = argc && !strcmp(w[argc - 1], "force");

	if (force)
		argc--;
	if (!argc) {
		tn_printf(c, "%u resources\r\n", hyp_res_count());
		tn_puts(c, usage);
		return;
	}

	if (!strcmp(w[0], "list") || !strcmp(w[0], "get") || !strcmp(w[0], "diff")) {
		bool diff = !strcmp(w[0], "diff");

		if (!strcmp(w[0], "get") && argc < 2) {
			tn_puts(c, "res get PART\r\n");
			return;
		}
		tn.job = diff ? JOB_DIFF : w[0][0] == 'l' ? JOB_LIST : JOB_GET;
		tn.cursor = 0;
		tn.force = force;
		strlcpy(tn.part, argc > 1 ? w[1] : "", sizeof(tn.part));
		tn_job_run(c, 400);	/* what the window takes now; hyp_telnet_tick() sends the rest */
	} else if (!strcmp(w[0], "snap")) {
		tn_printf(c, "%u registers remembered\r\n", hyp_res_snapshot(force));
	} else if ((!strcmp(w[0], "on") || !strcmp(w[0], "off")) && argc == 2) {
		idx = hyp_res_find(w[1]);
		if (idx < 0) {
			tn_res_error(c, idx);
			return;
		}
		ret = hyp_res_enable(idx, w[0][1] == 'n', force);
		if (ret == -EACCES)
			tn_printf(c, "refused: %s (add `force` to do it anyway)\r\n", hyp_res_guard(idx));
		else if (ret)
			tn_printf(c, "%s: %d\r\n", w[0], ret);
		hyp_res_describe(idx, buf, sizeof(buf), true);
		tn_printf(c, "%s\r\n", buf);
	} else if (!strcmp(w[0], "set") && (argc == 3 || argc == 4)) {
		idx = hyp_res_find(w[1]);
		if (idx < 0) {
			tn_res_error(c, idx);
			return;
		}
		ret = hyp_res_set(idx, argc == 4 ? w[2] : NULL,
				  !strcmp(w[argc - 1], "max") ? ~0ULL : simple_strtoull(w[argc - 1], NULL, 0), force);
		if (ret == -EACCES)
			tn_printf(c, "refused: %s (add `force` to do it anyway)\r\n", hyp_res_guard(idx));
		else if (ret)
			tn_printf(c, "set: %d\r\n", ret);
		hyp_res_describe(idx, buf, sizeof(buf), true);
		tn_printf(c, "%s\r\n", buf);
	} else {
		tn_puts(c, usage);
	}
}

/* The next line of the listing that is going out: false when there are no more */
static bool __hyp tn_job_line(struct tcp_conn *c)
{
	char buf[160];
	int n;

	if (tn.job == JOB_DIFF) {
		n = hyp_res_diff_line(&tn.cursor, buf, sizeof(buf), tn.force);
		if (!n)
			return false;
		tn_printf(c, "%s\r\n", buf);
		return true;
	}

	while (tn.cursor < hyp_res_count()) {
		int idx = tn.cursor++;

		if (*tn.part && !strstr(hyp_res_name(idx), tn.part))
			continue;
		if (tn.job == JOB_LIST)
			hyp_res_label(idx, buf, sizeof(buf));
		else
			hyp_res_describe(idx, buf, sizeof(buf), tn.force);
		tn_printf(c, "%s\r\n", buf);
		return true;
	}

	return false;
}

/* Send up to @max lines of the listing; true when it is finished */
static bool __hyp tn_job_run(struct tcp_conn *c, int max)
{
	for (int i = 0; tn.job && i < max && hyp_tcp_room(c) >= 256; i++) {
		if (!tn_job_line(c)) {
			tn.job = JOB_NONE;
			return true;
		}
	}

	return !tn.job;
}

/**
 * hyp_telnet_tick() - send more of the listing the client asked for
 *
 * Called with every poll of the controller: a few lines each time, as the connection's window
 * has room, so a listing of hundreds of resources does not need the window to hold it.
 */
void __hyp hyp_telnet_tick(void)
{
	struct tcp_conn *c = tn.conn;

	if (c && tn.job && tn_job_run(c, 4))
		tn_prompt(c);
}

static void __hyp tn_command(struct tcp_conn *c, char *line)
{
	while (*line == ' ')
		line++;

	if (!*line)
		return;

	if (!strcmp(line, "help")) {
		tn_puts(c, "help      this list\r\n"
			   "info      the services on this link and the guest's state\r\n"
			   "stats     USB controller and network counters\r\n"
			   "boot      let the halted guest carry on and start the OS\r\n"
			   "uptime    seconds since the counter started\r\n"
			   "pas       the remote processors EL2 loads, and the SMMU's fault state\r\n"
			   "pas smoke arm/disarm a diagnostic program for the next processor start\r\n"
			   "pas scan  look for what the processor wrote in its zero-filled memory\r\n"
			   "pas smmu  toggle the processors' SMMU streams between bypass and fault\r\n"
			   "pas leave toggle: a shutdown touches nothing and leaves the processor running\r\n"
			   "pas vmid N set the VMID the processors' bypass streams carry (the memory protection units look at it)\r\n"
			   "pas ctx N  translate the processor's memory accesses for N ms after its start and log what it touches\r\n"
			   "pas ropll [N|off] when the always-on PLL comes up: before the start (default), N ms after it, never\r\n"
			   "pas run N [M [P]] stop the processor N ms after it started (0: let it run); with M, send it an NMI (once bit 31 of GCC's DSP GDSC vote reads P, if given) and wait M ms; it is then left running if `pas leave` is on\r\n"
			   "pas watch N print the processor's SMMU streams and every fault for N ms after its start\r\n"
			   "pas poll N sample the processor's power and bus status for N ms after its start\r\n"
			   "pas tz    send the PAS calls to the secure world instead of answering them\r\n"
			   "log uart [on|off] the hypervisor log goes to the debug UART as well as to the :2222 console (default on)\r\n"
			   "res ...    the system resources: clocks, power domains, RPMh votes (`res` for the list)\r\n"
			   "quit      close the connection\r\n");
	} else if (!strcmp(line, "info")) {
		const struct tcp_service *svc;
		bool up;

		tn_printf(c, "guest: %s\r\n", hyp_usb_boot_waiting() ? "halted, waiting for `boot`" :
			  "running");
		tn_printf(c, "%-10s %-5s %-6s %s\r\n", "service", "port", "client", "description");
		for (int i = 0; (svc = hyp_tcp_service(i, &up)); i++)
			tn_printf(c, "%-10s %-5u %-6s %s\r\n", svc->name, svc->port, up ? "yes" : "-",
				  svc->description);
	} else if (!strcmp(line, "boot")) {
		tn_puts(c, hyp_usb_boot_release() ? "booting the guest\r\n" :
			   "the guest is not waiting for `boot`\r\n");
	} else if (!strcmp(line, "stats")) {
		char buf[192];

#if CONFIG_IS_ENABLED(HYP_USB)
		hyp_dwc3_describe(buf, sizeof(buf));
		tn_printf(c, "%s\r\n", buf);
#endif
		hyp_net_describe(buf, sizeof(buf));
		tn_printf(c, "%s\r\n", buf);
		hyp_vdev_describe(buf, sizeof(buf));
		tn_printf(c, "%s\r\n", buf);
	} else if (!strncmp(line, "log uart", 8)) {
		if (!strcmp(line + 8, " on"))
			hyp_log_uart_set(true);
		else if (!strcmp(line + 8, " off"))
			hyp_log_uart_set(false);
		tn_printf(c, "the hypervisor log also goes to the debug UART: %s\r\n", hyp_log_uart() ? "on" : "off");
	} else if (!strcmp(line, "pas smoke")) {
		tn_printf(c, "the next processor start runs the smoke test: %s\r\n",
			  hyp_pas_toggle_smoke() ? "armed" : "disarmed");
	} else if (!strcmp(line, "pas smmu")) {
		tn_printf(c, "the processors' SMMU streams now: %s\r\n", hyp_smmu_toggle_fault() ? "fault" : "bypass");
	} else if (!strcmp(line, "pas leave")) {
		tn_printf(c, "a shutdown leaves the core running: %s\r\n", hyp_pas_toggle_leave() ? "yes" : "no");
	} else if (!strncmp(line, "pas vmid ", 9)) {
		u32 vmid = 0;

		for (const char *p = line + 9; *p >= '0' && *p <= '9'; p++)
			vmid = vmid * 10 + *p - '0';
		hyp_smmu_set_vmid(vmid);
		tn_printf(c, "the processors' bypass streams carry VMID %u\r\n", vmid & 0xff);
	} else if (!strncmp(line, "pas ctx ", 8)) {
		u32 ms = 0;

		for (const char *p = line + 8; *p >= '0' && *p <= '9'; p++)
			ms = ms * 10 + *p - '0';
		hyp_pas_set_ctx_ms(ms);
		tn_printf(c, "the next processor start goes through an EL2 SMMU context for: %u ms (0: not)\r\n", ms);
	} else if (!strncmp(line, "pas ropll", 9)) {
		u32 ms = 0;
		const char *p = line + 9;

		if (!strcmp(p, " off")) {
			ms = 0xffffffffU;
		} else {
			for (; *p == ' '; p++)
				;
			for (; *p >= '0' && *p <= '9'; p++)
				ms = ms * 10 + *p - '0';
		}
		hyp_pas_set_ro_pll(ms);
		tn_printf(c, "the always-on PLL comes up: %s\r\n", ms == 0xffffffffU ? "never" :
			  ms ? "after the processor started" : "before the processor starts");
	} else if (!strncmp(line, "pas watch ", 10)) {
		u32 ms = 0;

		for (const char *p = line + 10; *p >= '0' && *p <= '9'; p++)
			ms = ms * 10 + *p - '0';
		hyp_pas_set_watch_ms(ms);
		tn_printf(c, "the next processor start prints its SMMU state and faults for %u ms\r\n", ms);
	} else if (!strncmp(line, "pas poll ", 9)) {
		u32 ms = 0;

		for (const char *p = line + 9; *p >= '0' && *p <= '9'; p++)
			ms = ms * 10 + *p - '0';
		hyp_pas_set_poll_ms(ms);
		tn_printf(c, "the next processor start samples the core's power and bus status for %u ms\r\n", ms);
	} else if (!strcmp(line, "pas stress-ropll")) {
		hyp_pas_ropll_stress();
		tn_puts(c, "done: see the hypervisor log\r\n");
	} else if (!strncmp(line, "pas ring ", 9)) {
		u64 pa = 0;
		u32 ms = 0;
		const char *p = line + 9;

		if (p[0] == '0' && p[1] == 'x')
			p += 2;
		for (; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++)
			pa = pa * 16 + (*p <= '9' ? *p - '0' : *p - 'a' + 10);
		while (*p == ' ')
			p++;
		for (; *p >= '0' && *p <= '9'; p++)
			ms = ms * 10 + *p - '0';
		hyp_pas_set_ring(pa, ms);
		tn_printf(c, "the next processor start watches the tracer ring at %#llx for %u ms\r\n", pa, ms);
	} else if (!strncmp(line, "pas run ", 8)) {
		u32 ms = 0, nmi_ms = 0, phase = 2;
		const char *p = line + 8;

		for (; *p >= '0' && *p <= '9'; p++)
			ms = ms * 10 + *p - '0';
		if (*p == ' ')
			for (p++; *p >= '0' && *p <= '9'; p++)
				nmi_ms = nmi_ms * 10 + *p - '0';
		if (*p == ' ')
			phase = p[1] - '0';
		hyp_pas_set_run_ms(ms, nmi_ms, phase);
		tn_printf(c, "the next processor start is stopped after: %u ms (0: not stopped), NMI first and %u ms wait: %s, GCC vote bit 31 = %u (2: any)\r\n",
			  ms, nmi_ms, nmi_ms ? "yes" : "no", phase);
	} else if (!strcmp(line, "pas tz")) {
		tn_printf(c, "the PAS calls go to: %s\r\n", hyp_pas_toggle_tz() ? "the secure world" : "EL2");
	} else if (!strcmp(line, "pas scan")) {
		hyp_pas_scan();
		tn_puts(c, "scanned: see the hypervisor console\r\n");
	} else if (!strcmp(line, "pas")) {
		char buf[384];

		hyp_pas_describe(buf, sizeof(buf));
		tn_puts(c, buf);
		hyp_smmu_describe(buf, sizeof(buf));
		tn_printf(c, "%s\r\n", buf);
	} else if (!strncmp(line, "res", 3) && (!line[3] || line[3] == ' ')) {
		tn_res(c, line + 3);
	} else if (!strcmp(line, "uptime")) {
		tn_printf(c, "%llu us\r\n", hyp_ticks_us());
	} else if (!strcmp(line, "quit") || !strcmp(line, "exit")) {
		tn_puts(c, "bye\r\n");
		hyp_tcp_close(c);
	} else {
		tn_printf(c, "unknown command '%s' (try help)\r\n", line);
	}
}

static void __hyp tn_connect(struct tcp_conn *c)
{
	memset(&tn, 0, sizeof(tn));
	tn.conn = c;
	tn_puts(c, "u-boot-hyp debug console\r\n");
	tn_prompt(c);
}

static void __hyp tn_data(struct tcp_conn *c, const u8 *data, u32 len)
{
	for (u32 i = 0; i < len; i++) {
		u8 b = data[i];

		switch (tn.iac) {
		case 1:
			if (b >= 251 && b <= 254)
				tn.iac = 2;
			else if (b == SB)
				tn.iac = 3;
			else
				tn.iac = 0;
			continue;
		case 2:
			tn.iac = 0;
			continue;
		case 3:
			if (b == IAC)
				tn.iac = 1;	/* IAC SE ends the subnegotiation; IAC IAC is data we drop */
			continue;
		}

		if (b == IAC) {
			tn.iac = 1;
			continue;
		}

		if (b == '\r' || b == '\n') {
			if (b == '\n' && tn.saw_cr) {
				tn.saw_cr = false;
				continue;
			}
			tn.saw_cr = b == '\r';
			tn.line[tn.len] = '\0';
			if (tn.job) {
				/* the listing that is still going out is dropped for the new command */
				tn.job = JOB_NONE;
				tn_puts(c, "^C\r\n");
			}
			tn_command(c, tn.line);
			tn.len = 0;
			if (!tn.job)
				tn_prompt(c);
		} else if (b == 0x08 || b == 0x7f) {
			if (tn.len)
				tn.len--;
			tn.saw_cr = false;
		} else if (b >= 0x20 && b < 0x7f && tn.len < LINE_MAX - 1) {
			tn.line[tn.len++] = b;
			tn.saw_cr = false;
		}
	}
}

static void __hyp tn_close(struct tcp_conn *c)
{
	tn.len = 0;
	tn.job = JOB_NONE;
	tn.conn = NULL;
}

static const struct tcp_service __hyp_rodata telnet_service = {
	.port = TELNET_PORT,
	.name = "console",
	.description = "hypervisor console: help, info, stats, boot, uptime, pas",
	.on_connect = tn_connect,
	.on_data = tn_data,
	.on_close = tn_close,
};

void __hyp hyp_telnet_init(void)
{
	hyp_tcp_listen(&telnet_service);
}
