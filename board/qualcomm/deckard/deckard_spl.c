#include <dm.h>
#include <env.h>
#include <init.h>
#include <asm/system.h>
#include <dm/device-internal.h>
#include <ufs.h>
#include <scsi.h>
#include <spl_load.h>
#include <spl.h>
#include <stdlib.h>
#include <env_internal.h>
#include <led.h>
#include <button.h>
#include <gpt.h>
#include <command.h>
#include "pmk8550.h"
#include "deckard_battery.h"
#include <sysreset.h>
#include <linux/delay.h>

#define BOOT_DEVICE_UFS_A	(BOOT_DEVICE_NONE + 1)
#define BOOT_DEVICE_UFS_B	(BOOT_DEVICE_NONE + 2)

#define DECKARD_IFNAME "scsi"

#define DECKARD_UBOOT_ENV_SLOT_A "ubootenv_a"
#define DECKARD_UBOOT_FW_SLOT_A "ubootfw_a"
#define DECKARD_UBOOT_SLOT_A "uboot_a"
#define DECKARD_BOOT_ENV_SLOT_A "bootenv"
#define DECKARD_BOOTFW_SLOT_A "xbl_a"

#define DECKARD_UBOOT_ENV_SLOT_B "ubootenv_b"
#define DECKARD_UBOOT_FW_SLOT_B "ubootfw_b"
#define DECKARD_UBOOT_SLOT_B "uboot_b"
#define DECKARD_BOOT_ENV_SLOT_B "bootenvb"
#define DECKARD_BOOTFW_SLOT_B "xbl_b"

#define BOOT_ATTEMPTS_MAX			8

//these are set as a bit-mask typically when we detect a button is enabled
#define POWER_BUTTON_MASK 0b1000
#define VOLUP_BUTTON_MASK 0b0100
#define VOLDN_BUTTON_MASK 0b0010
#define SELECT_BUTTON_MASK 0b0001
enum combined_button_state_t {
	POWER_OFF__VOLUP_OFF__VOLDN_OFF__SELECT_OFF = 0,
	POWER_OFF__VOLUP_OFF__VOLDN_OFF__SELECT_ON  = 1,
	POWER_OFF__VOLUP_OFF__VOLDN_ON__SELECT_OFF  = 2,
	POWER_OFF__VOLUP_OFF__VOLDN_ON__SELECT_ON   = 3,
	POWER_OFF__VOLUP_ON__VOLDN_OFF__SELECT_OFF  = 4,
	POWER_OFF__VOLUP_ON__VOLDN_OFF__SELECT_ON   = 5,
	POWER_OFF__VOLUP_ON__VOLDN_ON__SELECT_OFF   = 6,
	POWER_OFF__VOLUP_ON__VOLDN_ON__SELECT_ON    = 7,
	POWER_ON__VOLUP_OFF__VOLDN_OFF__SELECT_OFF  = 8,
	POWER_ON__VOLUP_OFF__VOLDN_OFF__SELECT_ON   = 9,
	POWER_ON__VOLUP_OFF__VOLDN_ON__SELECT_OFF   = 10,
	POWER_ON__VOLUP_OFF__VOLDN_ON__SELECT_ON    = 11,
	POWER_ON__VOLUP_ON__VOLDN_OFF__SELECT_OFF   = 12,
	POWER_ON__VOLUP_ON__VOLDN_OFF__SELECT_ON    = 13,
	POWER_ON__VOLUP_ON__VOLDN_ON__SELECT_OFF    = 14,
	POWER_ON__VOLUP_ON__VOLDN_ON__SELECT_ON     = 15,
};
 

DECLARE_GLOBAL_DATA_PTR;

static ulong boot_wrapper_version __section(".data");

static char *name = "uboot_a";
static char *ifname = "scsi";

static struct pon_on_off_reason pon_poff_reason;

int g_boot_wrapper_version;
/**
 * Save x0 register value, which should encode the boot wrapper version
 */
void save_boot_params(ulong r0)
{
	boot_wrapper_version = r0;
	save_boot_params_ret();
}

static int deckard_spl_get_dev(const char *part_name)
{
	char dev_part_str[MAX_DEV_PART_STR];
	char *dev_str;
	int ret;

	ret = part_get_dev_part_str_by_name(DECKARD_IFNAME, part_name,
					    dev_part_str);
	if (ret < 0)
		return ret;

	dev_str = strtok(dev_part_str, ":");
	if (!dev_str)
		return -EINVAL;

	return simple_strtoul(dev_str, NULL, 10);
}

