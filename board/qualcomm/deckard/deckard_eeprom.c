#include "deckard_eeprom.h"
#include <dm.h>
#include <fdt_support.h>
#include <asm/global_data.h>
#include <init.h>
#include <i2c.h>
#include <i2c_eeprom.h>
#include <env.h>
#include <net.h>
#include <string.h>
#include "deckard_log.h"

DECLARE_GLOBAL_DATA_PTR;

static uint8_t s_wifi_mac_addr[EEPROM_MAC_ADDR_LEN];
static bool s_wifi_mac_addr_valid;

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

static bool _eeprom_verify(eeprom_bootloader_t *eeprom_data)
{
	eeprom_checksum_t calc_checksum = _eeprom_calculate_checksum((uint8_t *)eeprom_data, sizeof(eeprom_bootloader_t) - sizeof(eeprom_checksum_t));

	if((eeprom_data->hdr.magic_number == EEPROM_BOOTLOADER_MAGIC_NUMBER) && (calc_checksum.value == eeprom_data->checksum.value))
	{
		deckard_log("EEPROM verify succeeded ( Magic Number %x ( expected %x ), CRC %x ( expected %x ) )\n", 
		eeprom_data->hdr.magic_number, EEPROM_BOOTLOADER_MAGIC_NUMBER,
		eeprom_data->checksum.value, calc_checksum.value );
		return true;
	}

	deckard_log("EEPROM verify failed ( Magic Number %x ( expected %x ), CRC %x ( expected %x ) )\n", 
			eeprom_data->hdr.magic_number, EEPROM_BOOTLOADER_MAGIC_NUMBER,
			eeprom_data->checksum.value, calc_checksum.value );

	return false;
}

void deckard_eeprom_init(void)
{
	int ret;
	struct udevice *dev;
	struct udevice *strap_dev;
	int node;
	ofnode eeprom;
	ofnode eeprom_strap;
	const char *path;
	const char *path_strap;
	eeprom_bootloader_t eeprom_data;
	eeprom_bootloader_t eeprom_strap_data;

	if (!gd->fdt_blob) {
		deckard_log("%s: don't have a valid gd->fdt_blob!\n", __func__);
		return;
	}

	node = fdt_path_offset(gd->fdt_blob, "/aliases");
	if (node < 0) {
		deckard_log("%s: no aliases found\n", __func__);
		return;
	}

	path = fdt_getprop(gd->fdt_blob, node, "eeprom-core", NULL);
	if (!path) {
		deckard_log("%s: no alias for %s\n", __func__, "eeprom-core");
		return;
	}

	eeprom = ofnode_path(path);
	if (!ofnode_valid(eeprom)) {
		deckard_log("%s: invalid hardware path to EEPROM\n", __func__);
		return;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_I2C_EEPROM, eeprom, &dev);
	if (ret) {
		deckard_log("%s: cannot find EEPROM by node\n", __func__);
		return;
	}

	ret = i2c_eeprom_read(dev, EEPROM_BOOTLOADER_ADDRESS_START, (uint8_t*)&eeprom_data, sizeof(eeprom_bootloader_t));
	if (ret) {
		deckard_log("%s: Could not read EEPROM\n", __func__);
	}

	if ( _eeprom_verify(&eeprom_data) ) {
		if ( eeprom_data.board_revision_valid == EEPROM_VALID ) {
			deckard_log("EEPROM Board Revision: %s\n", eeprom_data.board_revision );
			env_set("EEPROM_BOARD",  eeprom_data.board_revision);
		} else {
			deckard_log("EEPROM Board Revision not valid\n" );
		}
	}

	// The "core" partition layout varies by board revision past mac_addr_ble,
	// so we only read/validate the common header here to pull out mac_addr_wifi.
	eeprom_core_t eeprom_core;
	ret = i2c_eeprom_read(dev, EEPROM_CORE_ADDRESS_START, (uint8_t *)&eeprom_core, sizeof(eeprom_core_t));
	if (ret) {
		deckard_log("%s: Could not read EEPROM core partition\n", __func__);
	} else if (eeprom_core.hdr.magic_number != EEPROM_CORE_MAGIC_NUMBER) {
		deckard_log("EEPROM core partition magic number invalid ( %x, expected %x )\n",
			    eeprom_core.hdr.magic_number, EEPROM_CORE_MAGIC_NUMBER);
	} else if (eeprom_core.mac_addr_wifi_valid != EEPROM_VALID) {
		deckard_log("EEPROM mac_addr_wifi not valid\n");
	} else if (!is_valid_ethaddr(eeprom_core.mac_addr_wifi)) {
		deckard_log("EEPROM mac_addr_wifi is not a valid ethernet address\n");
	} else {
		memcpy(s_wifi_mac_addr, eeprom_core.mac_addr_wifi, EEPROM_MAC_ADDR_LEN);
		s_wifi_mac_addr_valid = true;
		deckard_log("EEPROM wifi mac address: %pM\n", s_wifi_mac_addr);
	}

	// test code for eeprom strap
	path_strap = fdt_getprop(gd->fdt_blob, node, "eeprom-strap", NULL);
	if (!path) {
		deckard_log("%s: no alias for %s\n", __func__, "eeprom-strap");
		return;
	}

	eeprom_strap = ofnode_path(path_strap);
	if (!ofnode_valid(eeprom)) {
		deckard_log("%s: invalid hardware path_strap to strap EEPROM\n", __func__);
		return;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_I2C_EEPROM, eeprom_strap, &strap_dev);
	if (ret) {
		deckard_log("%s: cannot find strap EEPROM by node\n", __func__);
		return;
	}

	ret = i2c_eeprom_read(strap_dev, EEPROM_BOOTLOADER_ADDRESS_START, (uint8_t*)&eeprom_strap_data, sizeof(eeprom_bootloader_t));
	if (ret) {
		deckard_log("%s: Could not read EEPROM\n", __func__);
	}
}

bool deckard_eeprom_get_wifi_mac_address(uint8_t mac[EEPROM_MAC_ADDR_LEN])
{
	if (!s_wifi_mac_addr_valid)
		return false;

	memcpy(mac, s_wifi_mac_addr, EEPROM_MAC_ADDR_LEN);
	return true;
}
