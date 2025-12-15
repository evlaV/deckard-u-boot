#include <dm.h>
#include <efi_loader.h>
#include <env.h>
#include <fs.h>
#include <part.h>

#include "deckard.h"
#include "deckard_disk.h"
#include "deckard_log.h"

char g_esp_part[DEVPARTMAXLEN] = { 0 };  //e.g. "1:d"

struct deckard_esp_info {
	bool valid;
	int devnum;
	int partnum;
};

static struct deckard_esp_info g_efivar_store = { 0 };

char * disk_interface_to_str(enum disk_interface_t disk_interface)
{
	switch(disk_interface)
	{
		case DISK_INTERFACE_SCSI:
			return "scsi";
		case DISK_INTERFACE_USB:
			return "usb";
		default:
			return "unknown";
	}
	return "unknown";
}

int deckard_find_efi_var_store(void)
{
	char partname[16] = { 0 };
	char *slot;
	enum disk_interface_t disk_interface = DISK_INTERFACE_SCSI;
	
	if (g_efivar_store.valid)
		return 0;

	if( booted_from_usb() ) disk_interface = DISK_INTERFACE_USB;

	slot = env_get("booted_slot");
	/* Build the partition name (efi-A/efi-B) */
	strlcat(partname, "efi-", 16);
	strlcat(partname, slot, 16);

	if (!deckard_find_part_by_label(disk_interface, partname, &g_efivar_store.devnum, &g_efivar_store.partnum)) {
		g_efivar_store.valid = true;
		return 0;
	}

	return -ENOENT;
}

void init_esp(void)
{
	if( booted_from_usb() )
	{
		snprintf(g_esp_part, DEVPARTMAXLEN, DECKARD_ESP_SLOT_U);
	}
	else
	{
		snprintf(g_esp_part, DEVPARTMAXLEN, DECKARD_ESP_PARTITION);
	}
}

/* EFI booting with EFI vars on a custom partition */
efi_status_t efi_set_blk_dev_to_system_partition(void)
{
	int r;
	char part_str[10];

	r = deckard_find_efi_var_store();
	if (r)
		return EFI_DEVICE_ERROR;
	// It would be nice to use fs_set_blk_dev_with_part() but it doesn't seem to behave quite right
	snprintf(part_str, sizeof(part_str), "%x:%x",
		 g_efivar_store.devnum, g_efivar_store.partnum);
	// deckard_log("%s: %s\n", __func__, part_str);
	r = fs_set_blk_dev(blk_get_uclass_name(efi_system_partition.uclass_id),
			   part_str, FS_TYPE_ANY);
	if (r)
		return EFI_DEVICE_ERROR;

	return EFI_SUCCESS;
}

int booted_from_usb(void)
{
	if(get_booted_slot() == 'U')
		return 1;
	else
		return 0;
}

char get_booted_slot(void)
{
	char * bootedslot;
	char bootedslot_char='X';
	bootedslot = env_get("booted_slot");
	if(bootedslot) bootedslot_char = bootedslot[0];

	return bootedslot_char;
}

// return the static slotname
static const char *env_scsi_get_dev_part_slotname(void)
{
	ofnode node = ofnode_path("/chosen");
	if (ofnode_valid(node)) {
		const char * bootdev = ofnode_read_string(node, "u-boot,spl-boot-device");
		if (!bootdev) {
			deckard_log("No u-boot,spl-boot-device, considering slot A\n");
			return DECKARD_UBOOT_ENV_SLOT_A;
		} else {
			deckard_log("Booted on slot '%s'\n", bootdev);

			switch (bootdev[0]) {
			case 'A':
				return DECKARD_UBOOT_ENV_SLOT_A;
			case 'B':
				return DECKARD_UBOOT_ENV_SLOT_B;
			case 'U':
				return NULL; //todo, figure out if we want to load an env for USB boot?
			default:
				return DECKARD_UBOOT_ENV_SLOT_A;
			}
		}
	} else {
		deckard_log("No chosen node default to A!\n");
		return DECKARD_UBOOT_ENV_SLOT_A;
	}
}

const char *env_scsi_get_dev_part(void)
{
	const char * slotname = env_scsi_get_dev_part_slotname();

	// We cannot change this signature, so we have to store it in static memory for return
	// TODO: we could rotate through multiple buffers if more than one of these need
	// to be in flight, concurrently
	static char s_device_partition[DEVPARTMAXLEN] = { 0 };
	if ( slotname && deckard_get_dev_part_str_buf( slotname, &s_device_partition[0], sizeof(s_device_partition ) ) == 0 )
	{
		return s_device_partition;
	}
	else
	{
		return NULL;
	}
}


const char *deckard_get_booted_firmware_slotname(void)
{
	switch( get_booted_slot() )
	{
		case 'B':
			return DECKARD_UBOOT_FW_SLOT_B;
		case 'U':
			return DECKARD_UBOOT_FW_SLOT_U;
		case 'A':
		default:
			return DECKARD_UBOOT_FW_SLOT_A;
	}
}

const char *deckard_get_booted_esp_part(void)
{
	return booted_from_usb() ? DECKARD_ESP_SLOT_U : DECKARD_ESP_PARTITION;
}