static int deckard_spl_get_part(const char *part_name)
{
	char dev_part_str[MAX_DEV_PART_STR];
	char *part_str;
	char *dev_str;
	int ret;

	ret = part_get_dev_part_str_by_name(DECKARD_IFNAME, part_name,
					    dev_part_str);
	if (ret < 0)
		return ret;

	dev_str = strtok(dev_part_str, ":");
	if (!dev_str)
		return -EINVAL;

	part_str = strtok(NULL, ":");
	if (!part_str)
		return -EINVAL;

	return simple_strtoul(part_str, NULL, 10);
}

void spl_perform_fixups(struct spl_image_info *spl_image)
{
	/* Use relocated FDT */
	void *blob = (void *)(((ulong)spl_image->load_addr + spl_image->size + 3) & ~3);
	int chosen;
	int ret;

	//now do fdt stuff
	chosen = fdt_find_or_add_subnode(blob, 0, "chosen");
	if (chosen < 0) {
		printf("%s: could not find/create '/chosen'\n", __func__);
		return;
	}

	if (spl_image->boot_device == BOOT_DEVICE_UFS_A)
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "A");
	else if (spl_image->boot_device == BOOT_DEVICE_UFS_B)
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "B");
	else
	{
		ret = fdt_setprop_string(blob, chosen,
				"u-boot,spl-boot-device", "X");
		printf("%s: u-boot,spl-boot-device was set to an unknown value!!\n", __func__);				
	}
	if (ret) {
		printf("%s: could not add u-boot,spl-boot-device property\n", __func__);
		return;
	}

	ret = fdt_setprop_u32(blob, chosen, "boot-wrapper-version", boot_wrapper_version);
	if (ret) {
		printf("%s: could not add u-boot,spl-boot-device property\n", __func__);
		return;
	}
}

void board_boot_order(u32 *spl_boot_list)
{
	//This function is called by the SPL loader to check what to boot!

	char *boot_order = env_get("BOOT_TARGET");

	if (!strcmp(boot_order, "A B")) 
	{
		spl_boot_list[0] = BOOT_DEVICE_UFS_A;
		spl_boot_list[1] = BOOT_DEVICE_UFS_B;
	} 
	else if(!strcmp(boot_order, "B A")) 
	{
		spl_boot_list[0] = BOOT_DEVICE_UFS_B;
		spl_boot_list[1] = BOOT_DEVICE_UFS_A;
	}
	else if(!strcmp(boot_order, "A")) 
	{
		spl_boot_list[0] = BOOT_DEVICE_UFS_A;
		spl_boot_list[1] = BOOT_DEVICE_NONE;
	}
	else if(!strcmp(boot_order, "B")) 
	{
		spl_boot_list[0] = BOOT_DEVICE_UFS_B;
		spl_boot_list[1] = BOOT_DEVICE_NONE;
	}
	else
	{
		spl_boot_list[0] = BOOT_DEVICE_UFS_A;
		spl_boot_list[1] = BOOT_DEVICE_UFS_B;
	}
}

static char *check_boot_order(void)
{
	char *boot_order = env_get("BOOT_ORDER");

	if (!boot_order || 
	    (strcmp(boot_order, "A B") && strcmp(boot_order, "B A"))) {
		printf("Invalid BOOT_ORDER (%s), default to 'A B'\n", boot_order);
		env_set("BOOT_ORDER", "A B");
		return env_get("BOOT_ORDER");
	}

	return boot_order;
}

static ulong iterate_boot_count(void)
{
	char *boot_count;
	ulong count = 0;
	ulong boot_count_max = BOOT_ATTEMPTS_MAX;
	char *boot_target = env_get("BOOT_TARGET");
	char * boot_count_target = NULL;

	//check first character of BOOT_TARGET
	if ( strncmp(boot_target, "A",1) == 0 )
	{
		boot_count_target = "BOOT_COUNT_A";
		boot_count_max = BOOT_ATTEMPTS_MAX;
	}
	else if( strncmp(boot_target, "B",1) == 0 )
	{
		boot_count_target = "BOOT_COUNT_B";
		boot_count_max = BOOT_ATTEMPTS_MAX;
	}
	else
	{
		return BOOT_ATTEMPTS_MAX;
	}

	boot_count = env_get(boot_count_target);

	if (!boot_count) { 
		//variable doesnt exist, so set max boot count
		count = boot_count_max;
	}
	else
	{
		//variable exists, so decrement it until it hits 0
		count = simple_strtoul(boot_count, NULL, 10);
		if(count > 0)
		{
			count--;
		}
	}

	env_set_ulong(boot_count_target, count);

	return count;
}

