/* SPDX-License-Identifier: GPL-2.0+ */
/*
 *  Copyright (C) 2026 Valve
 *  Todd Goyen <todd@tgoyen.com>
 */

#include <dm.h>
#include <errno.h>
#include <i2c.h>
#include <linux/printk.h>
#include <power/pmic.h>
#include <power/slg4ad48669m_hotswap.h>

// clang-format off
static const u8 hotswap_regs[] = {
    [REG_PATTERN_ID] = 0xE6,
    [REG_LATCHED_FAULTS] = 0xF1,
    [REG_REALTIME_STATUS] = 0xF2,
    [REG_I2C_VIRTUAL_IN] = 0xF4,
    [REG_FET_STATUS] = 0xF5,
    [REG_REALTIME_STATUS2] = 0xF7,
};
// clang-format on

static int hotswap_reg_count(struct udevice *dev) {
  return SLG4AD48669M_NUM_REGS;
}

static int hotswap_write(struct udevice *dev, uint reg, const uint8_t *buff,
                         int len) {
  int ret;

  ret = dm_i2c_write(dev, hotswap_regs[reg], buff, len);
  if (ret)
    pr_err("write error to device: %p register: %#x!\n", dev, reg);

  return ret;
}

static int hotswap_read(struct udevice *dev, uint reg, uint8_t *buff, int len) {
  int ret;

  ret = dm_i2c_read(dev, hotswap_regs[reg], buff, len);
  if (ret)
    pr_err("read error from device: %p register: %#x!\n", dev, reg);
  return ret;
}

static struct dm_pmic_ops hotswap_ops = {
    .reg_count = hotswap_reg_count,
    .read = hotswap_read,
    .write = hotswap_write,
};

static const struct udevice_id hotswap_ids[] = {
    {.compatible = "silego,northgate_hotswap"},
    {},
};

U_BOOT_DRIVER(pmic_hotswap) = {
    .name = "slg4ad48669m_hotswap",
    .id = UCLASS_PMIC,
    .of_match = hotswap_ids,
    .ops = &hotswap_ops,
};
