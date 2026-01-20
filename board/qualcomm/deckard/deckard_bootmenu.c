#include "deckard.h"
#include "deckard_bootmenu.h"
#include "deckard_disk.h"
#include "deckard_log.h"

#include <command.h>
#include <env.h>
#include <linux/types.h>
#include <mapmem.h>
#include <memalign.h>
#include <membuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vsprintf.h>
#include <fs.h>
#include <usb.h>


static int deckard_build_slot_menu_items(char * current_slot_menu_str, int current_slot_menu_str_sz, char * previous_slot_menu_str, int previous_slot_menu_str_sz);

int splctl_force_usb_set_boot_target(char * vendor_var, char * product_var, char * serial_var);
int deckard_set_spl_usb_boot_target(void);

static void trim_quotes(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        s[len - 1] = '\0';
        memmove(s, s + 1, len - 1);
    }
}

static int parse_os_info(char *buf, size_t bufsz,
                   char *name, size_t name_sz,
                   char *version_id, size_t version_id_sz,
                   char *build_id, size_t build_id_sz)
{
	struct membuf mb;
	char line[256];
	char *data;
	int len;
	int namefound=0;
	int versionidfound=0;
	int buildidfound=0;

	if(!buf) return 1;

	membuf_init(&mb, buf, bufsz);
	membuf_putraw(&mb, bufsz, true, &data);

	while (len = membuf_readline(&mb, line, sizeof(line) - 1, ' ', false), len) {
		char *tok, *p = line;

		tok = strsep(&p, "=");

		//deckard_log("[%s] [%s]\n",tok,p);

		if (p) {
			if (!strcmp("NAME", tok))       { strncpy(name,p,name_sz); namefound=1; }
			if (!strcmp("VERSION_ID", tok)) { strncpy(version_id,p,version_id_sz); versionidfound=1; }
			if (!strcmp("BUILD_ID", tok))   { strncpy(build_id,p,build_id_sz); buildidfound=1; }
		}
	}

	trim_quotes(name);

	//deckard_log("name=%s\n",name);
	//deckard_log("version_id=%s\n",version_id);
	//deckard_log("build_id=%s\n",build_id);

	if(namefound && versionidfound && buildidfound) 
		return 0;

	return 1;
}

int deckard_bootmenu_set_entry(int * bootmenu_idx, char * str)
{
	char tmpstr[16]; 

	if(!bootmenu_idx) 
		return 1;

	snprintf(tmpstr,16,"bootmenu_%d",*bootmenu_idx);
	env_set(tmpstr, str);
	*bootmenu_idx = *bootmenu_idx + 1;

	return 0;
}
		
int deckard_bootmenu_set_usb_info_env_var(int usb_dev_idx, int maxstrlen, char * vendor_str, char * product_str, char * serialnum_str)
{
	char tmpstr[32]; 
	char usb_bootable_dev_unique_id[256];

	snprintf(tmpstr,32,"usb_boot_vendor_%d",usb_dev_idx);
	env_set(tmpstr, vendor_str);

	snprintf(tmpstr,32,"usb_boot_product_%d",usb_dev_idx);
	env_set(tmpstr, product_str);

	snprintf(tmpstr,32,"usb_boot_serial_%d",usb_dev_idx);
	env_set(tmpstr, serialnum_str);

	return 0;
}

int deckard_set_spl_usb_boot_target()
{
	int ret= 0;
	const char * usbboottarget = env_get("usbboottarget");
	char vendor_env_var_str[32]; 
	char product_env_var_str[32]; 
	char serial_env_var_str[32]; 

	if(usbboottarget)
	{
		int usb_dev_idx = simple_strtoul(usbboottarget, NULL, 10);

		snprintf(vendor_env_var_str,32,"usb_boot_vendor_%d",usb_dev_idx);
		snprintf(product_env_var_str,32,"usb_boot_product_%d",usb_dev_idx);
		snprintf(serial_env_var_str,32,"usb_boot_serial_%d",usb_dev_idx);
		
		splctl_force_usb_set_boot_target( vendor_env_var_str, product_env_var_str, serial_env_var_str );
	}
	
	return 0;
}


