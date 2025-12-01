#include <dm.h>
#include <i2c.h>
#include <linux/printk.h>
#include <power/pmic.h>
#include <power/max17320_fg.h>
#include <errno.h>

static int max17320_reg_count(struct udevice *dev)
{
	return MAX17320_NUM_OF_REGS;
}

static int max17320_write(struct udevice *dev, uint reg, const uint8_t *buff,
		int len)
{
	int ret;

	ret = dm_i2c_write(dev, max1730x_regs[reg], buff, len);
	if (ret)
		pr_err("write error to device: %p register: %#x!\n", dev, reg);

	return ret;
}

static int max17320_read(struct udevice *dev, uint reg, uint8_t *buff, int len)
{
	int ret;

	ret = dm_i2c_read(dev, max1730x_regs[reg], buff, len);
	if (ret)
		pr_err("read error from device: %p register: %#x!\n", dev, reg);
	return ret;
}

static struct dm_pmic_ops max17320_ops = {
	.reg_count = max17320_reg_count,
	.read	= max17320_read,
	.write	= max17320_write,
};

static const struct udevice_id max17320_ids[] = {
	{ .compatible = "maxim,max17320" },
	{ },
};

U_BOOT_DRIVER(pmic_max17320) = {
	.name		= "max17320_fg",
	.id		= UCLASS_PMIC,
	.of_match	= max17320_ids,
	.ops		= &max17320_ops,
};
