/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * (C) Copyright 2017 Jorge Ramirez-Ortiz <jorge.ramirez-ortiz@linaro.org>
 */
#ifndef _CLOCK_QCOM_H
#define _CLOCK_QCOM_H

#include <dm.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <div64.h>
#include <linux/bug.h>

struct udevice;

int qcom_cc_bind_pd_reset(struct udevice *parent);

unsigned long gcd(unsigned long a, unsigned long b);

void rational_best_approximation(
	unsigned long given_numerator, unsigned long given_denominator,
	unsigned long max_numerator, unsigned long max_denominator,
	unsigned long *best_numerator, unsigned long *best_denominator);

#define usleep_range(min, max) udelay(max)

static inline const char *str_enable_disable(bool v)
{
	return v ? "enable" : "disable";
}
#define str_disable_enable(v)		str_enable_disable(!(v))

#endif /* _CLOCK_QCOM_H */
