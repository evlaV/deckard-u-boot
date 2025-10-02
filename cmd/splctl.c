// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025
  * Valve Software
 */

#include <command.h>
#include <cmd_spl.h>
#include <env.h>
#include <env_internal.h>
#include <image.h>
#include <log.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>
#include <search.h>
#include <memalign.h>
#include <scsi.h>
#include <part.h>
#include <u-boot/crc.h>

#define DECKARD_SPL_ENV_PARTITION "2:1"
#define DECKARD_SPL_ENV_BACKUP_PARTITION "2:2"
#define DECKARD_SPL_CONFIG_ENV_SIZE 0x1f000
#define DECKARD_SPL_ENV_SIZE (DECKARD_SPL_CONFIG_ENV_SIZE - ENV_HEADER_SIZE)

DECLARE_GLOBAL_DATA_PTR;

struct hsearch_data env_spl_htab = {
	.change_ok = env_flags_validate,
};

static int env_id = 1;

int env_spl_get_id(void)
{
	return env_id;
}

void env_spl_inc_id(void)
{
	env_id++;
}

int env_spl_do_env_set(int flag, int argc, char *const argv[], int env_flag)
{
	int   i, len;
	char  *name, *value, *s;
	struct env_entry e, *ep;

	debug("Initial value for argc=%d\n", argc);

	while (argc > 1 && **(argv + 1) == '-') {
		char *arg = *++argv;

		--argc;
		while (*++arg) {
			switch (*arg) {
			case 'f':		/* force */
				env_flag |= H_FORCE;
				break;
			default:
				return CMD_RET_USAGE;
			}
		}
	}
	debug("Final value for argc=%d\n", argc);
	name = argv[1];

	if (strchr(name, '=')) {
		printf("## Error: illegal character '=' " "in variable name \"%s\"\n", name);
		return 1;
	}

	env_spl_inc_id();

	/* Delete only ? */
	if (argc < 3 || argv[2] == NULL) {
		int rc = hdelete_r(name, &env_spl_htab, env_flag);

		/* If the variable didn't exist, don't report an error */
		return rc && rc != -ENOENT ? 1 : 0;
	}

	/*
	 * Insert / replace new value
	 */
	for (i = 2, len = 0; i < argc; ++i)
		len += strlen(argv[i]) + 1;

	value = malloc(len);
	if (value == NULL) {
		printf("## Can't malloc %d bytes\n", len);
		return 1;
	}
	for (i = 2, s = value; i < argc; ++i) {
		char *v = argv[i];

		while ((*s++ = *v++) != '\0')
			;
		*(s - 1) = ' ';
	}
	if (s != value)
		*--s = '\0';

	e.key	= name;
	e.data	= value;
	hsearch_r(e, ENV_ENTER, &ep, &env_spl_htab, env_flag);
	free(value);
	if (!ep) {
		printf("## Error inserting \"%s\" variable, errno=%d\n",
			name, errno);
		return 1;
	}

	return 0;
}

int env_spl_set(const char *varname, const char *varvalue)
{
	const char * const argv[4] = { "setenv", varname, varvalue, NULL };

	if (varvalue == NULL || varvalue[0] == '\0')
		return env_spl_do_env_set(0, 2, (char * const *)argv, H_PROGRAMMATIC);
	else
		return env_spl_do_env_set(0, 3, (char * const *)argv, H_PROGRAMMATIC);
}


static int env_spl_print(char *name, int flag)
{
	char *res = NULL;
	ssize_t len;

	if (name) {		/* print a single name */
		struct env_entry e, *ep;

		e.key = name;
		e.data = NULL;
		hsearch_r(e, ENV_FIND, &ep, &env_spl_htab, flag);
		if (ep == NULL)
			return 0;
		len = printf("%s=%s\n", ep->key, ep->data);
		return len;
	}

	/* print whole list */
	len = hexport_r(&env_spl_htab, '\n', flag, &res, 0, 0, NULL);

	if (len > 0) {
		puts(res);
		free(res);
		return len;
	}

	/* should never happen */
	printf("## Error: cannot export environment\n");
	return 0;
}