/*
 * for debuging, maybe remove it later
static int show_button_state(struct udevice *dev)
{
	int ret;

	ret = button_get_state(dev);
	if (ret >= BUTTON_COUNT)
		ret = -EINVAL;
	if (ret >= 0)
		printf("%s\n", ret ? "on" : "off");

	return ret;
}

static void list_buttons(void)
{
	struct udevice *dev;
	int ret;

	for (ret = uclass_first_device_check(UCLASS_BUTTON, &dev);
	     dev;
	     ret = uclass_next_device_check(&dev)) {
		struct button_uc_plat *plat = dev_get_uclass_plat(dev);

		if (ret) {
			printf("BUTTON device %s probe error %i\n",
			       dev->name, ret);
			continue;
		}

		if (plat->label) {
			printf("%-15s ", plat->label);
			ret = show_button_state(dev);
			if (ret < 0)
				printf("Error %d\n", ret);
		}
	}
}
*/

static enum button_state_t deckard_spl_get_button_state(const char *label)
{
	enum button_state_t state;
	struct udevice *dev;
	int ret;

	ret = button_get_by_label(label, &dev);
	if (ret < 0)
		return ret;

	state = button_get_state(dev);
	if (state < 0 || state >= BUTTON_COUNT)
		return -EINVAL;

	return state;
}

static void deckard_spl_leds_set(void)
{
	struct udevice *led_blue;
	int ret;

	ret = led_get_by_label("blue", &led_blue);
	if (ret < 0)
		printf("failed to get blue led\n");

	led_set_state(led_blue, LEDST_ON);
}

static int update_boot_state_and_boot_valid(char boot_slot)
{
	int boot_state = -1;

	switch(boot_slot)
	{
		case 'A':
			if(env_get("BOOT_STATE_A"))
			{
				boot_state = env_get_ulong("BOOT_STATE_A", 10, 0);
				if(boot_state == 1)
				{
					//set boot count to max to allow it to be used
					env_set_ulong("BOOT_COUNT_A", BOOT_ATTEMPTS_MAX);
					env_set_ulong("BOOT_VALID_A", 1);
				}
				else
				{
					//set boot count to 0 so it won't try to be used
					env_set_ulong("BOOT_COUNT_A", 0);
					env_set_ulong("BOOT_VALID_A", 0); //this BOOT_VALID var will persist
				}
				env_set("BOOT_STATE_A",NULL); //always delete this variable after we process it
			}
			break;

		case 'B':
			if(env_get("BOOT_STATE_B"))
			{
				boot_state = env_get_ulong("BOOT_STATE_B", 10, 0);
				if(boot_state == 1)
				{
					//set boot count to max to allow it to be used
					env_set_ulong("BOOT_COUNT_B", BOOT_ATTEMPTS_MAX);
					env_set_ulong("BOOT_VALID_B", 1);
				}
				else
				{
					//set boot count to 0 so it won't try to be used
					env_set_ulong("BOOT_COUNT_B", 0);
					env_set_ulong("BOOT_VALID_B", 0); //this BOOT_VALID var will persist
				}
				env_set("BOOT_STATE_B",NULL); //always delete this variable after we process it
			}
			break;
		
		default:
			printf("get_boot_counts: error\n");
			break;
	}

	return boot_state;
}


static int check_for_boot_firmware_slot_change(void)
{
	char *boot_fw_slot;
	int slot_change_success;
	int ret = 0;
	int bootfw_lun;
	struct blk_desc *blk_dev;

	boot_fw_slot = env_get("BOOT_FW_SLOT");
	if (!boot_fw_slot)
	{
		//we don't run if the variable is unset
		return 0;
	}

	//todo use DECKARD_UFS_BOOTFW_LUN instead of hard code 1
	if(strcmp(boot_fw_slot, "A") == 0)
	{
		bootfw_lun = deckard_spl_get_dev(DECKARD_BOOTFW_SLOT_A);
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, bootfw_lun);
		ret = gpt_set_xbl_partition_bootable_by_slot_partfstype(blk_dev, "a");
	}
	else if(strcmp(boot_fw_slot, "B") == 0)
	{
		bootfw_lun = deckard_spl_get_dev(DECKARD_BOOTFW_SLOT_B);
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, bootfw_lun);
		ret = gpt_set_xbl_partition_bootable_by_slot_partfstype(blk_dev, "b");
	}

	printf("check_for_boot_firmware_slot_change, ret = %d\n",ret);

	env_set("BOOT_FW_SLOT",NULL); //always delete this variable after we process it

	slot_change_success = (ret == 0);

	return slot_change_success;
}

