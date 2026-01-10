/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard disk handling
 *
 */

#ifndef __DECKARD_DISK_H
#define __DECKARD_DISK_H

enum disk_interface_t {
	DISK_INTERFACE_SCSI = 0,
	DISK_INTERFACE_USB = 1,
};

#define DECKARD_UBOOT_ENV_SLOT_A "ubootenv_a"
#define DECKARD_UBOOT_FW_SLOT_A "ubootfw_a"
#define DECKARD_ROOTFS_SLOT_A "rootfs-A"
#define DECKARD_UBOOT_SLOT_A "uboot_a"

#define DECKARD_UBOOT_ENV_SLOT_B "ubootenv_b"
#define DECKARD_UBOOT_FW_SLOT_B "ubootfw_b"
#define DECKARD_ROOTFS_SLOT_B "rootfs-B"
#define DECKARD_UBOOT_SLOT_B "uboot_b"

//add these names as alternatives for supporting old ev2 layouts -- delete once we stop using ev2's
#define ALLOW_OLD_ROOTFS_NAME_FALLBACK
#ifdef ALLOW_OLD_ROOTFS_NAME_FALLBACK
  #define DECKARD_ROOTFS_SLOT_OLD_A "system0"
  #define DECKARD_ROOTFS_SLOT_OLD_B "system1"
#endif

char * disk_interface_to_str( enum disk_interface_t disk_interface );
int booted_from_usb(void);
char get_booted_slot(void);
int deckard_find_part_by_label(enum disk_interface_t disk_interface, int devnum_restrict, const char *partname, int *devnum, int *partnum);
int do_load_helper(enum disk_interface_t disk_interface, char * partition, char * addr, char * path);
int deckard_load_dtb(enum disk_interface_t disk_interface, char * partition, char * addr);
int deckard_find_efi_var_store(void);
int init_steamos_efi_uuid_part_arg(char * efi_part_uuid_arg);

// gets dev:part string for partition name.  return 0 on success
int deckard_get_dev_part_str_buf( const char *part_name, char *out, int outsize );
const char *deckard_get_booted_firmware_slotname(void);
void deckard_get_booted_esp_part(char * disk_part_str, int disk_part_str_len);

#endif