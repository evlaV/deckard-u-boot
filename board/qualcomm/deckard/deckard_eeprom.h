/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard EEPROM
 *
 */

#ifndef __DECKARD_EEPROM_H
#define __DECKARD_EEPROM_H

#include <common.h>

// CORE DATA //
#define EEPROM_CORE_MAGIC_NUMBER        (0x45f731ea)
#define EEPROM_MAC_ADDR_LEN             (6)
#define EEPROM_VERSION_STRING_LEN       (32)
#define EEPROM_CORE_SN_STRING_LEN       (14)    // 13 ASCII + NULL

typedef union _eeprom_checksum
{
	uint16_t value;
	uint8_t  bytes[2];
} __attribute__((packed)) eeprom_checksum_t;

typedef struct{
	uint32_t magic_number;
	uint32_t data_version;
	bool locked;
	char board_version[EEPROM_VERSION_STRING_LEN];
	char mpca_sn[EEPROM_CORE_SN_STRING_LEN];
	char unit_sn[EEPROM_CORE_SN_STRING_LEN];
	uint8_t mac_addr_wifi[EEPROM_MAC_ADDR_LEN];
	uint8_t mac_addr_ble[EEPROM_MAC_ADDR_LEN];
	eeprom_checksum_t checksum;
} __attribute__((packed)) eeprom_data_t;

void deckard_eeprom_init(void);

#endif