/*
 * Check if CRC is valid and (if yes) import the environment.
 * Note that "buf" may or may not be aligned.
 */
int env_spl_import(const char *buf, int check, int flags)
{
	env_t *ep = (env_t *)buf;

	if (check) {
		uint32_t crc;

		memcpy(&crc, &ep->crc, sizeof(crc));

		if (crc32(0, ep->data, DECKARD_SPL_ENV_SIZE) != crc) {
			return -ENOMSG; /* needed for env_load() */
		}
	}

	if (himport_r(&env_spl_htab, (char *)ep->data, DECKARD_SPL_ENV_SIZE, '\0', flags, 0, 0, NULL)) {
		return 0;
	}

	pr_err("Cannot import environment: errno = %d\n", errno);

	return -EIO;
}

static int env_spl_scsi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, envbuf, DECKARD_SPL_CONFIG_ENV_SIZE);
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int err, part;
	ulong count;
	const char *dev_and_part = DECKARD_SPL_ENV_PARTITION;

	//scsi_scan(true);

	part = blk_get_device_part_str("scsi", dev_and_part, &blk_dev, &info, 1);
	if (part < 0) {
		printf("error reading from %s\n", dev_and_part);
		return 1;
	}

	//printf("Loading env from %s, %s, %s, %ld\n", dev_and_part, info.name, info.type, info.size);

	count = blk_read(blk_dev->bdev, info.start, DECKARD_SPL_CONFIG_ENV_SIZE / info.blksz, envbuf);

	//printf("blk_read: read %ld, block size = %ld\n",count,info.blksz);

	err = env_spl_import(envbuf, 1, H_EXTERNAL);

	return err;
}


/* Export the environment and generate CRC for it. */
int env_spl_export(env_t *env_out)
{
	char *res;
	ssize_t	len;

	res = (char *)env_out->data;
	len = hexport_r(&env_spl_htab, '\0', 0, &res, ENV_SIZE, 0, NULL);
	if (len < 0) {
		pr_err("Cannot export environment: errno = %d\n", errno);
		return 1;
	}

	env_out->crc = crc32(0, env_out->data, ENV_SIZE);

	return 0;
}

static int env_spl_scsi_save_buffer(env_t *env_new)
{
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int ret, part;
	const char *dev_and_part = DECKARD_SPL_ENV_PARTITION;

	part = blk_get_device_part_str("scsi", dev_and_part,
					&blk_dev, &info, 1);
	if (part < 0)
		return 1;

	/* NOTE: blk_write() size is in blocks ( 4k ) */
	// TODO Should we check info.size against CONFIG_ENV_SIZE
	ret = blk_write(blk_dev->bdev, info.start, info.size, env_new);
	if (!ret)
		printf("Unable to write env\n");

	ret = env_spl_scsi_load();
	if (ret != 0)
		printf("Unable to read back env after writing\n");

	return 0;
}


static int env_scsi_save(void)
{
	env_t env_new;
	int err;

	err = env_spl_export(&env_new);
	if (err)
		return err;

	err = env_spl_scsi_save_buffer(&env_new);
	if (err)
		return err;

	puts("done\n");

	return 0;
}

static int do_set_uboot_poweroff_reason(struct cmd_tbl *cmdtp, int flag, int argc,
			      char *const argv[])
{
	int ret = 0;
	char * reason = NULL;

	if (argc < 2) /* no subcommand */
		return cmd_usage(cmdtp);

	reason = argv[1]; //battery or ?

	ret = env_spl_scsi_load();

	if(ret)	goto fail;

	ret = env_spl_set("UBOOT_POWEROFF_REASON",reason);
	if(ret)	goto fail;
	ret = env_scsi_save();
	if(ret)	goto fail;

	//no error, don't print anything
	return ret;

fail:
	printf("err %d\n",ret);
	return ret;
}