int deckard_bootmenu_create_usb_entries(int * bootmenu_entry_idx)
{
	int devcount = -1;
	char vendorstr[64];
	char productstr[64];
	char serialnumstr[64];
	char menu_item_str[256];
	int curr_bootmenu_idx = 0;

	#define BOOTMENUMAXUSBDEVS 4

	if(!bootmenu_entry_idx)
		return -2;

		
	usb_stor_device_get_info(-1, 256, &devcount, NULL, NULL, NULL, NULL);

	if(devcount > BOOTMENUMAXUSBDEVS)
		devcount = BOOTMENUMAXUSBDEVS;

	if(devcount > 0)
	{
		for(int i=0; i<devcount; i++)
		{
			int storage_size_megabytes = 0;
			usb_stor_device_get_info(i, 64, NULL, vendorstr, productstr, serialnumstr, &storage_size_megabytes);
			printf("[%d] %s %s %s\n",i,vendorstr,productstr,serialnumstr);
			snprintf(menu_item_str,256,"USB: %s %s [%d GB]=setenv slottarget usb && setenv usbboottarget %d",vendorstr,productstr,storage_size_megabytes/1024,i);
			deckard_bootmenu_set_entry(bootmenu_entry_idx,menu_item_str);
			deckard_bootmenu_set_usb_info_env_var(i,64,vendorstr,productstr,serialnumstr);
		}
	}
	else
	{
		deckard_bootmenu_set_entry(bootmenu_entry_idx,"Boot from USB=setenv slottarget usb");
	}

	return devcount;
}

int deckard_bootmenu_populate(void)
{
	char current_slot_menu_str[256] = {0};
	char previous_slot_menu_str[256] = {0};
	int usb_boot_mode = 0;

	env_set("factoryreset", "");

	usb_boot_mode = booted_from_usb();

	//bootmenu
	if( usb_boot_mode )
	{
		//usb boot mode
		env_set("bootmenu_title", "SteamOS (USB Boot)");
		env_set("bootmenu_0", "USB Disk=setenv slottarget current"); //this means just continue booting
	}
	else
	{
		deckard_build_slot_menu_items( current_slot_menu_str, sizeof(current_slot_menu_str),
			previous_slot_menu_str,	sizeof(previous_slot_menu_str) );

		//normal boot
		env_set("bootmenu_title", "SteamOS");
		//add new entries and function will increment 
		int bootmenu_entry_idx = 0;
		deckard_bootmenu_set_entry(&bootmenu_entry_idx,current_slot_menu_str);
		deckard_bootmenu_set_entry(&bootmenu_entry_idx,previous_slot_menu_str);
		deckard_bootmenu_create_usb_entries(&bootmenu_entry_idx); 
		deckard_bootmenu_set_entry(&bootmenu_entry_idx,"  -- ERASE USER DATA FROM FRAME --=setenv slottarget current && setenv factoryreset 1");
		if(env_get_ulong("bootmenuadvanced", 10, 0))
		{
			env_set("bootmenuadvanced", ""); //clear var
			deckard_bootmenu_set_entry(&bootmenu_entry_idx,"ADB mode enable=setenv adb-mode 1");
			//put more stuff here as needed
		}
		else
		{
			deckard_bootmenu_set_entry(&bootmenu_entry_idx,"Advanced=setenv bootmenuadvanced 1 && setenv bootmenu_run_again 1");
		}
	}

	// TODO: add advanced option to add fastboot

	env_set("bootmenu_delay", "15");
	env_set("bootdefault", "0");
	
	return 0;
}

