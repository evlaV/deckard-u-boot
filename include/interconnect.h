/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Linaro Limited
 */

#ifndef _INTERCONNECT_H
#define _INTERCONNECT_H

#include <linux/errno.h>

struct udevice;

/* macros for converting to icc units */
#define Bps_to_icc(x)	((x) / 1000)
#define kBps_to_icc(x)	(x)
#define MBps_to_icc(x)	((x) * 1000)
#define GBps_to_icc(x)	((x) * 1000 * 1000)
#define bps_to_icc(x)	(1)
#define kbps_to_icc(x)	((x) / 8 + ((x) % 8 ? 1 : 0))
#define Mbps_to_icc(x)	((x) * 1000 / 8)
#define Gbps_to_icc(x)	((x) * 1000 * 1000 / 8)

struct icc_path;

#if CONFIG_IS_ENABLED(INTERCONNECT)
struct icc_path *of_icc_get(struct udevice *dev, const char *name);
#else
static inline
struct icc_path *of_icc_get(struct udevice *dev, const char *name)
{
	return NULL;
}
#endif

#if CONFIG_IS_ENABLED(INTERCONNECT)
struct icc_path *of_icc_get_by_index(struct udevice *dev, int idx);
#else
static inline
struct icc_path *of_icc_get_by_index(struct udevice *dev, int idx)
{
	return NULL;
}
#endif

#if CONFIG_IS_ENABLED(INTERCONNECT)
int icc_put(struct icc_path *path);
#else
static inline int icc_put(struct icc_path *path)
{
	return 0;
}
#endif

#if CONFIG_IS_ENABLED(INTERCONNECT)
int icc_enable(struct icc_path *path);
#else
static inline int icc_enable(struct icc_path *path)
{
	return -ENOSYS;
}
#endif

#if CONFIG_IS_ENABLED(INTERCONNECT)
int icc_disable(struct icc_path *path);
#else
static inline int icc_disable(struct icc_path *path)
{
	return -ENOSYS;
}
#endif

#if CONFIG_IS_ENABLED(INTERCONNECT)
int icc_set_bw(struct icc_path *path, u32 avg_bw, u32 peak_bw);
#else
static inline int icc_set_bw(struct icc_path *path, u32 avg_bw, u32 peak_bw)
{
	return -ENOSYS;
}
#endif

#endif
