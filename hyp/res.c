// SPDX-License-Identifier: GPL-2.0+
/*
 * The hypervisor's table of system resources: what can be looked at and switched from EL2.
 *
 * A resource is one named thing with a state:
 *
 *  - register resources, described in the board's device tree under /hypervisor-resources (made
 *    by scripts/mk-resources.py from scripts/resources.txt): a branch clock (CBCR), a power domain
 *    (GDSCR) or a plain register, each at an offset in a register window;
 *  - RPMh resources, found in the command DB at boot: ARC levels (cx.lvl, lcx.lvl, ...), BCM
 *    bandwidth votes (MC0, SH0, ...) and regulators (ldoc1, smpb2, ...), read and voted on EL2's own
 *    DRV (hyp/rpmh.c), which the guest's DRV cannot take back.
 *
 * Names are `WINDOW.RESOURCE` (lpass_aon_cc.lpi_noc_hs) and `rpmh.NAME` (rpmh.cx.lvl).  The API
 * looks a resource up, describes its state in one line, switches it on or off, sets a value, and
 * keeps a snapshot of every register-backed resource to show what changed since (hyp_res_snapshot(),
 * hyp_res_diff_line()): the console (hyp/telnet.c, `res`) is its user.  scripts/RESOURCES.md.
 *
 * Two things can make an access to a register window fatal for the board instead of failing: the
 * window is behind a power domain that is off (the bus never answers), or it is in the LPASS while
 * the Hexagon runs (its register space answers only some of the time, and the board resets on a
 * timeout, see scripts/ADSP.md).  A window names the power domain it needs (`hypervisor-res-needs`)
 * and whether it is in the LPASS (`hypervisor-res-guard`), and an access that either rules out is
 * refused unless the caller forces it.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <stdio.h>
#include <string.h>

#include "hv.h"

#define RES_MAX_WINDOWS		24
#define RES_MAX			1024
#define RES_NAME		40

/* CBCR: the branch clock's control and status */
#define CBCR_ENABLE		BIT(0)
#define CBCR_HW_CTL		BIT(1)
#define CBCR_OFF		BIT(31)
/* GDSCR: the power domain's */
#define GDSCR_SW_COLLAPSE	BIT(0)
#define GDSCR_PWR_ON		BIT(31)
#define GDSCR_STATE		GENMASK(27, 24)

#define RES_WAIT_US		1000

/* The command DB, as hyp/rpmh.c and drivers/soc/qcom/cmd-db.c read it */
#define CMD_DB_MAX_SLV		8
#define CMD_DB_SLV_SHIFT	16
#define CMD_DB_SLV_MASK		0x7

struct cmd_db_rsc_hdr {
	u16 slv_id, header_offset, data_offset, cnt, version, reserved[3];
};

struct cmd_db_header {
	u32 version;
	u8 magic[4];
	struct cmd_db_rsc_hdr header[CMD_DB_MAX_SLV];
	u32 checksum, reserved;
	u8 data[];
};

struct cmd_db_entry {
	u8 id[8];
	u32 priority[2];
	u32 addr;
	u16 len, offset;
};

static const u8 __hyp_rodata cmd_db_magic[4] = { 0xdb, 0x30, 0x03, 0x0c };

/* The RPMh resource address' slave id: which kind of resource it is */
enum {
	SLAVE_ARC = 3,
	SLAVE_VRM = 4,
	SLAVE_BCM = 5,
};

/* A VRM (regulator) is voted on through three registers at its address */
#define VRM_VOLTAGE		0x0
#define VRM_ENABLE		0x4
#define VRM_MODE		0x8

enum res_kind {
	RES_REG,
	RES_CBCR,
	RES_GDSC,
	RES_ARC,
	RES_VRM,
	RES_BCM,
};

static const char __hyp_rodata *const kind_names[] = { "reg", "cbcr", "gdsc", "arc", "vrm", "bcm" };

