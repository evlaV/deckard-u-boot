// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2022 Sean Anderson <sean.anderson@seco.com>
 */

#include <i2c_eeprom.h>
#include <linker_lists.h>
#include <misc.h>
#include <nvmem.h>
#include <rtc.h>
#include <power/pmic.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <dm/uclass.h>

int nvmem_cell_read(struct nvmem_cell *cell, void *buf, size_t size)
{
	int ret;

	dev_dbg(cell->nvmem, "%s: off=0x%x size=%zu\n", __func__, cell->offset, size);
	if (!cell->size)
		return -EINVAL;

	size = min_t(size_t, size, cell->size);

	switch (cell->nvmem->driver->id) {
	case UCLASS_I2C_EEPROM:
		return i2c_eeprom_read(cell->nvmem, cell->offset, buf, size);
	case UCLASS_MISC: {
		ret = misc_read(cell->nvmem, cell->offset, buf, size);

		if (ret < 0)
			return ret;
		if (ret != size)
			return -EIO;
		return 0;
	}
	case UCLASS_RTC:
		return dm_rtc_read(cell->nvmem, cell->offset, buf, size);
	case UCLASS_PMIC:
		ret = pmic_reg_read(cell->nvmem, cell->offset);
		if (ret < 0)
			return -EIO;
		*(uint *)buf = ret;

		return 0;
	default:
		return -ENOSYS;
	}
}

int nvmem_cell_write(struct nvmem_cell *cell, const void *buf, size_t size)
{
	dev_dbg(cell->nvmem, "%s: off=0x%x size=%zu\n", __func__, cell->offset, size);
	if (!cell->size)
		return -EINVAL;

	size = min_t(size_t, size, cell->size);

	switch (cell->nvmem->driver->id) {
	case UCLASS_I2C_EEPROM:
		return i2c_eeprom_write(cell->nvmem, cell->offset, buf, size);
	case UCLASS_MISC: {
		int ret = misc_write(cell->nvmem, cell->offset, buf, size);

		if (ret < 0)
			return ret;
		if (ret != size)
			return -EIO;
		return 0;
	}
	case UCLASS_RTC:
		return dm_rtc_write(cell->nvmem, cell->offset, buf, size);
	case UCLASS_PMIC:
		return pmic_write(cell->nvmem, cell->offset, buf, size);
	default:
		return -ENOSYS;
	}
}

int nvmem_device_read(struct nvmem_device *nvmem_dev, unsigned int offset, void *buf, size_t size)
{
	struct nvmem_cell cell = { 0 };

	cell.nvmem = nvmem_dev->nvmem;
	cell.offset = nvmem_dev->addr + offset;

	return nvmem_cell_read(&cell, buf, size);
}

int nvmem_device_write(struct nvmem_device *nvmem_dev, unsigned int offset, const void *buf, size_t size)
{
	struct nvmem_cell cell = { 0 };

	cell.nvmem = nvmem_dev->nvmem;
	cell.offset = nvmem_dev->addr + offset;

	return nvmem_cell_write(&cell, buf, size);
}

/**
 * nvmem_get_device() - Get an nvmem device for a cell
 * @node: ofnode of the nvmem device
 * @nvmem: nvmem device
 *
 * Try to find a nvmem-compatible device by going through the nvmem interfaces.
 *
 * Return:
 * * 0 on success
 * * -ENODEV if we didn't find anything
 * * A negative error if there was a problem looking up the device
 */
static int nvmem_get_device(ofnode node, struct udevice **nvmem)
{
	int i, ret;
	enum uclass_id ids[] = {
		UCLASS_I2C_EEPROM,
		UCLASS_MISC,
		UCLASS_RTC,
		UCLASS_PMIC,
	};

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		ret = uclass_get_device_by_ofnode(ids[i], node, nvmem);
		if (!ret)
			return 0;
		if (ret != -ENODEV && ret != -EPFNOSUPPORT)
			return ret;
	}

	return -ENODEV;
}

int nvmem_cell_get_by_index(struct udevice *dev, int index,
			    struct nvmem_cell *cell)
{
	fdt_addr_t offset;
	fdt_size_t size = FDT_SIZE_T_NONE;
	int ret;
	struct ofnode_phandle_args args;

	dev_dbg(dev, "%s: index=%d\n", __func__, index);

	ret = dev_read_phandle_with_args(dev, "nvmem-cells", NULL, 0, index,
					 &args);
	if (ret)
		return ret;

	ret = nvmem_get_device(ofnode_get_parent(args.node), &cell->nvmem);
	if (ret)
		return ret;

	offset = ofnode_get_addr_size_index_notrans(args.node, 0, &size);
	if (offset == FDT_ADDR_T_NONE || size == FDT_SIZE_T_NONE) {
		dev_dbg(cell->nvmem, "missing address or size for %s\n",
			ofnode_get_name(args.node));
		return -EINVAL;
	}

	cell->offset = offset;
	cell->size = size;
	return 0;
}

int nvmem_cell_get_by_name(struct udevice *dev, const char *name,
			   struct nvmem_cell *cell)
{
	int index;

	dev_dbg(dev, "%s, name=%s\n", __func__, name);

	index = dev_read_stringlist_search(dev, "nvmem-cell-names", name);
	if (index < 0)
		return index;

	return nvmem_cell_get_by_index(dev, index, cell);
}

int nvmem_get_by_name(struct udevice *dev, const char *name,
		      struct nvmem_device *nvmem_dev)
{
	struct ofnode_phandle_args args;
	int index;
	int ret;

	index = dev_read_stringlist_search(dev, "nvmem-names", name);
	if (index < 0)
		return index;

	ret = dev_read_phandle_with_args(dev, "nvmem", NULL, 0, index,
					 &args);
	if (ret)
		return ret;

	ret = nvmem_get_device(ofnode_get_parent(args.node), &nvmem_dev->nvmem);
	if (ret)
		return ret;

	nvmem_dev->addr = ofnode_get_addr_size_index_notrans(args.node, 0, NULL);
	if (nvmem_dev->addr == FDT_ADDR_T_NONE) {
		dev_dbg(nvmem_dev->nvmem, "missing address or size for %s\n",
			ofnode_get_name(args.node));
		return -EINVAL;
	}
	return 0;
}