static int get_boot_counts(char boot_slot)
{
	int boot_count = -1;
	switch(boot_slot)
	{
		case 'A':
			if(env_get("BOOT_COUNT_A"))
			{
				boot_count = env_get_ulong("BOOT_COUNT_A", 10, 0);
			}
			else
			{
				env_set_ulong("BOOT_COUNT_A", BOOT_ATTEMPTS_MAX);
				boot_count = BOOT_ATTEMPTS_MAX;
			}
			break;

		case 'B':
			if(env_get("BOOT_COUNT_B"))
			{
				boot_count = env_get_ulong("BOOT_COUNT_B", 10, 0);
			}
			else
			{
				env_set_ulong("BOOT_COUNT_B", BOOT_ATTEMPTS_MAX);
				boot_count = BOOT_ATTEMPTS_MAX;
			}
			break;

		default:
			printf("get_boot_counts: error\n");
			break;
	}

	return boot_count;
}

static int get_boot_valid(char boot_slot)
{
	int boot_valid = -1;
	switch(boot_slot)
	{
		case 'A':
			//check if unset, then set a valid value
			if(! env_get("BOOT_VALID_A"))
			{
				env_set_ulong("BOOT_VALID_A", 1);
			}
			boot_valid = env_get_ulong("BOOT_VALID_A", 10, 0);
			break;

		case 'B':
			//check if unset, then set a valid value
			if(! env_get("BOOT_VALID_B"))
			{
				env_set_ulong("BOOT_VALID_B", 1);
			}
			boot_valid = env_get_ulong("BOOT_VALID_B", 10, 0);
			break;

		default:
			printf("get_boot_valid: error\n");
			break;			
	}

	return boot_valid;

}

static int determine_boot_target(void)
{
	ulong boot_count_a = 0;
	ulong boot_count_b = 0;
	int boot_state_a = 0;
	int boot_state_b = 0;
	int boot_valid_a = 0;
	int boot_valid_b = 0;
	char *boot_order;
	int boot_count_ok_a = 0;
	int boot_count_ok_b = 0;
	
	boot_state_a = update_boot_state_and_boot_valid('A');
	boot_state_b = update_boot_state_and_boot_valid('B');
	
	boot_count_a = get_boot_counts('A');
	boot_count_b = get_boot_counts('B');

	boot_valid_a = get_boot_valid('A');
	boot_valid_b = get_boot_valid('B');

	boot_order = check_boot_order(); //this will ensure the boot order is either "A B" or "B A"

	boot_count_ok_a = boot_count_a > 0;
	boot_count_ok_b = boot_count_b > 0;

	int swap_boot_order = 0;

	//check if the target slot either is invalid or has run out of boot attempts
	//if so, swap boot order
	//in either case, provide a backup slot to try (will be used by SPL if the kernel / initrd fails to be loadable / missing)
	//if we end up in a situation where both slots are invalid, we will end up just trying to boot our existing slot
	if (!strcmp(boot_order, "A B")) 
	{
		printf("BOOT_ORDER set to A as primary\n");

		if((!boot_valid_a || !boot_count_ok_a) && boot_valid_b && boot_count_ok_b) swap_boot_order = 1;
		
		if(swap_boot_order)
		{
			if(boot_valid_a) env_set("BOOT_TARGET", "B A"); //provide A as a backup since it is valid
			else env_set("BOOT_TARGET", "B");
		}
		else
		{
			if(boot_valid_b) env_set("BOOT_TARGET", "A B"); //provide B as a backup since it is valid
			else env_set("BOOT_TARGET", "A");
		}
	} 
	else if (!strcmp(boot_order, "B A")) 
	{
		printf("BOOT_ORDER set to B as primary\n");

		if((!boot_valid_b || !boot_count_ok_b) && boot_valid_a && boot_count_ok_a) swap_boot_order = 1;
		
		if(swap_boot_order)
		{
			if(boot_valid_b) env_set("BOOT_TARGET", "A B"); //provide B as a backup since it is valid
			else env_set("BOOT_TARGET", "A");
		}
		else
		{
			if(boot_valid_a) env_set("BOOT_TARGET", "B A"); //provide A as a backup since it is valid
			else env_set("BOOT_TARGET", "B");
		}
	}

	return 0;
}