static int do_factory_reset(void)
{
	char cfg_file_contents[]="factoryreset\n"; // We can write anything into this file, it just needs to exist
	char cmdstr[256] = {0};
	char addr[19] = {0};
	char *buffer;
	enum disk_interface_t disk_interface = DISK_INTERFACE_SCSI;

	deckard_log("factory reset: creating file in /efi/steamos/factory-reset/factoryreset.cfg\n");
	snprintf(cmdstr,256,"mkdir %s %s /efi", disk_interface_to_str(disk_interface), DECKARD_ESP_PARTITION); run_command(cmdstr,0);
	memset(cmdstr, '\0', sizeof(cmdstr));
	snprintf(cmdstr,256,"mkdir %s %s /efi/steamos", disk_interface_to_str(disk_interface), DECKARD_ESP_PARTITION); run_command(cmdstr,0);
	memset(cmdstr, '\0', sizeof(cmdstr));
	snprintf(cmdstr,256,"mkdir %s %s /efi/steamos/factory-reset", disk_interface_to_str(disk_interface), DECKARD_ESP_PARTITION); run_command(cmdstr,0);
	
	buffer = malloc_cache_aligned(strlen(cfg_file_contents));
	if(!buffer) return -1;
	sprintf(addr, "0x%08lx", (ulong)map_to_sysmem(buffer));
	memcpy(buffer,cfg_file_contents,strlen(cfg_file_contents));
	memset(cmdstr, '\0', sizeof(cmdstr));
	snprintf(cmdstr,256,"save %s %s %s /efi/steamos/factory-reset/factoryreset.cfg %d",
						disk_interface_to_str(disk_interface), DECKARD_ESP_PARTITION, addr, (int)strlen(cfg_file_contents));
	deckard_log("cmdstr=[%s]\n",cmdstr);
	run_command(cmdstr,0);
	deckard_log("factory reset: file create done\n");
	free(buffer);

	return 0;
}


int deckard_bootmenu_handle_actions(void)
{
	deckard_log("deckard_bootmenu_handle_actions\n");

	int reset_needed = 0;
	const char * factoryreset = env_get("factoryreset");
	if ( factoryreset && !strcmp(factoryreset, "1") )
	{
		deckard_log("do_factory_reset\n");
		do_factory_reset();
	}

	const char * slottarget = env_get("slottarget");
	if ( slottarget )
	{
		deckard_log( "slottarget %s\n", slottarget );
		if ( !strcmp( slottarget, "current") )
		{
			deckard_log( "Selected current target. do nothing\n" );
			if( ! booted_from_usb() )
			{
				 //usb stop is called to ensure vbus is not left in a high state before starting the kernel
				deckard_log( "Stopping USB before booting kernel\n" );
				run_command("usb stop",0);
			}
		}
		else if ( !strcmp( slottarget, "previous") )
		{
			char slot = get_booted_slot();
			if( slot == 'A' )
			{
				deckard_log("splctl force-slot B\n");
				run_command("splctl force-slot B",0);
				reset_needed = 1;
			}
			else if(slot == 'B')
			{
				deckard_log("splctl force-slot A\n");
				run_command("splctl force-slot A",0);
				reset_needed = 1;
			}
		}
		else if ( !strcmp( slottarget, "usb") )
		{

			deckard_log("splctl force-usb\n");
			run_command("splctl force-usb",0);
			
			deckard_set_spl_usb_boot_target();

			reset_needed = 1;
		}
	}

	if ( reset_needed )
	{
		printf("resetting...\n");
		run_command("reset",0);
	}

	return 0;
}

