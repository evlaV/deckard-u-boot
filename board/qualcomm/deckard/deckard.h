#ifndef __DECKARD__H
#define __DECKARD__H

#include <stdbool.h>

#define DECKARD_SHUTDOWN_TIMEOUT_S 10
#define DECKARD_SHUTDOWN_TIMEOUT_US (DECKARD_SHUTDOWN_TIMEOUT_S * 1000 * 1000)


struct udevice;

const char *deckard_get_fw_part(void);
bool deckard_shutdown_condition(struct udevice *pmic);

#define DEVPARTMAXLEN 16
#define LOADADDRMAXLEN 16
#define PATHMAXLEN 256
#define LOGO_PATHMAX 256

//TODO: search for the partlabel with "esp" on LUN0 rather than hardcode here!
#define DECKARD_ESP_PARTITION_NAME "esp"
//hardcoded for now
#define DECKARD_ESP_PARTITION "0:1"

//TODO: search for "efi-A" and "efi-B"
#define DECKARD_EFI_NAME_SLOT_A "efi-A"
#define DECKARD_EFI_NAME_SLOT_B "efi-B"
//hardcoded for now
#define DECKARD_EFI_SLOT_A "0:2"
#define DECKARD_EFI_SLOT_B "0:3"

#define ENV_EFI_ENABLED "enable-efi"
#define ENV_EFI_REMOVABLE_BOOT "efi-removable-boot"
#define ENV_EFI_RESET_BOOT_ENTRIES "efi-reset-boot-entries"
/* Our boot entry is always 0xFAFF for convenience and to avoid conflicts */
#define EFI_INTERNAL_BOOT_INDEX (0xFAFF)

#define DECKARD_USB_PGOOD_DELAY 1000
// FIXME WTF: This leaks memory. 
char *deckard_get_dev_part_str(const char *part_name);

#endif /* __DECKARD_H */