static int spl_env_load(struct blk_desc *blk_dev, struct disk_partition *mainenv_info, struct disk_partition *altenv_info, void *mainbuf, void *altbuf)
{
	int env_part_a, env_part_b;
	long count;
	int ret;

	env_part_a = deckard_spl_get_part(DECKARD_BOOT_ENV_SLOT_A);
	env_part_b = deckard_spl_get_part(DECKARD_BOOT_ENV_SLOT_B);

	/* Read spl_env partition */
	ret = part_get_info(blk_dev, env_part_a, mainenv_info);
	if (ret == -ENOENT)
		panic("partition %d for env not found\n", env_part_a);

	count = blk_read(blk_dev->bdev, mainenv_info->start, mainenv_info->size, mainbuf);
	if (count < 0)
		panic("Failed to read env partition (%ld)\n", count);

	/* Load spl_alt_env right after the spl_env */
	altbuf = mainbuf + (mainenv_info->size * blk_dev->blksz);

	/* Read spl_alt_env partition */
	ret = part_get_info(blk_dev, env_part_b, altenv_info);
	if (ret == -ENOENT)
		panic("partition %d for env_alt not found\n",
		       env_part_b);

	count = blk_read(blk_dev->bdev, altenv_info->start, altenv_info->size, altbuf);
	if (count < 0)
		panic("Failed to read env_alt partition (%ld)\n", count);

	/* Open coded env_import_redund */
	ret = env_import(mainbuf, 1, 0);
	if (ret) {
		ret = env_import(altbuf, 1, 0);
		if (ret)
			printf("Failed to import environment\n");
	}

	return ret;
}

static int spl_env_save(struct blk_desc *blk_dev, struct disk_partition *mainenv_info, struct disk_partition *altenv_info, void *mainbuf, void *altbuf)
{
	int ret;

	/* Export and write env in both partitions */
    /* NOTE: blk_write() size is in blocks ( 4k )*/
	ret = env_export(mainbuf);
	if (!ret) 
	{
		//write env primary
		ret = blk_write(blk_dev->bdev, mainenv_info->start, mainenv_info->size, mainbuf);
		if (!ret) printf("SPL: Unable to write env\n");
		else printf("SPL: env written OK\n");
		
		//write env alternate
		ret = blk_write(blk_dev->bdev, altenv_info->start, altenv_info->size, mainbuf);
		if (!ret) printf("SPL: Unable to write env_alt\n");
		else printf("SPL: env_alt written OK\n");
	} 
	else
	{
		printf("Failed to export environment");
	}

	return 0;
}

void trigger_xblramdump_fault() {
	//trigger xblramdump by reading from memory we should not be reading from!
	int memfaultaddr=0x8000000;
	void * tmpbuf = map_sysmem(memfaultaddr, 4); 
	printf("causing mem fault on 0x%0X\n",memfaultaddr);
	printf("mem fault on: %x\n",*(volatile uint32_t *)tmpbuf);
	unmap_sysmem(tmpbuf);
}

enum combined_button_state_t button_state_handler(void) 
{
	enum combined_button_state_t total_button_state;

	if ( deckard_spl_get_button_state("Select") == BUTTON_ON) {
		printf("Select button pressed\n");
		total_button_state |= SELECT_BUTTON_MASK;
	}

	if ( deckard_spl_get_button_state("Volume Down") == BUTTON_ON) {
		printf("Vol- button pressed\n");
		total_button_state |= VOLDN_BUTTON_MASK;
	}

	if ( deckard_spl_get_button_state("Volume Up") == BUTTON_ON) {
		printf("Vol+ button pressed\n");
		total_button_state |= VOLUP_BUTTON_MASK;
	}

	if ( deckard_spl_get_button_state("Power Button") == BUTTON_ON) {
		printf("Power button pressed\n");
		total_button_state |= POWER_BUTTON_MASK;
	}

