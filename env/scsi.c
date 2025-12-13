// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on SCSI implmentation.  Env file
 * saved on raw partition, not mountable ( eg ext4 )
*/

#include <part.h>

#include <command.h>
#include <env.h>
#include <env_internal.h>
#include <linux/stddef.h>
#include <malloc.h>
#include <memalign.h>
#include <search.h>
#include <errno.h>
#include <scsi.h>
#include <virtio.h>
#include <asm/global_data.h>
#include <u-boot/crc.h>
DECLARE_GLOBAL_DATA_PTR;

struct hsearch_data env_local_htab = {
	.change_ok = env_flags_validate,
};
__weak const char *env_scsi_get_dev_part(void)
{
	return (const char *)CONFIG_ENV_SCSI_DEVICE_AND_PART;
}


int env_local_do_env_set(int flag, int argc, char *const argv[], int env_flag)
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


	/* Delete only ? */
	if (argc < 3 || argv[2] == NULL) {
		int rc = hdelete_r(name, &env_local_htab, env_flag);

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
	hsearch_r(e, ENV_ENTER, &ep, &env_local_htab, env_flag);
	free(value);
	if (!ep) {
		printf("## Error inserting \"%s\" variable, errno=%d\n",
			name, errno);
		return 1;
	}

	return 0;
}

int env_local_set(const char *varname, const char *varvalue)
{
	const char * const argv[4] = { "setenv", varname, varvalue, NULL };

	if (varvalue == NULL || varvalue[0] == '\0')
		return env_local_do_env_set(0, 2, (char * const *)argv, H_PROGRAMMATIC);
	else
		return env_local_do_env_set(0, 3, (char * const *)argv, H_PROGRAMMATIC);
}

static int env_local_print(char *name, int flag)
{
	char *res = NULL;
	ssize_t len;

	if (name) {		/* print a single name */
		struct env_entry e, *ep;

		e.key = name;
		e.data = NULL;
		hsearch_r(e, ENV_FIND, &ep, &env_local_htab, flag);
		if (ep == NULL)
			return 0;
		len = printf("%s=%s\n", ep->key, ep->data);
		return len;
	}

	/* print whole list */
	len = hexport_r(&env_local_htab, '\n', flag, &res, 0, 0, NULL);

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
int env_local_import(const char *buf, int check, int flags)
{
	env_t *ep = (env_t *)buf;

	if (check) {
		uint32_t crc;

		memcpy(&crc, &ep->crc, sizeof(crc));

		if (crc32(0, ep->data, ENV_SIZE) != crc) {
			return -ENOMSG; /* needed for env_load() */
		}
	}

	if (himport_r(&env_local_htab, (char *)ep->data, ENV_SIZE, '\0', flags, 0, 0, NULL)) {
		return 0;
	}

	pr_err("Cannot import environment: errno = %d\n", errno);

	return -EIO;
}

/* Export the environment and generate CRC for it. */
int env_local_export(env_t *env_out)
{
	char *res;
	ssize_t	len;

	res = (char *)env_out->data;
	len = hexport_r(&env_local_htab, '\0', 0, &res, ENV_SIZE, 0, NULL);
	if (len < 0) {
		pr_err("Cannot export environment: errno = %d\n", errno);
		return 1;
	}

	env_out->crc = crc32(0, env_out->data, ENV_SIZE);

	return 0;
}

static int env_scsi_save_buffer(env_t *env_new)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, envbuf, CONFIG_ENV_SIZE); //used for temp read back
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int ret, part;
	const char *dev_and_part = env_scsi_get_dev_part();

	part = blk_get_device_part_str("scsi", dev_and_part,
					&blk_dev, &info, 1);
	if (part < 0)
		return 1;

	/* NOTE: blk_write() size is in blocks ( 4k ) */
	// TODO Should we check info.size against CONFIG_ENV_SIZE
	ret = blk_write(blk_dev->bdev, info.start, info.size, env_new);
	if (!ret)
		printf("Unable to write env\n");


	//TODO: should read back buffer to ensure it is written / flushed to disk!!
	//int blkcnt = blk_read(blk_dev->bdev, info.start, info.size, envbuf);
	//if (blkcnt != info.size)
	//	printf("Unable to read back env after writing\n");

	return 0;
}

