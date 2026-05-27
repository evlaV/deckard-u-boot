// SPDX-License-Identifier: BSD-3-Clause AND GPL-2.0
/*
 * Clock and reset drivers for Qualcomm platforms Global Clock
 * Controller (GCC).
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 * (C) Copyright 2020 Sartura Ltd. (reset driver)
 *     Author: Robert Marko <robert.marko@sartura.hr>
 * (C) Copyright 2022 Linaro Ltd. (reset driver)
 *     Author: Sumit Garg <sumit.garg@linaro.org>
 *
 * Based on Little Kernel driver, simplified
 */

#include <clk-uclass.h>
#include <linux/clk-provider.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <asm/io.h>
#include <linux/bug.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <reset-uclass.h>
#include <power-domain-uclass.h>

#include "common-uboot.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

int qcom_cc_bind_pd_reset(struct udevice *parent)
{
	struct qcom_cc_desc *data = (struct qcom_cc_desc *)dev_get_driver_data(parent);
	struct regmap *regmap;
	int ret;

	regmap = qcom_cc_map(parent, data);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	if (data->resets) {
		ret = qcom_reset_register(parent, data->resets, data->num_resets, regmap);
		if (ret)
			return ret;
	}

	if (data->gdscs) {
		ret = gdsc_register(parent, data->gdscs, data->num_gdscs, regmap);
		if (ret)
			return ret;
	}

	return 0;
}

// FIXME: import gcd.c from Linux?
/**
 * gcd - calculate and return the greatest common divisor of 2 unsigned longs
 * @a: first value
 * @b: second value
 */
unsigned long gcd(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;

	if (!a || !b)
		return r;

	/* Isolate lsbit of r */
	r &= -r;

	while (!(b & r))
		b >>= 1;
	if (b == r)
		return r;

	for (;;) {
		while (!(a & r))
			a >>= 1;
		if (a == r)
			return r;
		if (a == b)
			return a;

		if (a < b)
			swap(a, b);
		a -= b;
		a >>= 1;
		if (a & r)
			a += b;
		a >>= 1;
	}
}

/*
 * calculate best rational approximation for a given fraction
 * taking into account restricted register size, e.g. to find
 * appropriate values for a pll with 5 bit denominator and
 * 8 bit numerator register fields, trying to set up with a
 * frequency ratio of 3.1415, one would say:
 *
 * rational_best_approximation(31415, 10000,
 *		(1 << 8) - 1, (1 << 5) - 1, &n, &d);
 *
 * you may look at given_numerator as a fixed point number,
 * with the fractional part size described in given_denominator.
 *
 * for theoretical background, see:
 * https://en.wikipedia.org/wiki/Continued_fraction
 */

void rational_best_approximation(
	unsigned long given_numerator, unsigned long given_denominator,
	unsigned long max_numerator, unsigned long max_denominator,
	unsigned long *best_numerator, unsigned long *best_denominator)
{
	/* n/d is the starting rational, which is continually
	 * decreased each iteration using the Euclidean algorithm.
	 *
	 * dp is the value of d from the prior iteration.
	 *
	 * n2/d2, n1/d1, and n0/d0 are our successively more accurate
	 * approximations of the rational.  They are, respectively,
	 * the current, previous, and two prior iterations of it.
	 *
	 * a is current term of the continued fraction.
	 */
	unsigned long n, d, n0, d0, n1, d1, n2, d2;
	n = given_numerator;
	d = given_denominator;
	n0 = d1 = 0;
	n1 = d0 = 1;

	for (;;) {
		unsigned long dp, a;

		if (d == 0)
			break;
		/* Find next term in continued fraction, 'a', via
		 * Euclidean algorithm.
		 */
		dp = d;
		a = n / d;
		d = n % d;
		n = dp;

		/* Calculate the current rational approximation (aka
		 * convergent), n2/d2, using the term just found and
		 * the two prior approximations.
		 */
		n2 = n0 + a * n1;
		d2 = d0 + a * d1;

		/* If the current convergent exceeds the maxes, then
		 * return either the previous convergent or the
		 * largest semi-convergent, the final term of which is
		 * found below as 't'.
		 */
		if ((n2 > max_numerator) || (d2 > max_denominator)) {
			unsigned long t = ULONG_MAX;

			if (d1)
				t = (max_denominator - d0) / d1;
			if (n1)
				t = min(t, (max_numerator - n0) / n1);

			/* This tests if the semi-convergent is closer than the previous
			 * convergent.  If d1 is zero there is no previous convergent as this
			 * is the 1st iteration, so always choose the semi-convergent.
			 */
			if (!d1 || 2u * t > a || (2u * t == a && d0 * dp > d1 * d)) {
				n1 = n0 + t * n1;
				d1 = d0 + t * d1;
			}
			break;
		}
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	*best_numerator = n1;
	*best_denominator = d1;
}

