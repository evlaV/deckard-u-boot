/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard EEPROM
 *
 */

#ifndef __DECKARD_EEPROM_H
#define __DECKARD_EEPROM_H

#include <common.h>

// BOOTLODAER DATA //
#define EEPROM_BOOTLOADER_EXPECTED_VERSION (0x00000001)
#define EEPROM_BOOTLOADER_MAGIC_NUMBER     (0x45f731ea)
#define EEPROM_VERSION_STRING_LEN          (32)

#define EEPROM_VALID                       (0xA5)
#define EEPROM_LOCKED                      (0x00)
#define EEPROM_UNLOCKED                    (0xFF)
#define EEPROM_BOOTLOADER_ADDRESS_START    (0x000)
#define EEPROM_CORE_ADDRESS_START          (0x100)

typedef struct
{
    uint32_t magic_number;
    uint32_t version;
    uint8_t locked;
} __attribute__((packed)) eeprom_header_t;

typedef union _eeprom_checksum
{
	uint16_t value;
	uint8_t  bytes[2];
} __attribute__((packed)) eeprom_checksum_t;

#if (EEPROM_BOOTLOADER_EXPECTED_VERSION == 0x00000001)
#define EEPROM_BOOTLOADER_RESERVED                                                                                     \
    (EEPROM_CORE_ADDRESS_START - sizeof(eeprom_header_t) - sizeof(eeprom_checksum_t) - 2 * EEPROM_VERSION_STRING_LEN - \
     2 * sizeof(uint8_t))
typedef struct
{
    eeprom_header_t hdr;

    // NOTE All strings are NULL Terminated on EEPROM

    // "ev1", etc... Revision is lowercase
    char board_revision[EEPROM_VERSION_STRING_LEN];
    uint8_t board_revision_valid;
    // Minor Revision Starts at 0 and increments 1,2,3,etc...
    char board_minor[EEPROM_VERSION_STRING_LEN];
    uint8_t board_minor_valid;

    uint8_t reserved[EEPROM_BOOTLOADER_RESERVED];

    eeprom_checksum_t checksum;
} __attribute__((packed)) eeprom_bootloader_t;
#endif // EEPROM_BOOTLOADER_EXPECTED_VERSION

void deckard_eeprom_init(void);

#endif