struct res_window {
	char name[24];
	u64 base;
	bool safe;		/* hypervisor-res-safe: answers in any state of the clocks and power domains */
	bool lpass;		/* hypervisor-res-guard = "lpass" */
	int needs;		/* the power domain that has to be on for the window to answer, or -1 */
	char needs_name[RES_NAME];
};

struct hyp_res {
	char name[RES_NAME];
	u8 kind;
	u8 win;			/* register resources: the window */
	u16 aux_len;		/* RPMh resources: bytes of aux data in the command DB (the ARC's levels) */
	u32 off;		/* a register's offset in its window; an RPMh resource's address */
	u32 aux_pa;
};

static __hyp_data struct res_window res_win[RES_MAX_WINDOWS];
static __hyp_data struct hyp_res res_tab[RES_MAX];
static __hyp_data u32 res_nwin, res_nres;
static __hyp_data u32 res_snap[RES_MAX];
static __hyp_data u32 res_snap_valid[RES_MAX / 32];

static bool __hyp res_is_reg(const struct hyp_res *r)
{
	return r->kind <= RES_GDSC;
}

/* The window's name without its unit address */
static void __hyp res_copy_name(char *dst, size_t len, const char *src)
{
	size_t n = 0;

	while (src[n] && src[n] != '@' && n < len - 1) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
}

static int __hyp res_add(const char *prefix, const char *name, u8 kind, u8 win, u32 off)
{
	struct hyp_res *r;

	if (res_nres >= RES_MAX)
		return -ENOSPC;

	r = &res_tab[res_nres];
	snprintf(r->name, sizeof(r->name), "%s%s", prefix, name);
	r->kind = kind;
	r->win = win;
	r->off = off;
	r->aux_len = 0;
	r->aux_pa = 0;

	return res_nres++;
}

/* The device tree's windows and what is in them */
static void __hyp res_from_dt(void)
{
	ofnode root = ofnode_path("/hypervisor-resources"), node, sub;

	if (!ofnode_valid(root)) {
		hprintf("res: no /hypervisor-resources in the device tree\n");
		return;
	}

	ofnode_for_each_subnode(node, root) {
		struct res_window *w;
		phys_addr_t size;
		fdt_addr_t base;
		const char *s;

		if (res_nwin >= RES_MAX_WINDOWS)
			break;
		base = ofnode_get_addr_size_index_notrans(node, 0, &size);
		if (base == FDT_ADDR_T_NONE)
			continue;

		w = &res_win[res_nwin];
		res_copy_name(w->name, sizeof(w->name), ofnode_get_name(node));
		w->base = base;
		w->safe = ofnode_read_bool(node, "hypervisor-res-safe");
		s = ofnode_read_string(node, "hypervisor-res-guard");
		w->lpass = s && !strcmp(s, "lpass");
		w->needs = -1;
		w->needs_name[0] = '\0';
		s = ofnode_read_string(node, "hypervisor-res-needs");
		if (s)
			strlcpy(w->needs_name, s, sizeof(w->needs_name));

		ofnode_for_each_subnode(sub, node) {
			const char *type = ofnode_read_string(sub, "type");
			char prefix[RES_NAME];
			u32 off;
			int kind;

			if (!type || ofnode_read_u32(sub, "offset", &off))
				continue;
			kind = !strcmp(type, "cbcr") ? RES_CBCR : !strcmp(type, "gdsc") ? RES_GDSC :
			       !strcmp(type, "reg") ? RES_REG : -1;
			if (kind < 0)
				continue;
			snprintf(prefix, sizeof(prefix), "%s.", w->name);
			res_add(prefix, ofnode_get_name(sub), kind, res_nwin, off);
		}
		res_nwin++;
	}
}

