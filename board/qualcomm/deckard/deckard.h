#ifndef __DECKARD__H
#define __DECKARD__H

#include <stdbool.h>

#define DECKARD_SHUTDOWN_TIMEOUT_S 10
#define DECKARD_SHUTDOWN_TIMEOUT_US (DECKARD_SHUTDOWN_TIMEOUT_S * 1000 * 1000)


struct udevice;

const char *deckard_get_fw_part(void);
bool deckard_shutdown_condition(struct udevice *pmic);

// TODO: move to deckard_fs

#define DECKARD_UBOOT_ENV_SLOT_A "ubootenv_a"
#define DECKARD_UBOOT_FW_SLOT_A "ubootfw_a"
#define DECKARD_ROOTFS_SLOT_A "rootfs-A"
#define DECKARD_UBOOT_SLOT_A "uboot_a"

#define DECKARD_UBOOT_ENV_SLOT_B "ubootenv_b"
#define DECKARD_UBOOT_FW_SLOT_B "ubootfw_b"
#define DECKARD_ROOTFS_SLOT_B "rootfs-B"
#define DECKARD_UBOOT_SLOT_B "uboot_b"

//USB boot disk has its own names in order to disambiguate
#define DECKARD_ESP_NAME_SLOT_U "usbesp"
#define DECKARD_ESP_SLOT_U "0:1"
#define DECKARD_EFI_NAME_SLOT_U "usbefi"
#define DECKARD_EFI_SLOT_U "0:2"
#define DECKARD_ROOTFS_SLOT_U "usbrootfs"
#define DECKARD_UBOOT_FW_SLOT_U "usbubootfw"
#define DECKARD_UBOOT_ENV_SLOT_U "usbubootenv"

#define DEVPARTMAXLEN 16
#define LOADADDRMAXLEN 16
#define PATHMAXLEN 256
#define LOGO_PATHMAX 256

//add these names as alternatives for supporting old ev2 layouts -- delete once we stop using ev2's
#define ALLOW_OLD_ROOTFS_NAME_FALLBACK
#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
  #define DECKARD_ROOTFS_SLOT_OLD_A "system0"
  #define DECKARD_ROOTFS_SLOT_OLD_B "system1"
#endif

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

enum disk_interface_t {
	DISK_INTERFACE_SCSI = 0,
	DISK_INTERFACE_USB = 1,
};

char * disk_interface_to_str( enum disk_interface_t disk_interface );
int booted_from_usb(void);
char get_booted_slot(void);
char *deckard_get_dev_part_str(const char *part_name);

#endif /* __DECKARD_H */
