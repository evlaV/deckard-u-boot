// SPDX-License-Identifier: GPL-2.0+
/* Some example code to get and transfer one byte for adsp */

#include <asm/io.h>
#include <command.h>
#include <dm.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <remoteproc.h>
#include <time.h>

#include "deckard_adsp.h"
#include "deckard_log.h"

// uboot-only
#define QDSP6V67SS_BASE		0x06800000

/*
	This is our bidirectional communications mechanism between uboot and adsp.
*/
#pragma pack(push, 1)

typedef union UBOOT_PayloadOut_t {
	uint32_t u32;
	struct {
		uint32_t magic  : 2; // DECKARD_UBOOT_OUT_MAGIC
		uint32_t dsp_exit_request : 1;
		uint32_t btn_power : 1;
		uint32_t rsvd   : 28; // sender should clear if unused
	} bits;
} UBOOT_PayloadOut_t;

typedef union ADSP_PayloadOut_t {
    uint32_t u32;
    struct {
        uint32_t magic  : 2;  // DECKARD_ADSP_OUT_MAGIC
        uint32_t sequence_count : 4; // sequence counter.  increments on each update
		uint32_t run_state : 2; // ADSPLite_RunState_t
        uint32_t rsvd   : 24; // sender should clear if unused
    } bits;
} ADSP_PayloadOut_t;

static_assert( sizeof( ADSP_PayloadOut_t ) == 4 );
static_assert( sizeof( UBOOT_PayloadOut_t ) == 4 );

#pragma pack(pop)

#define QDSP6V67SS_BOOT_PARAMS0 (QDSP6V67SS_BASE + 0x60)
#define DECKARD_ADSP_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 4)
#define DECKARD_UBOOT_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 8)
#define DECKARD_UBOOT_OUT_MAGIC 0b10
#define DECKARD_ADSP_OUT_MAGIC 0b01

#define DECKARD_ADSP_ID 0


int deckard_adsp_start( void )
{
	deckard_log("Starting ADSP...\n");

	// Before starting, invalidate the prior output data the ADSP writes
	// This assures that subsequent reads to deckard_adsp_read
	// wont return stale data
	writel( 0, DECKARD_ADSP_OUT_ADDR );

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

void deckard_adsp_write( bool dsp_exit_request,
	bool btn_power )
{
	UBOOT_PayloadOut_t payload = { 0 };
	payload.bits.magic = DECKARD_UBOOT_OUT_MAGIC; 
	payload.bits.dsp_exit_request = dsp_exit_request ? 1 : 0;
	payload.bits.btn_power = btn_power ? 1 : 0;
	writel( payload.u32, DECKARD_UBOOT_OUT_ADDR );
}

uint32_t deckard_adsp_read( ADSPLite_RunState_t * runstate, int * sequence_count )
{
	ADSP_PayloadOut_t payload;
	payload.u32 = readl(DECKARD_ADSP_OUT_ADDR);

	if ( payload.bits.magic == DECKARD_ADSP_OUT_MAGIC )
	{
		if ( runstate ) *runstate = payload.bits.run_state;
		if ( sequence_count ) *sequence_count = ( payload.bits.sequence_count ) + 1; // to not overlap with 0
		return payload.u32;
	}
	else
	{
		if ( runstate ) *runstate = RUNSTATE_UNKNOWN;
		if ( sequence_count ) *sequence_count = 0;
		return 0;
	}
}

ADSPLite_RunState_t deckard_adsp_wait_for_runstate( ADSPLite_RunState_t expected_runstate, int max_wait_ms )
{
	static const int k_pollDelayMS = 50;

	deckard_log( "Wait for ADSP runstate: %d ... (max wait %d ms)\n", (int) expected_runstate, max_wait_ms );

	ADSPLite_RunState_t runstate = RUNSTATE_UNKNOWN;
	int sequence_count = 0;
	deckard_adsp_read( &runstate, &sequence_count );

	ulong starttime = get_timer( 0 );
	while ( get_timer( starttime ) < max_wait_ms )
	{
		deckard_adsp_read( &runstate, &sequence_count );
		if ( runstate == expected_runstate )
		{
			deckard_log( "    completed ADSP wait in %d ms\n", (int) get_timer( starttime ) );
			return runstate;
		}

		mdelay( k_pollDelayMS );
	}

	deckard_log( "    ADSP wait timeout\n" );
	return runstate;
}