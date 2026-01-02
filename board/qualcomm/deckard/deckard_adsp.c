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

#define DECKARD_ADSP_ID 0

#define QDSP6V67SS_BASE		0x06800000
#define QDSP6V67SS_BOOT_PARAMS0 (QDSP6V67SS_BASE + 0x60)
#define DECKARD_ADSP_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 4)
#define DECKARD_ADSP_IN_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 8)

int deckard_adsp_start( void )
{
	deckard_log("Starting ADSP...\n");

	int id = DECKARD_ADSP_ID;
	int ret;

	if (!rproc_is_initialized()) {
		ret = rproc_init();
		if (ret < 0) {
			deckard_log("ERROR: rproc_init failed (%d)\n", ret );
			return ret;
		}
	}

	ret = rproc_start_with_param(id,0);
	if (ret < 0) {
		deckard_log("ERROR: rproc_start_with_param failed(%d)\n", ret );
		return ret;
	}

	deckard_log("Started ADSP\n");
	return 0;
}

int deckard_adsp_stop( void )
{
	deckard_log("Stopping ADSP...\n");
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

void deckard_adsp_write( ADSP_PayloadIn_t * payload )
{
	if ( !payload )
		return;
	payload->bits.magic = DECKARD_ADSP_IN_MAGIC;
	payload->bits.rsvd = 0;
	writel( payload->u32, DECKARD_ADSP_IN_ADDR );
}

int deckard_adsp_read( ADSP_PayloadOut_t * payload )
{
	u32 cur = readl(DECKARD_ADSP_OUT_ADDR);
	if ( payload )
	{
		payload->u32 = cur;
	}

	return ( payload->bits.magic == DECKARD_ADSP_OUT_MAGIC ) ? 0 : -1;
}