static int env_local_scsi_save(void)
{
	env_t env_new;
	int err;

	err = env_local_export(&env_new);
	if (err)
		return err;

	err = env_scsi_save_buffer(&env_new);
	if (err)
		return err;

	gd->env_valid = ENV_VALID;
	puts("done\n");

	return 0;
}

static int env_scsi_save(void)
{
	env_t env_new;
	int err;

	err = env_export(&env_new);
	if (err)
		return err;

	err = env_scsi_save_buffer(&env_new);
	if (err)
		return err;

	gd->env_valid = ENV_VALID;
	puts("done\n");

	return 0;
}

static int env_scsi_erase(void)
{
	env_t env_new;
	int err;

	memset(&env_new, 0, sizeof(env_t));

	err = env_scsi_save_buffer(&env_new);
	if (err)
		return err;

	gd->env_valid = ENV_INVALID;
	puts("done\n");

	return 0;
}

static int env_local_scsi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, envbuf, CONFIG_ENV_SIZE);
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int err, part;
	ulong count;
	const char *dev_and_part;

	scsi_scan(false);

	dev_and_part = env_scsi_get_dev_part();
		
	part = blk_get_device_part_str("scsi", dev_and_part, &blk_dev, &info, 1);
	if (part < 0) {
		printf("error reading from %s\n", dev_and_part);
		return 1;
	}

	count = blk_read(blk_dev->bdev, info.start, CONFIG_ENV_SIZE < info.size * info.blksz ? CONFIG_ENV_SIZE / info.blksz : info.size, envbuf);
	if (count < 0)
		panic("Failed to read env partition (%ld)\n", count);

	err = env_local_import(envbuf, 1, H_EXTERNAL);
	if (!err)
		gd->env_valid = ENV_VALID;
		
	return err;
}

static int env_local_save_env_var(const char *varname, const char *varvalue)
{
	int ret = 0;

	ret = env_local_scsi_load();

	if(ret)	goto fail;

	ret = env_local_set(varname,varvalue);
	if(ret)	goto fail;
	ret = env_scsi_save();
	if(ret)	goto fail;

	//no error, don't print anything
	return ret;

fail:
	printf("err %d\n",ret);
	return ret;
}


static int env_scsi_load(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(char, buf, CONFIG_ENV_SIZE);
	struct blk_desc *blk_dev = NULL;
	struct disk_partition info;
	int err, part;
	ulong count;
	char *default_bootargs;
	const char *dev_and_part;

	scsi_scan(false);

	dev_and_part = env_scsi_get_dev_part();

	part = blk_get_device_part_str("scsi", dev_and_part,
						&blk_dev, &info, 1);
	if (part < 0)
		goto err_env_relocate;

	count = blk_read(blk_dev->bdev, info.start, CONFIG_ENV_SIZE < info.size * info.blksz ? CONFIG_ENV_SIZE / info.blksz : info.size, buf);
	if (count < 0)
		panic("Failed to read env partition (%ld)\n", count);

	err = env_import(buf, 1, H_EXTERNAL);
	if (!err)
		gd->env_valid = ENV_VALID;

	return err;

err_env_relocate:
	env_set_default(NULL, 0);

	return -EIO;
}

U_BOOT_ENV_LOCATION(scsi) = {
	.location	= ENVL_SCSI,
	ENV_NAME("SCSI")
	.load		= env_scsi_load,
	.save		= ENV_SAVE_PTR(env_scsi_save),
	//add save_env_var or something similar
	.erase		= ENV_ERASE_PTR(env_scsi_erase),
};
