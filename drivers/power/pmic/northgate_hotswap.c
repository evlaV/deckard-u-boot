// SPDX-License-Identifier: GPL-2.0
/*
 * northgate_hotswap.c - Northgate hot-swap Silego (Boulder headstrap)
 *
 * U-Boot only needs to know which battery the Silego has connected to the
 * system, and to be able to change that selection. Cross charging and pack
 * hot-swap handling are left to the kernel and aDSP drivers.
 */

#include <dm.h>
#include <i2c.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <power/pmic.h>

#include <power/northgate_hotswap.h>

/* --- Registers --- */
#define REG_PATTERN_ID 0xE6
#define NORTHGATE_PATTERN_ID 0x05
#define REG_LATCHED_FAULTS 0xF1
#define REG_REALTIME_STATUS 0xF2
#define REG_I2C_VIRTUAL_IN 0xF4
#define REG_FET_STATUS 0xF5
#define REG_REALTIME_STATUS2 0xF7

/* --- Bit macros --- */
/* LATCHED_FAULTS (RO) */
#define LT_HW_BATT_CONTROL BIT(3)
#define LT_CHARGER_INT_LATCHED BIT(4)
#define LT_USE_INT_BATT BIT(5)
#define LT_PACK_STATE_CHANGED BIT(6)
#define LT_INTERNAL_UVLO BIT(7)

/* REALTIME_STATUS (RO) */
#define RT_PACK_CONNECTED BIT(2)
#define RT_CROSS_CHG_VINT_FROM_VREM BIT(6)
#define RT_CROSS_CHG_VREM_FROM_VINT BIT(7)

/* I2C_VIRTUAL_IN (RW) */
#define VI_USE_INTERNAL_BATTERY BIT(0)
#define VI_CHARGE_FROM_INT BIT(1)
#define VI_ENABLE_CROSS_CHARGER BIT(2)
#define VI_CLEAR_BATT_INTERRUPT BIT(4)

/* FET_STATUS (RO) */
#define FET_INT_CHG BIT(0)
#define FET_INT_DIS BIT(1)
#define FET_REM_CHG BIT(2)
#define FET_REM_DSG BIT(3)
#define FET_INT_ACT BIT(5)
#define FET_REM_ACT BIT(6)
#define FET_SELECTED_BATTERY_MASK (FET_INT_ACT | FET_REM_ACT)

/* REALTIME_STATUS2 (RO) */
#define RT2_REMOVABLE_V_OK BIT(1)
#define RT2_INTERNAL_V_OK BIT(2)

/* Time allowed for the Silego to act on a new battery selection */
#define NORTHGATE_SELECT_SETTLE_MS 100

static int northgate_read(struct udevice *dev, uint reg, u8 *val)
{
	return dm_i2c_read(dev, reg, val, 1);
}

static int northgate_write(struct udevice *dev, uint reg, u8 val)
{
	return dm_i2c_write(dev, reg, &val, 1);
}

battery_t northgate_hotswap_get_active_battery(struct udevice *dev)
{
	u8 fet;

	if (!dev)
		return BATTERY_UNKNOWN;

	if (northgate_read(dev, REG_FET_STATUS, &fet))
		return BATTERY_UNKNOWN;

	fet &= FET_SELECTED_BATTERY_MASK;

	if (fet == FET_INT_ACT)
		return BATTERY_INTERNAL;

	if (fet == FET_REM_ACT)
		return BATTERY_EXTERNAL;

	return BATTERY_UNKNOWN;
}

int northgate_hotswap_select_battery(struct udevice *dev, battery_t battery)
{
	u8 vi, sel;
	int ret;

	if (!dev || (battery != BATTERY_INTERNAL && battery != BATTERY_EXTERNAL))
		return -EINVAL;

	ret = northgate_read(dev, REG_I2C_VIRTUAL_IN, &vi);
	if (ret)
		return ret;

	/* The cross charger must be off before the selected battery changes */
	if (vi & VI_ENABLE_CROSS_CHARGER) {
		vi &= ~VI_ENABLE_CROSS_CHARGER;
		ret = northgate_write(dev, REG_I2C_VIRTUAL_IN, vi);
		if (ret)
			return ret;
	}

	sel = (battery == BATTERY_INTERNAL) ? VI_USE_INTERNAL_BATTERY : 0;
	ret = northgate_write(dev, REG_I2C_VIRTUAL_IN, (vi & ~VI_USE_INTERNAL_BATTERY) | sel);
	mdelay(NORTHGATE_SELECT_SETTLE_MS);
	if (ret)
		return ret;

	if (northgate_hotswap_get_active_battery(dev) != battery) {
		/* The Silego didn't follow, put the previous selection back */
		northgate_write(dev, REG_I2C_VIRTUAL_IN, vi);
		mdelay(NORTHGATE_SELECT_SETTLE_MS);
		return -EIO;
	}

	return 0;
}

static int northgate_hotswap_probe(struct udevice *dev)
{
	u8 val;
	int ret;

	/* Check for the Device, if its not there, just bail */
	ret = northgate_read(dev, REG_PATTERN_ID, &val);
	if (ret) {
		debug("northgate-hotswap pattern id read failed: %d\n", ret);
		return -ENODEV;
	}

	if (val != NORTHGATE_PATTERN_ID) {
		debug("northgate-hotswap unknown pattern id 0x%02x (expected 0x%02x)\n",
		      val, NORTHGATE_PATTERN_ID);
		return -ENODEV;
	}

	return 0;
}

static const struct udevice_id northgate_hotswap_ids[] = {
	{ .compatible = "silego,northgate_hotswap" },
	{ }
};

U_BOOT_DRIVER(northgate_hotswap) = {
	.name     = "northgate_hotswap",
	.id       = UCLASS_PMIC,
	.of_match = northgate_hotswap_ids,
	.probe    = northgate_hotswap_probe,
};