/* The command DB's resources: every name is one, its slave id says which kind */
static void __hyp res_from_cmd_db(void)
{
	ofnode node = ofnode_by_compatible(ofnode_null(), "qcom,cmd-db");
	const volatile struct cmd_db_header *db;
	phys_addr_t size;
	fdt_addr_t base;
	u32 n = 0;

	if (!ofnode_valid(node))
		return;
	base = ofnode_get_addr_size_index_notrans(node, 0, &size);
	if (base == FDT_ADDR_T_NONE)
		return;

	db = (const volatile struct cmd_db_header *)(uintptr_t)base;
	if (memcmp((const void *)db->magic, cmd_db_magic, sizeof(cmd_db_magic))) {
		hprintf("res: the command DB at %#llx has no magic\n", (u64)base);
		return;
	}

	for (int i = 0; i < CMD_DB_MAX_SLV && db->header[i].slv_id; i++) {
		const volatile struct cmd_db_rsc_hdr *h = &db->header[i];
		const volatile struct cmd_db_entry *e = (const volatile struct cmd_db_entry *)
			(db->data + h->header_offset);

		for (int j = 0; j < h->cnt; j++, e++) {
			u32 slave = (e->addr >> CMD_DB_SLV_SHIFT) & CMD_DB_SLV_MASK;
			char name[sizeof(e->id) + 1];
			struct hyp_res *r;
			int idx;

			if (slave < SLAVE_ARC || slave > SLAVE_BCM)
				continue;
			memcpy(name, (const void *)e->id, sizeof(e->id));
			name[sizeof(e->id)] = '\0';
			idx = res_add("rpmh.", name, RES_ARC + slave - SLAVE_ARC, 0, e->addr);
			if (idx < 0)
				continue;
			r = &res_tab[idx];
			r->aux_len = e->len;
			r->aux_pa = (u32)(base + offsetof(struct cmd_db_header, data) + h->data_offset + e->offset);
			n++;
		}
	}
	hprintf("res: %u RPMh resources from the command DB\n", n);
}

static int __hyp res_find_exact(const char *name)
{
	for (u32 i = 0; i < res_nres; i++)
		if (!strcmp(res_tab[i].name, name))
			return i;

	return -ENOENT;
}

/**
 * hyp_res_init() - build the table, once at boot
 */
void __hyp hyp_res_init(void)
{
	res_from_dt();
	for (u32 i = 0; i < res_nwin; i++)
		if (res_win[i].needs_name[0])
			res_win[i].needs = res_find_exact(res_win[i].needs_name);
	res_from_cmd_db();

	/* the resource table is read by every core: the data of an EL2 that runs with caches off */
	hyp_clean_range(res_win, sizeof(res_win));
	hyp_clean_range(res_tab, sizeof(res_tab));
	hyp_clean_range(&res_nwin, sizeof(res_nwin));
	hyp_clean_range(&res_nres, sizeof(res_nres));
	hprintf("res: %u resources in %u windows\n", res_nres, res_nwin);
}

u32 __hyp hyp_res_count(void)
{
	return res_nres;
}

const char *__hyp hyp_res_name(int idx)
{
	return idx >= 0 && (u32)idx < res_nres ? res_tab[idx].name : NULL;
}

/**
 * hyp_res_find() - a resource by name
 * @name: its name, or a part of it that only one name contains
 *
 * Return: its index, -ENOENT when there is none, -EEXIST when the part fits more than one
 */
int __hyp hyp_res_find(const char *name)
{
	int found = -ENOENT;

	if (!*name)
		return -ENOENT;
	found = res_find_exact(name);
	if (found >= 0)
		return found;

	for (u32 i = 0; i < res_nres; i++) {
		if (!strstr(res_tab[i].name, name))
			continue;
		if (found >= 0)
			return -EEXIST;
		found = i;
	}

	return found;
}

static int __hyp res_reg_read(const struct hyp_res *r, u32 *val)
{
	return hyp_phys_read(res_win[r->win].base + r->off, val, sizeof(*val));
}