static int do_set_bootfw_slot(struct cmd_tbl *cmdtp, int flag, int argc,
			      char *const argv[])
{
	int ret = 0;
	char * target = NULL;

	if (argc < 2) /* no subcommand */
		return cmd_usage(cmdtp);

	target = argv[1]; // A or B
	
	ret = env_spl_scsi_load();

	if(ret)	goto fail;

	if(!strcmp(target,"A"))
	{
		ret = env_spl_set("BOOT_FW_SLOT","A");
		if(ret)	goto fail;
		ret = env_scsi_save();
		if(ret)	goto fail;
	}
	else if(!strcmp(target,"B"))
	{
		ret = env_spl_set("BOOT_FW_SLOT","B");
		if(ret)	goto fail;
		ret = env_scsi_save();
		if(ret)	goto fail;
	}
	else
	{
		printf("invalid target\n");
		return 0;
	} 

	//no error, don't print anything
	return ret;

fail:
	printf("err %d\n",ret);
	return ret;
}

static int do_set_primary(struct cmd_tbl *cmdtp, int flag, int argc,
			      char *const argv[])
{
	int ret = 0;
	char * target = NULL;

	if (argc < 2) /* no subcommand */
		return cmd_usage(cmdtp);

	target = argv[1]; // A or B
	
	ret = env_spl_scsi_load();

	if(ret)	goto fail;

	if(!strcmp(target,"A"))
	{
		ret = env_spl_set("BOOT_STATE_A","1");
		if(ret)	goto fail;
		ret = env_spl_set("BOOT_ORDER","A B");
		if(ret)	goto fail;
		ret = env_scsi_save();
		if(ret)	goto fail;
	}
	else if(!strcmp(target,"B"))
	{
		ret = env_spl_set("BOOT_STATE_B","1");
		if(ret)	goto fail;
		ret = env_spl_set("BOOT_ORDER","B A");
		if(ret)	goto fail;
		ret = env_scsi_save();
		if(ret)	goto fail;
	}
	else
	{
		printf("invalid target\n");
		return 0;
	} 

	//no error, don't print anything
	return ret;

fail:
	printf("err %d\n",ret);
	return ret;
}

static int do_force_usb(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	int ret = 0;
	ret = env_spl_scsi_load();
	if(ret)	goto fail;
	ret = env_spl_set("FORCE_USB","1");
	if(ret)	goto fail;
	ret = env_scsi_save();
	if(ret)	goto fail;

	//no error, don't print anything
	return ret;

fail:
	printf("err %d\n",ret);
	return ret;
}

static int do_print_spl_env(struct cmd_tbl *cmdtp, int flag, int argc,
			      char *const argv[])
{
	int ret = 0;
	ret = env_spl_scsi_load();
	if( ret == 0 ) 
		env_spl_print(NULL,0);
	else 
		printf("error\n");
	return ret;
}

static struct cmd_tbl cmd_splctl_sub[] = {
	U_BOOT_CMD_MKENT(set-primary, 2, 1, do_set_primary, "", ""),
	U_BOOT_CMD_MKENT(set-bootfw-slot, 2, 1, do_set_bootfw_slot, "", ""),
	U_BOOT_CMD_MKENT(force-usb, 1, 1, do_force_usb, "", ""),
	U_BOOT_CMD_MKENT(env, 1, 1, do_print_spl_env, "", ""),
	U_BOOT_CMD_MKENT(set-uboot-poweroff-reason, 2, 1, do_set_uboot_poweroff_reason, "", ""),
};

static int do_splctl(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct cmd_tbl *cp;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* drop initial arg */
	argc--;
	argv++;

	cp = find_cmd_tbl(argv[0], cmd_splctl_sub, ARRAY_SIZE(cmd_splctl_sub));
	if (cp)
		return cp->cmd(cmdtp, flag, argc, argv);

	return CMD_RET_USAGE;
}

U_BOOT_LONGHELP(splctl,
	"set-primary [A/B] - SPL set primary partition\n"
	"       force-usb    - SPL force booting from usb\n"
	"       env - SPL print env\n"
	);

#define SPLCTL_CMD_MAXARGS 3
U_BOOT_CMD(splctl, SPLCTL_CMD_MAXARGS, 1, do_splctl,
	   "splctl", splctl_help_text
);
