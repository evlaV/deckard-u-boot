/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Silego SLG4AX46073V fan controller
 *
 */

#ifndef __SLG4AX46073V_H
#define __SLG4AX46073V_H

#include <stdbool.h>

struct udevice;

/**
 * slg4ax46073v_get() - Find the fan controller, probing it on first use
 * @devp: returns the device
 *
 * Probing resets the controller, so a state left behind by the kernel (PWM clock disabled, fan at
 * full speed) doesn't carry over. The fan supply is left off.
 *
 * Return: 0 on success, negative error code on failure
 */
int slg4ax46073v_get(struct udevice **devp);

/**
 * slg4ax46073v_set_enabled() - Switch the fan supply
 * @dev: fan controller
 * @enable: true runs the fan at the controller's default speed
 *
 * Turning the fan on resets the controller first (100 ms with the supply off), so a PWM clock left
 * disabled by the kernel or the ADSP doesn't run the fan at full speed. Does nothing if the fan is
 * already in the requested state.
 *
 * Return: 0 on success, negative error code on failure
 */
int slg4ax46073v_set_enabled(struct udevice *dev, bool enable);

#endif /* __SLG4AX46073V_H */
