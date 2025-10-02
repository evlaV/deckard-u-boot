// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Linaro 2025
 */

/*
 * Boot support
 */
#include <command.h>

enum pstore_type_id {
	PSTORE_TYPE_DMESG	= 0,
	PSTORE_TYPE_MCE		= 1,
	PSTORE_TYPE_CONSOLE	= 2,
	PSTORE_TYPE_FTRACE	= 3,
	PSTORE_TYPE_PPC_RTAS	= 4,
	PSTORE_TYPE_PPC_OF	= 5,
	PSTORE_TYPE_PPC_COMMON	= 6,
	PSTORE_TYPE_PMSG	= 7,
	PSTORE_TYPE_PPC_OPAL	= 8,
	PSTORE_TYPE_UNKNOWN	= 255
};

extern void console_ramoops_records_get(enum pstore_type_id);

static int do_ramoops(struct cmd_tbl *cmd, int flag, int argc,
		      char *const argv[])
{
	enum pstore_type_id type = PSTORE_TYPE_CONSOLE;

	if (argc == 2 && !strcmp(argv[1], "dmesg"))
		type = PSTORE_TYPE_DMESG;

	console_ramoops_records_get(type);

	return 0;
}

U_BOOT_CMD(
	ramoops,	2,	1,	do_ramoops,
	"print console or dmesg saved in ramoops area",
	"console        - dump console record (default)\n"
	"ramoops dmesg          - dump saved dmesg\n"
);
