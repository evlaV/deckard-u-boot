/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * The text side of the GDB remote serial protocol, shared by the two GDB servers (hyp/gdb.c for the
 * guest's CPU, hyp/gdbmem.c for physical memory): hex digits and numbers, and the packet
 * checksum.
 */
#ifndef __HYP_RSP_H__
#define __HYP_RSP_H__

#include <hypervisor.h>
#include <linux/types.h>

static inline char __hyp rsp_hexc(u32 n)
{
	n &= 0xf;

	return n < 10 ? '0' + n : 'a' + n - 10;
}

static inline int __hyp rsp_unhex(u8 c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/* A hex number at *@p, up to the first byte that is not a digit; false when there is none */
static inline bool __hyp rsp_parse_hex(const char **p, u64 *v)
{
	int d = rsp_unhex(**p);

	if (d < 0)
		return false;

	*v = 0;
	while ((d = rsp_unhex(**p)) >= 0) {
		*v = (*v << 4) | d;
		(*p)++;
	}

	return true;
}

/* @len bytes as hex, in memory order; returns the digits written */
static inline u32 __hyp rsp_put_hex(char *dst, const u8 *src, u32 len)
{
	for (u32 i = 0; i < len; i++) {
		dst[2 * i] = rsp_hexc(src[i] >> 4);
		dst[2 * i + 1] = rsp_hexc(src[i]);
	}

	return 2 * len;
}

/* @n hex digit pairs at @src into bytes; false on a bad digit */
static inline bool __hyp rsp_get_hex(const char *src, u8 *dst, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		int hi = rsp_unhex(src[2 * i]), lo = rsp_unhex(src[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return false;
		dst[i] = (hi << 4) | lo;
	}

	return true;
}

#endif /* __HYP_RSP_H__ */