	printf("total button state = %d\n", total_button_state);

	switch(total_button_state)
	{
		//recommend not to use any key combinations that include POWER_ON to take any actions
		//but we could at least detect those combos if we desire

		case POWER_OFF__VOLUP_OFF__VOLDN_OFF__SELECT_OFF:
			break;
		case POWER_OFF__VOLUP_OFF__VOLDN_OFF__SELECT_ON:
			break;
		case POWER_OFF__VOLUP_OFF__VOLDN_ON__SELECT_OFF:
			break;
		case POWER_OFF__VOLUP_OFF__VOLDN_ON__SELECT_ON:
			break;
		case POWER_OFF__VOLUP_ON__VOLDN_OFF__SELECT_OFF:
			//trigger_xblramdump_fault();
			break;
		case POWER_OFF__VOLUP_ON__VOLDN_OFF__SELECT_ON:
			break;
		case POWER_OFF__VOLUP_ON__VOLDN_ON__SELECT_OFF:
			break;
		case POWER_OFF__VOLUP_ON__VOLDN_ON__SELECT_ON:
			break;
		case POWER_ON__VOLUP_OFF__VOLDN_OFF__SELECT_OFF:
			break;
		case POWER_ON__VOLUP_OFF__VOLDN_OFF__SELECT_ON:
			break;
		case POWER_ON__VOLUP_OFF__VOLDN_ON__SELECT_OFF:
			break;
		case POWER_ON__VOLUP_OFF__VOLDN_ON__SELECT_ON:
			break;
		case POWER_ON__VOLUP_ON__VOLDN_OFF__SELECT_OFF:
			break;
		case POWER_ON__VOLUP_ON__VOLDN_OFF__SELECT_ON:
			break;
		case POWER_ON__VOLUP_ON__VOLDN_ON__SELECT_OFF:
			break;
		case POWER_ON__VOLUP_ON__VOLDN_ON__SELECT_ON: 
			printf("why are you hitting all of the buttons?\n");
			break;
		default:
			break;
	}
	return 0;
}

static int set_booted_fw_slot(void)
{
	char active_partition_slot[2];
	int bootfw_lun;
	int ret = 0;
	struct blk_desc *blk_dev;

	bootfw_lun = deckard_spl_get_dev(DECKARD_BOOTFW_SLOT_A);
	blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, bootfw_lun);
	ret = gpt_get_xbl_active_partition_slot(blk_dev, active_partition_slot);
	//always check xbl_a / xbl_b to see which slot we are on
	env_set("BOOTED_FW_SLOT", active_partition_slot);

	return ret;
}