static int __hyp res_reg_write(const struct hyp_res *r, u32 val)
{
	return hyp_phys_write(res_win[r->win].base + r->off, &val, sizeof(val));
}

/* Whether a power domain (GDSC) or branch clock is on, by its status bit; < 0: it cannot be told */
static int __hyp res_is_on(const struct hyp_res *r)
{
	u32 v;

	if (res_reg_read(r, &v))
		return -EFAULT;
	if (r->kind == RES_GDSC)
		return !!(v & GDSCR_PWR_ON);
	if (r->kind == RES_CBCR)
		return !(v & CBCR_OFF);

	return -EINVAL;
}

/**
 * hyp_res_guard() - why a register resource must not be touched now, or NULL
 * @idx: the resource
 */
const char *__hyp hyp_res_guard(int idx)
{
	const struct hyp_res *r;
	const struct res_window *w;

	if (idx < 0 || (u32)idx >= res_nres)
		return "no such resource";
	r = &res_tab[idx];
	if (!res_is_reg(r))
		return NULL;

	w = &res_win[r->win];
	if (!w->safe && w->needs < 0)
		return "not known to answer in every state, it may reset the board";
	if (w->lpass && hyp_pas_core_up())
		return "the Hexagon runs: the LPASS registers time out and reset the board";
	if (w->needs >= 0 && res_is_on(&res_tab[w->needs]) != 1)
		return "the power domain this window needs is off";

	return NULL;
}

static int __hyp res_rpmh_read(const struct hyp_res *r, u32 off, u32 *val)
{
	return hyp_rpmh_read(r->off + off, val);
}

/**
 * hyp_res_value() - read a resource
 * @idx: the resource
 * @val: its value: a register's, an ARC's level, a BCM's vote word; a regulator's enable
 * @force: read a register the guard would refuse
 */
int __hyp hyp_res_value(int idx, u32 *val, bool force)
{
	const struct hyp_res *r;

	if (idx < 0 || (u32)idx >= res_nres)
		return -ENOENT;
	r = &res_tab[idx];

	if (res_is_reg(r)) {
		if (!force && hyp_res_guard(idx))
			return -EACCES;
		return res_reg_read(r, val);
	}

	return res_rpmh_read(r, r->kind == RES_VRM ? VRM_ENABLE : 0, val);
}

static const char __hyp_rodata *const vrm_modes[] = {
	"mode0", "mode1", "bypass", "retention", "low-power", "mode5", "nominal", "high-power",
};

/* "name kind address": what a resource is and where, without reading it */
static int __hyp res_label(const struct hyp_res *r, char *buf, size_t len)
{
	return snprintf(buf, len, "%-36s %-4s %#10llx", r->name, kind_names[r->kind],
			res_is_reg(r) ? res_win[r->win].base + r->off : (u64)r->off);
}

int __hyp hyp_res_label(int idx, char *buf, size_t len)
{
	if (idx < 0 || (u32)idx >= res_nres)
		return -ENOENT;

	return res_label(&res_tab[idx], buf, len);
}

/**
 * hyp_res_describe() - one line: the resource, where it is and what state it is in
 * @idx: the resource
 * @buf: where the line goes (no newline)
 * @len: its size
 * @force: read a register the guard would refuse
 */
