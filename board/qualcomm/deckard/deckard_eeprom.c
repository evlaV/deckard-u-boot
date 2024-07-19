#include "deckard_eeprom.h"
#include <common.h>
#include <dm.h>
#include <fdt_support.h>
#include <asm/global_data.h>
#include <init.h>
#include <i2c.h>
#include <i2c_eeprom.h>
#include <env.h>

DECLARE_GLOBAL_DATA_PTR;

// These are taken directly from Deckard EEPROM user space tool
static eeprom_checksum_t _eeprom_calculate_checksum(uint8_t *data, uint32_t len)
{
	//https://en.wikipedia.org/wiki/Fletcher's_checksum

	eeprom_checksum_t check;
	check.value = 0;

	if (NULL == data)
	{
		return check;
	}

	uint16_t sum1 = 0;
	uint16_t sum2 = 0;

	for (uint32_t index = 0; index < len; ++index)
	{
		sum1 = (sum1 + data[index]) % 255;
		sum2 = (sum2 + sum1) % 255;
	}

	check.value = (sum2 << 8) | sum1;
	return check;
}

static bool _eeprom_verify_core(eeprom_data_t *eeprom_data)
{
	eeprom_checksum_t calc_checksum = _eeprom_calculate_checksum((uint8_t *)eeprom_data, sizeof(eeprom_data_t) - sizeof(eeprom_checksum_t));

	if((eeprom_data->magic_number == EEPROM_CORE_MAGIC_NUMBER) && (calc_checksum.value == eeprom_data->checksum.value))
	{
		return true;
	}

	printf("EEPROM verify failed ( Magic Number %x ( expected %x ), CRC %x ( expected %x ) )\n", 
			eeprom_data->magic_number, EEPROM_CORE_MAGIC_NUMBER,
			eeprom_data->checksum.value, calc_checksum.value );

	return false;
}

void deckard_eeprom_init(void)
{
	int ret;
	struct udevice *dev;
	int node;
	ofnode eeprom;
	const char *path;
	eeprom_data_t eeprom_data;

	if (!gd->fdt_blob) {
		printf("%s: don't have a valid gd->fdt_blob!\n", __func__);
		return;
	}

	node = fdt_path_offset(gd->fdt_blob, "/aliases");
	if (node < 0) {
		printf("%s: no aliases found\n", __func__);
		return;
	}

	path = fdt_getprop(gd->fdt_blob, node, "eeprom-core", NULL);
	if (!path) {
		printf("%s: no alias for %s\n", __func__, "eeprom-core");
		return;
	}

	eeprom = ofnode_path(path);
	if (!ofnode_valid(eeprom)) {
		printf("%s: invalid hardware path to EEPROM\n", __func__);
		return;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_I2C_EEPROM, eeprom, &dev);
	if (ret) {
		printf("%s: cannot find EEPROM by node\n", __func__);
		return;
	}

	ret = i2c_eeprom_read(dev, 0, (uint8_t*)&eeprom_data, sizeof(eeprom_data_t));
	if (ret) {
		printf("%s: Could not read EEPROM\n", __func__);
	}

	if ( !_eeprom_verify_core(&eeprom_data) ) {
		return;
	}

	printf("EEPROM Board Revision: %s\n", eeprom_data.board_version );

	env_set("EEPROM_BOARD",  eeprom_data.board_version);
}