void qcom_spl_board_init(void)
{
	struct udevice *dev;
	struct disk_partition mainenv_info;
	struct disk_partition altenv_info;
	struct blk_desc *blk_dev;
	void *mainbuf = (void *)(CONFIG_SYS_LOAD_ADDR);
	void *altbuf = NULL;
	char *boot_order;
	int ret;
	int boot_count;
	int env_lun, boot_lun;
	char dev_part_str[MAX_DEV_PART_STR + 1] = { 0 };
	enum combined_button_state_t total_button_state = 0;
	int power_button_released_since_boot = 0;
	int power_button_release_rtc_time_since_pon  = 0;
	uint16_t battery_voltage;

	printf("Hello from deckard_spl\n");
	printf("spl: boot wrapper version %ld\n", boot_wrapper_version);

	ret = pmk8550_get_on_off_reason(&pon_poff_reason);
	if (ret == 0) {
		if (pon_poff_reason.trigger == PON_RESET_TRIGGER_OTHER)
		{
			printf("PON %s Unknown Trigger (Irq %d PID %d SID %d)\n",
			       pmk8550_get_pon_type_str(pon_poff_reason.type),
				pon_poff_reason.trigger_data.irq,
				pon_poff_reason.trigger_data.pid,
				pon_poff_reason.trigger_data.sid);
		}
		else
		{
			printf("PON %s Trigger %s\n",
			       pmk8550_get_pon_type_str(pon_poff_reason.type),
			       pmk8550_get_pon_trigger_str(pon_poff_reason.trigger));
		}

		printf("PON Reason = 0x%0X\n",pon_poff_reason.pon_reason);
		printf("PON BOOT1 POFF Reason = 0x%0X\n",pon_poff_reason.pon_boot1_poff_reason);
		printf("PON BOOT1 OFF Reason = 0x%0X\n",pon_poff_reason.pon_boot1_off_reason);

		// We do this check for when power button was released to check for a situation that should be impossible
		// Scenario of concern is that somehow the device appears to have been shutdown by powerbutton
		// however, the device did not immediately power back up
		// so we could potentially be confused as to if the user is:
		// 1. holding the button down to boot device after a hard shutdown (by power button hold down)
		// 2. or the user is currently shutting down the device via hard shutdown (by power button hold down)
		//    and they are holding the key down still 
		printf("PON RTC time = %d\n",pon_poff_reason.pon_rtc_time);
		printf("Power button release RTC time = %d\n",pon_poff_reason.power_button_release_rtc_time);
		power_button_release_rtc_time_since_pon = pon_poff_reason.power_button_release_rtc_time - pon_poff_reason.pon_rtc_time;

		if(power_button_release_rtc_time_since_pon <= 0)
		{
			printf("Power button not released since boot\n");
			power_button_released_since_boot = 0;
		}
		else
		{
			printf("Power button released %d ms after boot\n",power_button_release_rtc_time_since_pon);
			power_button_released_since_boot = 1;
		}

		if( pon_poff_reason.type == PON_RESET_TYPE_SHUTDOWN 
			&& pon_poff_reason.trigger == PON_RESET_TRIGGER_KPDPWR )
		{
			printf("Last Shutdown occurred by hard shutdown from power button hold down\n");

			if(power_button_released_since_boot == 0)
			{
				//loop for 5s = (500 * 10ms)
				for(int i=0; i < 100 * 5; i++)
				{
					if ( deckard_spl_get_button_state("Power Button") == BUTTON_OFF) {
						printf("Power button released\n");
						power_button_released_since_boot = 1;
						break;
					}
					udelay(10000);
				}
			}

			if(power_button_released_since_boot)
			{
				printf("SPL: shutting down...\n");
				ret = sysreset_walk(SYSRESET_POWER_OFF);
			}

			//if we get here, then it appears the user is still holding the button, so just continue booting
		}
			
	}

	//this needs to be after we check if we are currently holding the button down for a power button hard shutdown
	deckard_spl_leds_set();

	deckard_spl_battery_charging_setup(&battery_voltage);

	ret = ufs_probe();
	if (ret)
		panic("Failed to probe for UFS (%d)\n", ret);

	ret = uclass_get_device_by_seq(UCLASS_SCSI, 0, &dev);
	if (ret)
		panic("SCSI device not found (%d)\n", ret);

	ret = scsi_scan_dev(dev, true);
	if (ret)
		panic("Failed to scan SCSI (%d)\n", ret);	

	env_lun = deckard_spl_get_dev(DECKARD_UBOOT_ENV_SLOT_A);
	if (env_lun < 0)
		panic("lun for env not found\n");

	blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, env_lun);
	if (!blk_dev)
		panic("blk desc for env not found\n");

	blk_show_device(UCLASS_SCSI, env_lun);

	if (boot_wrapper_version == 9001)
	{
		//todo: handle this variable being set by bootwrapper if we want to use for something
		//previously was used to force rescue from boot-wrapper button push
	}

	spl_env_load(blk_dev, &mainenv_info, &altenv_info, mainbuf, altbuf);

	total_button_state = button_state_handler();

	if(check_for_boot_firmware_slot_change())
	{
		printf("Boot firmware changed\n");
		
		ret = spl_env_save(blk_dev, &mainenv_info, &altenv_info, mainbuf, altbuf);
		if( ret == 0 ) ret = spl_env_load(blk_dev, &mainenv_info, &altenv_info, mainbuf, altbuf);

		if(ret == 0)
		{
			char *boot_fw_slot = env_get("BOOT_FW_SLOT");
			if (!boot_fw_slot) 
			{
				//confirmed that the variable was removed, safe to reboot now
				printf("rebooting!\n");
				do_reset(NULL, 0, 0, NULL);
			}
		}
	}

	set_booted_fw_slot();

	/* Sanitize BOOT_ORDER && BOOT_COUNT */
	boot_order = check_boot_order();

	printf("Environment Variables:\n");
	printf("- BOOT_ORDER   = '%s'\n", env_get("BOOT_ORDER"));
	printf("- BOOT_COUNT_A = '%s'\n", env_get("BOOT_COUNT_A"));
	printf("- BOOT_COUNT_B = '%s'\n", env_get("BOOT_COUNT_B"));
	printf("- BOOT_STATE_A = '%s'\n", env_get("BOOT_STATE_A"));
	printf("- BOOT_STATE_B = '%s'\n", env_get("BOOT_STATE_B"));
	printf("- BOOT_VALID_A = '%s'\n", env_get("BOOT_VALID_A"));
	printf("- BOOT_VALID_B = '%s'\n", env_get("BOOT_VALID_B"));
	printf("- BOOTED_FW_SLOT = '%s'\n", env_get("BOOTED_FW_SLOT"));

	determine_boot_target();

	// Only iterate boot count if we believe this should be a successful boot based on battery level
	if ( battery_voltage >= SPL_BATTERY_MIN_BOOT_VOLTAGE ) 
	{
		boot_count = iterate_boot_count();
	}
	else
	{
		printf("boot count not iterated due to low battery voltage\n");
	}
	printf("Boot count on target slot is %d\n",boot_count);

	spl_env_save(blk_dev, &mainenv_info, &altenv_info, mainbuf, altbuf);
	
}