int __hyp hyp_res_describe(int idx, char *buf, size_t len, bool force)
{
	const struct hyp_res *r;
	const char *why;
	u32 v = 0;
	int n, ret;

	if (idx < 0 || (u32)idx >= res_nres)
		return -ENOENT;
	r = &res_tab[idx];

	if (res_is_reg(r)) {
		n = res_label(r, buf, len);
		why = force ? NULL : hyp_res_guard(idx);
		if (why)
			return n + snprintf(buf + n, len - n, "  not read: %s", why);
		ret = res_reg_read(r, &v);
		if (ret)
			return n + snprintf(buf + n, len - n, "  no answer (%d)", ret);
		n += snprintf(buf + n, len - n, " = %#010x", v);
		if (r->kind == RES_CBCR)
			n += snprintf(buf + n, len - n, "  clock %s, %s%s", v & CBCR_OFF ? "off" : "on",
				      v & CBCR_ENABLE ? "enabled" : "not enabled", v & CBCR_HW_CTL ? ", hardware" : "");
		else if (r->kind == RES_GDSC)
			n += snprintf(buf + n, len - n, "  power %s (state %lu)%s", v & GDSCR_PWR_ON ? "on" : "off",
				      FIELD_GET(GDSCR_STATE, v), v & GDSCR_SW_COLLAPSE ? ", collapse requested" : "");
		return n;
	}

	n = res_label(r, buf, len);
	if (r->kind == RES_VRM) {
		u32 volt = ~0U, en = ~0U, mode = ~0U;

		ret = hyp_rpmh_read(r->off + VRM_VOLTAGE, &volt);
		ret = ret ?: hyp_rpmh_read(r->off + VRM_ENABLE, &en);
		ret = ret ?: hyp_rpmh_read(r->off + VRM_MODE, &mode);
		if (ret)
			return n + snprintf(buf + n, len - n, "  no answer (%d)", ret);
		return n + snprintf(buf + n, len - n, "  %s, %u mV, %s", en & 1 ? "enabled" : "disabled", volt,
				    mode < ARRAY_SIZE(vrm_modes) ? vrm_modes[mode] : "?");
	}

	ret = res_rpmh_read(r, 0, &v);
	if (ret)
		return n + snprintf(buf + n, len - n, "  no answer (%d)", ret);
	if (r->kind == RES_BCM)
		return n + snprintf(buf + n, len - n, " = %#010x  x %#x y %#x%s%s", v, (v >> 14) & 0x3fff, v & 0x3fff,
				    v & BIT(30) ? ", commit" : "", v & BIT(29) ? "" : ", not valid");
	n += snprintf(buf + n, len - n, " = %u", v);

	/* the levels an ARC takes: the command DB's aux data, 16 bits each */
	if (r->aux_len) {
		u16 levels[16];
		u32 count = min_t(u32, r->aux_len / 2, ARRAY_SIZE(levels));

		if (!hyp_phys_read(r->aux_pa, levels, count * 2)) {
			n += snprintf(buf + n, len - n, "  levels");
			for (u32 i = 0; i < count && (size_t)n < len; i++)
				n += snprintf(buf + n, len - n, " %u", levels[i]);
		}
	}

	return n;
}

/* Wait for a status bit to read @want, after a write */
static int __hyp res_wait(const struct hyp_res *r, u32 bit, bool want)
{
	u64 t0 = hyp_ticks();
	u32 v;

	do {
		if (res_reg_read(r, &v))
			return -EFAULT;
		if (!!(v & bit) == want)
			return 0;
	} while (!hyp_ticks_elapsed(t0, RES_WAIT_US));

	return -ETIMEDOUT;
}

/**
 * hyp_res_enable() - switch a branch clock, power domain or regulator on or off
 * @idx: the resource
 * @on: the state wanted
 * @force: touch a register the guard would refuse
 *
 * Return: 0, -ETIMEDOUT when the status bit does not follow, -ENOTSUPP for a resource that has no
 * on and off (set its value instead)
 */
int __hyp hyp_res_enable(int idx, bool on, bool force)
{
	const struct hyp_res *r;
	u32 v;
	int ret;

	if (idx < 0 || (u32)idx >= res_nres)
		return -ENOENT;
	r = &res_tab[idx];

	if (r->kind == RES_VRM)
		return hyp_rpmh_write(r->off + VRM_ENABLE, on);
	if (r->kind != RES_CBCR && r->kind != RES_GDSC)
		return -ENOTSUPP;
	if (!force && hyp_res_guard(idx))
		return -EACCES;

	ret = res_reg_read(r, &v);
	if (ret)
		return ret;
	if (r->kind == RES_CBCR) {
		ret = res_reg_write(r, on ? v | CBCR_ENABLE : v & ~CBCR_ENABLE);
		return ret ?: res_wait(r, CBCR_OFF, !on);
	}
	/* a power domain: SW_COLLAPSE set takes it down */
	ret = res_reg_write(r, on ? v & ~GDSCR_SW_COLLAPSE : v | GDSCR_SW_COLLAPSE);

	return ret ?: res_wait(r, GDSCR_PWR_ON, on);
}

