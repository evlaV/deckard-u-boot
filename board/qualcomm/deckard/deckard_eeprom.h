/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard EEPROM
 *
 * Layouts here are mirrored from the userspace deckard-eeprom tool
 * (eeprom_core_v2.c, eeprom_strap_v2.c). Keep them in sync:
 * every structure is packed and its size is part of the on-disk format.
 */

#ifndef __DECKARD_EEPROM_H
#define __DECKARD_EEPROM_H

#include <init.h>
#include <linux/stddef.h>
#include <stdbool.h>

// BOOTLODAER DATA //
#define EEPROM_BOOTLOADER_MAGIC_NUMBER     (0x45f731ea)
#define EEPROM_VERSION_STRING_LEN          (32)

#define EEPROM_VALID                       (0xA5)
#define EEPROM_LOCKED                      (0x00)
#define EEPROM_UNLOCKED                    (0xFF)
#define EEPROM_BOOTLOADER_ADDRESS_START    (0x000)
#define EEPROM_CORE_ADDRESS_START          (0x100)

// CORE DATA //
#define EEPROM_CORE_MAGIC_NUMBER           (0x45f731ea)
#define EEPROM_MAC_ADDR_LEN                (6)
#define EEPROM_VALVE_SN_STRING_LEN         (14)
#define EEPROM_VENDOR_SN_STRING_LEN        (31)
#define EEPROM_CONFIG_STRING_LEN           (32)

// STRAP DATA //
// Only the EV3.3-onwards strap part is supported ("v2": 8 bit addressing, 256
// bytes, i2c 0x51). The pre-EV3.3 layout on 0x50 used a different magic number
// and is deliberately not handled - EV3.2 and earlier are out of scope.
#define EEPROM_STRAP_V2_MAGIC_NUMBER       (0x544F474F)
#define EEPROM_STRAP_ADDRESS_START         (0x000)
// v2 shrank the revision/minor/config fields to squeeze under 256 bytes
#define EEPROM_STRAP_V2_STRING_LEN         (8)

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

/// @brief Maps an on-EEPROM header version to the record length it implies.
/// The checksum always occupies the final two bytes of that length, so the
/// version has to be resolved before a record can be verified.
typedef struct
{
	uint32_t version;
	uint32_t length;
} eeprom_version_len_t;

// Bootloader partition, version 3 (EV2 onwards). The whole partition is
// exactly 256 bytes; `reserved` absorbs whatever is left over.
#define EEPROM_BOOTLOADER_RESERVED                                                                                     \
    (EEPROM_CORE_ADDRESS_START - sizeof(eeprom_header_t) - sizeof(eeprom_checksum_t) - 2 * EEPROM_VERSION_STRING_LEN - \
     3 * sizeof(uint8_t) - sizeof(char))
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

    // EV2 and later only
    char imu_rev;
    uint8_t imu_rev_valid;

    uint8_t reserved[EEPROM_BOOTLOADER_RESERVED];

    eeprom_checksum_t checksum;
} __attribute__((packed)) eeprom_bootloader_t;

_Static_assert(sizeof(eeprom_bootloader_t) == EEPROM_CORE_ADDRESS_START,
	       "EEPROM Bootloader must be exactly 256 bytes");

typedef struct
{
    int16_t x;
    int16_t y;
    int16_t z;
} __attribute__((packed)) ipd_position_t;

// Core partition, declared at its widest (version 5). Versions 3 (EV2/EV3),
// 4 (DV1) and 5 (DV2/PV) share this exact prefix and simply stop earlier, so a
// shorter record read into this struct leaves the trailing members zeroed.
// Anything past `diplexer` must be gated on the header version.
typedef struct
{
    eeprom_header_t hdr;

    // NOTE All strings are NULL Terminated on EEPROM

    char mpcba_sn[EEPROM_VALVE_SN_STRING_LEN];
    uint8_t mpcba_sn_valid;

    char unit_sn[EEPROM_VALVE_SN_STRING_LEN];
    uint8_t unit_sn_valid;
    char unit_config[EEPROM_CONFIG_STRING_LEN];
    uint8_t unit_config_valid;

    uint8_t mac_addr_wifi[EEPROM_MAC_ADDR_LEN];
    uint8_t mac_addr_wifi_valid;
    uint8_t mac_addr_ble[EEPROM_MAC_ADDR_LEN];
    uint8_t mac_addr_ble_valid;

    uint16_t prox_near;
    uint8_t prox_near_valid;
    uint16_t prox_mid;
    uint8_t prox_mid_valid;
    uint16_t prox_far;
    uint8_t prox_far_valid;

    ipd_position_t ipd_min;
    uint8_t ipd_min_valid;
    ipd_position_t ipd_max;
    uint8_t ipd_max_valid;

    char core_cm_sn[EEPROM_VENDOR_SN_STRING_LEN];
    uint8_t core_cm_sn_valid;
    char core_assembly_cm_sn[EEPROM_VENDOR_SN_STRING_LEN];
    uint8_t core_assembly_cm_sn_valid;
    char heat_pipe_sn[EEPROM_VENDOR_SN_STRING_LEN];
    uint8_t heat_pipe_sn_valid;
    char fan_sn[EEPROM_VENDOR_SN_STRING_LEN];
    uint8_t fan_sn_valid;

    uint16_t diplexer;
    uint8_t diplexer_valid;

    // version 4 and later
    char midwall_heat_pipe_sn[EEPROM_VENDOR_SN_STRING_LEN];
    uint8_t midwall_heat_pipe_sn_valid;

    // version 5 and later
    uint16_t prox_noise;
    uint8_t prox_noise_valid;

    eeprom_checksum_t checksum;
} __attribute__((packed)) eeprom_core_t;