static int deckard_build_slot_menu_items(char * CURRENT_slot_menu_str, int CURRENT_slot_menu_str_sz, char * PREVIOUS_slot_menu_str, int PREVIOUS_slot_menu_str_sz)
{
	char cmdstr[256] = {0};
	char * filecontents;
	size_t filesize;
	char CURRENT_slot='X';
	char PREVIOUS_slot='X';
	char name[128] = {0};
	char version_id[128] = {0};
	char build_id[128] = {0};
	enum disk_interface_t disk_interface = DISK_INTERFACE_SCSI;
	int ret;
	int osreleasefilefound;
	char * slot_partition;

	char partition_A[DEVPARTMAXLEN] = { 0 };
	if ( deckard_get_dev_part_str_buf( DECKARD_ROOTFS_SLOT_A, &partition_A[0], sizeof(partition_A ) ) != 0 )
	{
		// TODO: Error
	}

	char partition_B[DEVPARTMAXLEN] = { 0 };
	if ( deckard_get_dev_part_str_buf( DECKARD_ROOTFS_SLOT_B, &partition_B[0], sizeof(partition_B ) ) != 0 )
	{
		// TODO: Error
	}

	if( get_booted_slot() == 'A' ) 
	{
		CURRENT_slot='A';
		PREVIOUS_slot='B';
	}
	else if( get_booted_slot() == 'B' ) 
	{
		CURRENT_slot='B';
		PREVIOUS_slot='A';
	}
	else if( get_booted_slot() == 'U' )
	{
		CURRENT_slot='A';
		PREVIOUS_slot='B';
	}

///////////////////////////////////
	slot_partition = ( CURRENT_slot  == 'B' ) ? partition_B : partition_A; //default to A unless slot is set to B
	osreleasefilefound=0;
	if( file_exists(disk_interface_to_str(disk_interface), slot_partition, "/etc/os-release", FS_TYPE_ANY) )
	{
		ret = run_commandf("load %s %s 0x%x /etc/os-release -1", disk_interface_to_str(disk_interface), slot_partition, CONFIG_SYS_LOAD_ADDR) ;
		if ( ret == 0 ) {
			filecontents = (char *)map_sysmem(CONFIG_SYS_LOAD_ADDR, 0);
			filesize = (size_t) hextoul(env_get("filesize"), NULL);
			ret = parse_os_info(filecontents, filesize, name, sizeof(name), version_id, sizeof(version_id), build_id, sizeof(build_id));
			if( ret == 0) osreleasefilefound = 1;
		}
	}

	if(!osreleasefilefound) 
	{
		printf("No valid os-release file found in A, using defaults\n");
		strncpy(name,"SteamOS",sizeof(name)); 
		strncpy(version_id,"0.0.0",sizeof(version_id));
		strncpy(build_id,"20000101.000000",sizeof(build_id));
	}

	snprintf(CURRENT_slot_menu_str, CURRENT_slot_menu_str_sz, "Current   %s-%c-%s-%s=setenv slottarget current", name, CURRENT_slot, version_id, build_id);
///////////////////////////////////

///////////////////////////////////
	slot_partition = ( PREVIOUS_slot  == 'B' ) ? partition_B : partition_A; //default to A unless slot is set to B
	osreleasefilefound=0;
	if( file_exists(disk_interface_to_str(disk_interface), slot_partition, "/etc/os-release", FS_TYPE_ANY) )
	{
		ret = run_commandf("load %s %s 0x%x /etc/os-release -1", disk_interface_to_str(disk_interface), slot_partition, CONFIG_SYS_LOAD_ADDR) ;
		if ( ret == 0 ) {
			filecontents = (char *)map_sysmem(CONFIG_SYS_LOAD_ADDR, 0);
			filesize = (size_t) hextoul(env_get("filesize"), NULL);
			ret = parse_os_info(filecontents, filesize, name, sizeof(name), version_id, sizeof(version_id), build_id, sizeof(build_id));
			if( ret == 0) osreleasefilefound = 1;
		}
	}
	if(!osreleasefilefound) 
	{
		deckard_log("No valid os-release file found in B, using defaults\n");
		strncpy(name,"SteamOS",sizeof(name)); 
		strncpy(version_id,"0.0.0",sizeof(version_id));
		strncpy(build_id,"20000101.000000",sizeof(build_id));
	}

	snprintf(PREVIOUS_slot_menu_str, PREVIOUS_slot_menu_str_sz, "Previous  %s-%c-%s-%s=setenv slottarget previous", name, PREVIOUS_slot, version_id, build_id);
///////////////////////////////////

	return 0;
}