/* The highest level an ARC takes: the last non-zero entry of its table (entry 0 is "off") */
static u32 __hyp res_arc_top(const struct hyp_res *r)
{
	u16 levels[32];
	u32 count = min_t(u32, r->aux_len / 2, ARRAY_SIZE(levels)), top = 0;

	if (hyp_phys_read(r->aux_pa, levels, count * 2))
		return 0;
	for (u32 i = 1; i < count; i++)
		if (levels[i])
			top = i;

	return top;
}

/**
 * hyp_res_set() - write a value
 * @idx: the resource
 * @field: for a regulator, "voltage" (mV), "enable" or "mode"; otherwise NULL
 * @val: the value: a register's, an ARC's level (an index into its levels; all ones: the highest),
 *	a BCM vote word
 * @force: touch a register the guard would refuse
 */
int __hyp hyp_res_set(int idx, const char *field, u64 val, bool force)
{
	const struct hyp_res *r;
	u32 off = 0;

	if (idx < 0 || (u32)idx >= res_nres)
		return -ENOENT;
	r = &res_tab[idx];

	if (res_is_reg(r)) {
		if (!force && hyp_res_guard(idx))
			return -EACCES;
		return res_reg_write(r, (u32)val);
	}

	if (r->kind == RES_VRM) {
		if (!field)
			return -EINVAL;
		off = !strcmp(field, "voltage") ? VRM_VOLTAGE : !strcmp(field, "enable") ? VRM_ENABLE :
		      !strcmp(field, "mode") ? VRM_MODE : ~0U;
		if (off == ~0U)
			return -EINVAL;
	}

	if (r->kind == RES_ARC && val == ~0ULL)
		val = res_arc_top(r);

	return hyp_rpmh_write(r->off + off, (u32)val);
}

/**
 * hyp_res_snapshot() - remember the value of every register resource that may be read now
 * @force: read those the guard would refuse too
 *
 * Return: how many were read
 */
u32 __hyp hyp_res_snapshot(bool force)
{
	u32 n = 0;

	memset(res_snap_valid, 0, sizeof(res_snap_valid));
	for (u32 i = 0; i < res_nres; i++) {
		if (!res_is_reg(&res_tab[i]) || (!force && hyp_res_guard(i)))
			continue;
		if (res_reg_read(&res_tab[i], &res_snap[i]))
			continue;
		res_snap_valid[i / 32] |= BIT(i % 32);
		n++;
	}

	return n;
}

/**
 * hyp_res_diff_line() - the next resource whose register differs from the snapshot
 * @cursor: where to go on from (start at 0)
 * @buf: the line, "name  was 0x... now 0x..."
 * @len: its size
 * @force: read registers the guard would refuse
 *
 * Return: the length of the line, or 0 when there is no more
 */
int __hyp hyp_res_diff_line(u32 *cursor, char *buf, size_t len, bool force)
{
	for (; *cursor < res_nres; (*cursor)++) {
		u32 i = *cursor, now;

		if (!(res_snap_valid[i / 32] & BIT(i % 32)) || (!force && hyp_res_guard(i)))
			continue;
		if (res_reg_read(&res_tab[i], &now) || now == res_snap[i])
			continue;
		(*cursor)++;

		return snprintf(buf, len, "%-36s was %#010x now %#010x", res_tab[i].name, res_snap[i], now);
	}

	return 0;
}