const char *fs_firmware_get_partition(void)
{
	// We cannot change this signature, so we have to store it in static memory for return
	// TODO: we could rotate through multiple buffers if more than one of these need
	// to be in flight, concurrently
	static char s_firmware_partition[DEVPARTMAXLEN] = { 0 };

	const char * slotname = deckard_get_booted_firmware_slotname();
	if ( slotname && deckard_get_dev_part_str_buf( slotname, &s_firmware_partition[0], sizeof(s_firmware_partition ) ) == 0 )
	{
		return s_firmware_partition;
	}
	else
	{
		return NULL;
	}

	return s_firmware_partition;
}

/* Get the devnum/partnum for a partition from its GPT partlabel */
int deckard_find_part_by_label(enum disk_interface_t disk_interface, const char *partname, int *devnum, int *partnum)
{
	int r;
	struct udevice *blkdev;
	struct blk_desc *desc;
	struct uclass *blk_class;
	struct disk_partition part;
	enum uclass_id target_uclass_id = UCLASS_SCSI;
	uclass_get(UCLASS_BLK, &blk_class);

	if(disk_interface == DISK_INTERFACE_USB) target_uclass_id = UCLASS_USB;

	/* Devices are already probed! */
	uclass_foreach_dev(blkdev, blk_class) {
		if (device_get_uclass_id(blkdev) != UCLASS_BLK)
			continue;

		desc = dev_get_uclass_plat(blkdev);
		if (!desc || desc->part_type == PART_TYPE_UNKNOWN)
			continue;
		/* Only look for non-removable SCSI devices (internal storage) */
		// FIXME: we might need a more reliable way to identify internal storage!
		if (desc->uclass_id != target_uclass_id || desc->removable)
			continue;

		for (*partnum = 1;; (*partnum)++) {
			r = part_get_info(desc, *partnum, &part);
			/* Last partition, go next */
			if (r < 0)
				break;
			/* Skip if the name doesn't match */
			if (strncmp(partname, part.name, strlen(partname)))
				continue;
			// printf("Found EFI partition '%s' (%d:%d) blksz %#lx\n", part.name, desc->devnum, *partnum, desc->blksz);
			*devnum = desc->devnum;

			return 0;
		}
	}

	return -ENODEV;
}


int deckard_get_dev_part_str_buf(const char *part_name, char *out, int out_sz)
{
	if (!part_name || !out || out_sz <= 0)
		return -EINVAL;

	out[0] = '\0';

	// TODO: use MAX_DEV_PART_STR+1 instead?
	// why the discrepancy vs DEVPARTMAXLEN
	char tmp[DEVPARTMAXLEN+1] = { 0 };  // +1 for null terminator

	enum disk_interface_t disk_interface = booted_from_usb() ? DISK_INTERFACE_USB : DISK_INTERFACE_SCSI;
	int ret = part_get_dev_part_str_by_name(disk_interface_to_str(disk_interface), part_name, tmp);
	if (ret < 0)
		return ret;

	size_t bytes_needed = strnlen( tmp, sizeof( tmp ) ) + 1; // +1 for null terminator
	if ( (int) bytes_needed > out_sz )
		return -ENOSPC;

	memcpy( out, tmp, bytes_needed );
	return 0;
}

int do_load_helper(enum disk_interface_t disk_interface, char * partition, char * addr, char * path)
{
	int  argc = 5;
	char *argv[5];
	char arg0[] = "do_the_dew"; // ?
	int ret;

	argv[0]=arg0;
	argv[1]=disk_interface_to_str(disk_interface);
	argv[2]=partition; //e.g. "1:d"
	argv[3]=addr; //e.g. "0xa7000000"
	argv[4]=path; // e.g. "/boot/maindtb.dtb"
	
	//try with BTRFS first while ignoring the blkdev error, then if that fails, then try with EXT4
	ret = do_load(NULL, FS_DO_LOAD_IGNORE_BLKDEV_ERROR, argc, argv, FS_TYPE_BTRFS);
	if (ret) 
	{
		ret = do_load(NULL, 0, argc, argv, FS_TYPE_EXT);
	}

	return ret;
}


// find dtb path based on env and eeprom settings, load dtb from flash to memory
// returns 0 on success
// returns -1 on failure
int deckard_load_dtb(enum disk_interface_t disk_interface, char * partition, char * addr)
{
	char * dtb_qcom;
	char * eeprom_board;
	char DTB_TARGET[PATHMAXLEN];

	// Get DTB
	dtb_qcom = env_get("DTB_QCOM");
	eeprom_board = env_get("EEPROM_BOARD");

	// try custom dtb from fw env
	if (dtb_qcom) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/%s", dtb_qcom);
		// load dtb
		if ( do_load_helper(disk_interface, partition, addr, DTB_TARGET) == 0 ) {
			return 0;
		}
	}

	// try dtb set in eeprom
	if (eeprom_board) {
		snprintf(DTB_TARGET, PATHMAXLEN, "/boot/dtbqcom/sm8650-%s.dtb", eeprom_board);
		// load dtb
		if ( do_load_helper(disk_interface, partition, addr, DTB_TARGET) == 0 ) {
			return 0;
		}
	}

	// try defualt dtb
	snprintf(DTB_TARGET, PATHMAXLEN, "/boot/maindtb.dtb");
	return do_load_helper(disk_interface, partition, addr, DTB_TARGET);
}