static ulong deckard_ufs_fit_read(struct spl_load_info *load, ulong file_offset,
				  ulong size, void *buf)
{
	struct blk_desc *blk_dev = (struct blk_desc *)load->priv;
	long count;

	debug("%s: off %lx size %lx buf %lx\n", __func__, file_offset, size, (ulong)buf);

	count = blk_read(blk_dev->bdev, file_offset / blk_dev->blksz,
			size / blk_dev->blksz, buf);
	if (count < 0)
		return 0;

	return count * blk_dev->blksz;
}

static int deckard_ufs_load_image(struct spl_image_info *spl_image,
				  struct spl_boot_device *bootdev)
{
	struct blk_desc *blk_dev;
	struct spl_load_info load;
	struct disk_partition info;
	int uboot_part_a, uboot_part_b;
	int uboot_lun;
	unsigned int partid;
	int ret;

	blk_dev = NULL;

	uboot_lun = deckard_spl_get_dev(DECKARD_UBOOT_SLOT_A);
	if (uboot_lun < 0) {
		printf("uboot lun with partition name %s not found\n",
			DECKARD_UBOOT_SLOT_A);
		return -ENODEV;
	}

	uboot_part_a = deckard_spl_get_part(DECKARD_UBOOT_SLOT_A);
	if (uboot_part_a < 0) {
		printf("uboot partition with name %s not found\n",
			DECKARD_UBOOT_SLOT_A);
		return -ENODEV;
	}
	partid = uboot_part_a;

	if (bootdev->boot_device == BOOT_DEVICE_UFS_B) {
		uboot_part_b = deckard_spl_get_part(DECKARD_UBOOT_SLOT_B);
		if (uboot_part_b < 0) {
			printf("uboot partition with name %s not found\n",
				DECKARD_UBOOT_SLOT_B);
			return -ENODEV;
		}
		partid = uboot_part_b;
	}

	if (bootdev->boot_device == BOOT_DEVICE_UFS_A ||
	    bootdev->boot_device == BOOT_DEVICE_UFS_B) {
		blk_dev = blk_get_devnum_by_uclass_id(UCLASS_SCSI, uboot_lun);
		if (!blk_dev) {
			printf("blk desc for %d %d not found\n", UCLASS_SCSI, uboot_lun);
			return -ENODEV;
		}

		blk_show_device(UCLASS_SCSI, uboot_lun - 1);
	}

	ret = part_get_info(blk_dev, partid, &info);
	if (ret == -ENOENT) {
		printf("partition %d for blk desc for %d %d not found\n",
		       partid, UCLASS_SCSI, uboot_lun);
		return -ENODEV;
	}

	load.read = deckard_ufs_fit_read;
	load.priv = blk_dev;
	load.bl_len = blk_dev->blksz;

	return spl_load(spl_image, bootdev, &load,
			info.size * blk_dev->blksz,
			info.start * blk_dev->blksz);
}

SPL_LOAD_IMAGE_METHOD("UFS_A", 0, BOOT_DEVICE_UFS_A, deckard_ufs_load_image);
SPL_LOAD_IMAGE_METHOD("UFS_B", 0, BOOT_DEVICE_UFS_B, deckard_ufs_load_image);
