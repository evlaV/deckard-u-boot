/* SPDX-License-Identifier: GPL-2.0 */
/*
 * northgate_hotswap.h - Northgate hot-swap Silego (Boulder headstrap)
 */

#ifndef __NORTHGATE_HOTSWAP_H
#define __NORTHGATE_HOTSWAP_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct udevice;

typedef enum {
	BATTERY_UNKNOWN,
	BATTERY_NONE = BATTERY_UNKNOWN,
	BATTERY_INTERNAL,
	BATTERY_EXTERNAL,
} battery_t;

#if IS_ENABLED(CONFIG_HOTSWAP_NORTHGATE)

/**
 * northgate_hotswap_get_active_battery() - Battery the Silego has connected to the system
 * @dev: Northgate device, from pmic_get()
 *
 * Return: BATTERY_INTERNAL or BATTERY_EXTERNAL, or BATTERY_UNKNOWN if the read
 *	   fails or the Silego doesn't report exactly one battery as active
 */
battery_t northgate_hotswap_get_active_battery(struct udevice *dev);

/**
 * northgate_hotswap_select_battery() - Ask the Silego to run the system from a battery
 * @dev: Northgate device, from pmic_get()
 * @battery: BATTERY_INTERNAL or BATTERY_EXTERNAL
 *
 * Turns the cross charger off if it was on, changes the selection, waits for
 * it to settle and checks that the Silego followed. If it didn't, the previous
 * selection is restored. The caller is responsible for stopping the system
 * charger around this call.
 *
 * Return: 0 if @battery is now active, -EINVAL for a bad @battery, -EIO if the
 *	   Silego didn't follow, or another negative errno on an I2C failure
 */
int northgate_hotswap_select_battery(struct udevice *dev, battery_t battery);

#else

static inline battery_t northgate_hotswap_get_active_battery(struct udevice *dev)
{
	return BATTERY_UNKNOWN;
}

static inline int northgate_hotswap_select_battery(struct udevice *dev, battery_t battery)
{
	return -ENOSYS;
}

#endif

#endif /* __NORTHGATE_HOTSWAP_H */