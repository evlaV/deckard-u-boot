// SPDX-License-Identifier: GPL-2.0+
/* Some example code to get and transfer one byte for adsp */

#include <asm/io.h>
#include <command.h>
#include <dm.h>
#include <linux/types.h>
#include <remoteproc.h>
#include <time.h>

#include "deckard_adsp.h"
#include "deckard_log.h"

#define QDSP6SS_BASE		0x06800000
#define QDSP6SS_BOOT_PARAMS0 	0x60

#define DECKARD_ADSP_TIMEOUT	3000

#define DECKARD_ADSP_CLEAN_PATTERN 0xffffffff

#define DECKARD_ADSP_TX_ADDR	(QDSP6SS_BASE + QDSP6SS_BOOT_PARAMS0 + 4)
#define DECKARD_ADSP_RX_ADDR	(QDSP6SS_BASE + QDSP6SS_BOOT_PARAMS0 + 8)

#define DECKARD_ADSP_ID 0


int deckard_adsp_start( void )
{
	int id = DECKARD_ADSP_ID;
	int ret;

	if (!rproc_is_initialized()) {
		ret = rproc_init();
		if (ret < 0) {
			deckard_log("failed to start ADSP rproc_init (%d)\n", ret );
			return ret;
		}
	}

	ret = rproc_start_with_param(id,0);
	if (ret < 0) {
		deckard_log("failed to start ADSP rproc_start_with_param (%d)\n", ret );
		return ret;
	}

	deckard_log("started ADSP\n");
	return 0;
}

int deckard_adsp_stop( void )
{
	int id = DECKARD_ADSP_ID;

	int ret = rproc_stop( id );
	if ( ret != 0 )
	{
		deckard_log("failed to stop ADSP rproc_stop (%d)\n", ret );
	}
	else
	{
		deckard_log("stopped ADSP\n" );
	}
	return ret;
}

static int deckard_adsp_tx(u32 val)
{
	ulong timeout;
	int ret;

	if (readl(DECKARD_ADSP_TX_ADDR) != DECKARD_ADSP_CLEAN_PATTERN)
		return -ENODEV;

	writel(val, DECKARD_ADSP_TX_ADDR);

	timeout = get_timer(0) + DECKARD_ADSP_TIMEOUT;

	while(get_timer(0) < timeout) {
		if (readl(DECKARD_ADSP_TX_ADDR) == DECKARD_ADSP_CLEAN_PATTERN)
			return 0;
	}

	return -ETIMEDOUT;
}

static int deckard_adsp_rx(u32 *val)
{
	ulong timeout;
	u32 cur;
	int ret;

	timeout = get_timer(0) + DECKARD_ADSP_TIMEOUT;

	while(get_timer(0) < timeout) {
		cur = readl(DECKARD_ADSP_RX_ADDR);
		if (cur == DECKARD_ADSP_CLEAN_PATTERN)
			continue;
		*val = cur;
		return 0;
	}

	return -ETIMEDOUT;
}

static int do_adsp(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct cmd_tbl *cp;
	u32 val = 0;
	int ret;

	if (argc < 1)
		return CMD_RET_USAGE;

	/* drop initial arg */
	argc--;
	argv++;

	if (!strncmp(argv[0], "tx", 2)) {
		val = simple_strtoul(argv[1], NULL, 10);

		printf("APPS PING: %d\n", val);
		ret = deckard_adsp_tx(val);
		if (ret < 0)
			return ret;
	} else if (!strncmp(argv[0], "rx", 2)) {
		ret = deckard_adsp_rx(&val);
		if (ret < 0)
			return ret;

		printf("ADSP PONG: %d\n", val);
	}

	return 0;
}

U_BOOT_LONGHELP(adsp,
		"tx [value] - send u32 value to ADSP and block until read on the other side\n"
		"rx - try to read until timeout from the ADSP\n");

U_BOOT_CMD(adsp, 3, 1, do_adsp, "adsp", adsp_help_text);
