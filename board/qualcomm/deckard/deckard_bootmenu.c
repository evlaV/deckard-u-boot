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


static int deckard_build_slot_menu_items(char * current_slot_menu_str, int current_slot_menu_str_sz, char * previous_slot_menu_str, int previous_slot_menu_str_sz);

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
		env_set("bootmenu_0", current_slot_menu_str);
		env_set("bootmenu_1", previous_slot_menu_str);
		env_set("bootmenu_2", "Boot from USB=setenv slottarget usb");
		env_set("bootmenu_3", "  -- ERASE USER DATA FROM FRAME --=setenv slottarget current && setenv factoryreset 1");
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
	osreleasefilefound=0;
	if( file_exists(disk_interface_to_str(disk_interface), partition_A, "/etc/os-release", FS_TYPE_ANY) )
	{
		ret = run_commandf("load %s %s 0x%x /etc/os-release -1", disk_interface_to_str(disk_interface), partition_A, CONFIG_SYS_LOAD_ADDR) ;
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
	osreleasefilefound=0;
	if( file_exists(disk_interface_to_str(disk_interface), partition_B, "/etc/os-release", FS_TYPE_ANY) )
	{
		ret = run_commandf("load %s %s 0x%x /etc/os-release -1", disk_interface_to_str(disk_interface), partition_B, CONFIG_SYS_LOAD_ADDR) ;
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
