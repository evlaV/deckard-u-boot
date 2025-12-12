#include "deckard.h"
#include "deckard_bootmenu.h"

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

static int deckard_load_slot_info(char * current_slot_menu_str, int current_slot_menu_str_sz, char * previous_slot_menu_str, int previous_slot_menu_str_sz);

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
	char line[200];
	char *data;
	int len;

	//init to some sane values
	strncpy(name,"SteamOS",name_sz); 
	strncpy(version_id,"0.0.0",version_id_sz);
	strncpy(build_id,"20000101.000000",build_id_sz);

	membuf_init(&mb, buf, bufsz);
	membuf_putraw(&mb, bufsz, true, &data);

	while (len = membuf_readline(&mb, line, sizeof(line) - 1, ' ', false), len) {
		char *tok, *p = line;

		tok = strsep(&p, "=");

		//printf("[%s] [%s]\n",tok,p);

		if (p) {
			if (!strcmp("NAME", tok))       strncpy(name,p,name_sz);
			if (!strcmp("VERSION_ID", tok)) strncpy(version_id,p,version_id_sz);
			if (!strcmp("BUILD_ID", tok))   strncpy(build_id,p,build_id_sz);
		}
	}

	trim_quotes(name);

	//printf("name=%s\n",name);
	//printf("version_id=%s\n",version_id);
	//printf("build_id=%s\n",build_id);

	return 0;
}

int deckard_setup_bootmenu(void)
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
		env_set("bootmenu_title", "SteamOS : USB Boot");
		env_set("bootmenu_0", "USB Disk=setenv slottarget current"); //this means just continue booting
	}
	else
	{
		deckard_load_slot_info(current_slot_menu_str,256,previous_slot_menu_str,256);
		//normal boot
		env_set("bootmenu_title", "SteamOS");
		env_set("bootmenu_0", current_slot_menu_str);
		env_set("bootmenu_1", previous_slot_menu_str);
		env_set("bootmenu_2", "Boot from USB=setenv slottarget usb");
		env_set("bootmenu_3", "  -- ERASE USER DATA FROM FRAME --=setenv slottarget current && setenv factoryreset 1");
	}
	env_set("bootmenu_delay", "15");
	env_set("bootdefault", "0");
	
	return 0;
}

static int enable_factory_reset_if_needed(void)
{
	char cfg_file_contents[]="factorreset\n";
	char cmdstr[256] = {0};
	char addr[19] = {0};
	char *buffer;
	enum disk_interface_t disk_interface = DISK_INTERFACE_SCSI;

	if (env_get("factoryreset") && !strcmp(env_get("factoryreset"), "1"))
	{
		printf("factory reset: creating file in /efi/steamos/factory-reset/factoryreset.cfg\n");
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
		printf("cmdstr=[%s]\n",cmdstr);
		run_command(cmdstr,0);
		printf("factory reset: file create done\n");
		free(buffer);
	}

	return 0;
}


int deckard_boot_menu_handle_actions(void)
{
	int bootother=0;
	int bootusb=0;
	int reset_needed = 0;

	enable_factory_reset_if_needed();

	if (env_get("slottarget") && !strcmp(env_get("slottarget"), "current"))
	{
		bootother=0;
	}

	if (env_get("slottarget") && !strcmp(env_get("slottarget"), "previous"))
	{
		bootother=1;
	}

	if (env_get("slottarget") && !strcmp(env_get("slottarget"), "usb"))
	{
		bootusb=1;
	}

	if(bootother)
	{
		if(get_booted_slot() == 'A')
		{
			run_command("splctl force-slot B",0);
			reset_needed = 1;
		}
		
		if(get_booted_slot() == 'B')
		{
			run_command("splctl force-slot A",0);
			reset_needed = 1;
		}
	}

	if(bootusb)
	{
		run_command("splctl force-usb",0);
		reset_needed = 1;
	}
	
	//printf("----------------\n");
	//printf("bootedslot=%c bootedslot=%s\n",bootedslot_char,bootedslot);
	//printf("bootother=%d\n",bootother);
	//printf("bootusb=%d\n",bootusb);
	//printf("----------------\n");

	if(reset_needed)
	{
		printf("resetting...\n");
		run_command("reset",0);
	}

	return 0;
}


static int deckard_load_slot_info(char * current_slot_menu_str, int current_slot_menu_str_sz, char * previous_slot_menu_str, int previous_slot_menu_str_sz)
{
	char cmdstr[256] = {0};
	char * partition_a;
	char * partition_b;
	char * filecontents;
	size_t filesize;
	char curr_slot='X';
	char prev_slot='X';
	char name[128] = {0};
	char version_id[128] = {0};
	char build_id[128] = {0};
	enum disk_interface_t disk_interface = DISK_INTERFACE_SCSI;

	partition_a = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_A);
	partition_b = deckard_get_dev_part_str(DECKARD_ROOTFS_SLOT_B);

	if( get_booted_slot() == 'A' ) 
	{
		curr_slot='A';
		prev_slot='B';
	}
	else if( get_booted_slot() == 'B' ) 
	{
		curr_slot='B';
		prev_slot='A';
	}
	else if( get_booted_slot() == 'U' )
	{
		curr_slot='A';
		prev_slot='B';
	}

///////////////////////////////////
	snprintf(cmdstr,256,"load %s %s 0x%x /etc/os-release -1",disk_interface_to_str(disk_interface),partition_a,CONFIG_SYS_LOAD_ADDR);
	if ( run_command(cmdstr,0) == 0 ) {
		filecontents = (char *)map_sysmem(CONFIG_SYS_LOAD_ADDR, 0);
		filesize = (size_t) hextoul(env_get("filesize"), NULL);
		parse_os_info(filecontents, filesize, name, sizeof(name), version_id,
				sizeof(version_id), build_id, sizeof(build_id));

		snprintf(current_slot_menu_str,current_slot_menu_str_sz,
			"Current   %s-%c-%s-%s=setenv slottarget current", name,curr_slot,version_id,build_id);
	} else {
		printf("No os-release file found in a\n");
	}
///////////////////////////////////

///////////////////////////////////
	memset(cmdstr, '\0', sizeof(cmdstr));
	snprintf(cmdstr,256,"load %s %s 0x%x /etc/os-release -1",disk_interface_to_str(disk_interface),partition_b,CONFIG_SYS_LOAD_ADDR);
	if ( run_command(cmdstr,0) == 0 ) {
		filecontents = (char *)map_sysmem(CONFIG_SYS_LOAD_ADDR, 0);
		filesize = hextoul(env_get("filesize"), NULL);
		parse_os_info(filecontents, filesize, name, sizeof(name), version_id,
				sizeof(version_id), build_id, sizeof(build_id));

		snprintf(previous_slot_menu_str,previous_slot_menu_str_sz,
			"Previous  %s-%c-%s-%s=setenv slottarget previous", name,prev_slot,version_id,build_id);
	} else {
		printf("No os-release file found in b\n");
	}
///////////////////////////////////

	return 0;
}
