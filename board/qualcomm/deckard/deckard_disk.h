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

//USB boot disk has its own names in order to disambiguate
#define DECKARD_ESP_NAME_SLOT_U "usbesp"
#define DECKARD_ESP_SLOT_U "0:1"
#define DECKARD_EFI_NAME_SLOT_U "usbefi"
#define DECKARD_EFI_SLOT_U "0:2"
#define DECKARD_ROOTFS_SLOT_U "usbrootfs"
#define DECKARD_UBOOT_FW_SLOT_U "usbubootfw"
#define DECKARD_UBOOT_ENV_SLOT_U "usbubootenv"

char * disk_interface_to_str( enum disk_interface_t disk_interface );
int booted_from_usb(void);
char get_booted_slot(void);
int deckard_find_part_by_label(enum disk_interface_t disk_interface, const char *partname, int *devnum, int *partnum);
int do_load_helper(enum disk_interface_t disk_interface, char * partition, char * addr, char * path);
int deckard_load_dtb(enum disk_interface_t disk_interface, char * partition, char * addr);

#endif