// Record length implied by each core header version. The trailing members of
// eeprom_core_t simply do not exist on the older layouts, so the checksum for a
// given version sits two bytes before the end of that version's length.
#define EEPROM_CORE_V3_LENGTH (offsetof(eeprom_core_t, midwall_heat_pipe_sn) + sizeof(eeprom_checksum_t))
#define EEPROM_CORE_V4_LENGTH (offsetof(eeprom_core_t, prox_noise) + sizeof(eeprom_checksum_t))
#define EEPROM_CORE_V5_LENGTH (sizeof(eeprom_core_t))

// Cross-check against the sizes the userspace tool computes for the same
// layouts (eeprom_core_3_t / _4_t / _5_t in eeprom_core_v2.c).
_Static_assert(EEPROM_CORE_V3_LENGTH == 242, "core v3 record length changed");
_Static_assert(EEPROM_CORE_V4_LENGTH == 274, "core v4 record length changed");
_Static_assert(EEPROM_CORE_V5_LENGTH == 277, "core v5 record length changed");
_Static_assert(EEPROM_CORE_ADDRESS_START + sizeof(eeprom_core_t) <= 0x300,
	       "core record overruns the region reserved for it");

// Strap partition, v2 layout (header version 1). Must stay under 256 bytes.
typedef struct
{
    eeprom_header_t hdr;

    // NOTE All strings are NULL Terminated on EEPROM

    char strap_sn[EEPROM_VALVE_SN_STRING_LEN];
    uint8_t strap_sn_valid;

    char strap_revision[EEPROM_STRAP_V2_STRING_LEN];
    uint8_t strap_revision_valid;
    // Minor Revision Starts at 0 and increments 1,2,3,etc...
    char strap_minor[EEPROM_STRAP_V2_STRING_LEN];
    uint8_t strap_minor_valid;
    char strap_config[EEPROM_STRAP_V2_STRING_LEN];
    uint8_t strap_config_valid;
    char audio_module_vendor[EEPROM_CONFIG_STRING_LEN];
    uint8_t audio_module_vendor_valid;

    char left_audio_sn[EEPROM_CONFIG_STRING_LEN];
    uint8_t left_audio_sn_valid;
    uint16_t left_audio_temp;
    uint8_t left_audio_temp_valid;
    uint16_t left_audio_resistance;
    uint8_t left_audio_resistance_valid;

    char right_audio_sn[EEPROM_CONFIG_STRING_LEN];
    uint8_t right_audio_sn_valid;
    uint16_t right_audio_temp;
    uint8_t right_audio_temp_valid;
    uint16_t right_audio_resistance;
    uint8_t right_audio_resistance_valid;

    eeprom_checksum_t checksum;
} __attribute__((packed)) eeprom_strap_v2_t;

_Static_assert(sizeof(eeprom_strap_v2_t) == 164, "strap v2 record length changed");
_Static_assert(sizeof(eeprom_strap_v2_t) <= 256, "strap v2 record must fit in a 256 byte EEPROM");

/// @brief Read and cache the core and strap EEPROMs. Called once during board
/// init so nothing later has to touch i2c.
void deckard_eeprom_init(void);

/// @brief Fetch the cached wifi MAC. Returns false if the EEPROM did not carry
/// a valid one.
bool deckard_eeprom_get_wifi_mac_address(uint8_t mac[EEPROM_MAC_ADDR_LEN]);

/// @brief Emit audio calibration data and hardware information into devicetree.
/// These are used by the max98390 amp driver to tune dynamic speaker
/// management, set device-specific volume limits, and adjust userspace speaker
/// tuning.
int deckard_eeprom_fixup_audio_info(void *fdt);

/// @brief Emit the cached immutable board identity into /board-info in @fdt so
/// userspace can read it out of /sys/firmware/devicetree/base/board-info/
/// without going near i2c. Only fields the EEPROM marked valid are emitted,
/// and their values are forwarded verbatim as ordinary NUL terminated device
/// tree strings.
int deckard_eeprom_fixup_board_info(void *fdt);

#endif
