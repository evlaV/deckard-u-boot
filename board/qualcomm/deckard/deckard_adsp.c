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
	} bits;
} UBOOT_PayloadOut_t;

typedef union ADSP_PayloadOut_t {
    uint32_t u32;
    struct {
        uint32_t magic  : 2;  // DECKARD_ADSP_OUT_MAGIC
        uint32_t sequence_count : 4; // sequence counter.  increments on each update
		uint32_t run_state : 2; // ADSPLite_RunState_t
		uint32_t fuel_gauge_internal : 8;  // 255 is invalid
		uint32_t battery_ok_for_userspace : 1;
		uint32_t inhibit_poweroff : 1; // inhibit poweroff?
		uint32_t pdo_volt : 4; // V
		uint32_t pdo_cur : 5; // mA / 100
		uint32_t qph2_en : 1;
    } bits;
} ADSP_PayloadOut_t;

typedef union ADSP_ChargerPayloadOut_t {
    uint32_t u32;
    struct {
	// 0
        uint32_t magic  : 2;  // DECKARD_ADSP_CHARGER_OUT_MAGIC
	// 2
        uint32_t sequence_count : 4; // sequence counter.  increments on each update
	// 6
	int32_t avg_current : 6; // mA / 100
	// 12
	uint32_t die_temp : 7; // milliC / 1000
	// 19
	uint32_t qph_state : 3;
	// 22
    } bits;
} ADSP_ChargerPayloadOut_t;

static int32_t sign_extend32( uint32_t value, int index )
{
	uint8_t shift = 31 - index;
	return (int32_t)( value << shift ) >> shift;
}

static_assert( sizeof( ADSP_PayloadOut_t ) == 4 );
static_assert( sizeof( UBOOT_PayloadOut_t ) == 4 );

#pragma pack(pop)

#define QDSP6V67SS_BOOT_PARAMS0 (QDSP6V67SS_BASE + 0x60)
#define DECKARD_ADSP_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 4)
#define DECKARD_UBOOT_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 8)
#define DECKARD_ADSP_CHARGER_OUT_ADDR	(QDSP6V67SS_BOOT_PARAMS0 + 12)
#define DECKARD_UBOOT_OUT_MAGIC 0b10
#define DECKARD_ADSP_OUT_MAGIC 0b01
#define DECKARD_ADSP_CHARGER_OUT_MAGIC 0b11

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

void deckard_adsp_state_clear( ADSP_State_t * payload )
{
	payload->valid = false;
	payload->runstate = RUNSTATE_UNKNOWN;
	payload->sequence_count = 0;
	payload->battery_ok_for_userspace = true; // fail safe
	payload->inhibit_poweroff = true;
	payload->fuel_gauge_internal = 255; // invalid
}

void deckard_adsp_read( ADSP_State_t * payload )
{
	ADSP_PayloadOut_t data;
	data.u32 = readl(DECKARD_ADSP_OUT_ADDR);

	if ( data.bits.magic == DECKARD_ADSP_OUT_MAGIC )
	{
		payload->valid = true;
		payload->runstate = data.bits.run_state;
		payload->sequence_count = ( data.bits.sequence_count ) + 1; // to not overlap with 0
		payload->battery_ok_for_userspace = data.bits.battery_ok_for_userspace;
		payload->inhibit_poweroff = data.bits.inhibit_poweroff;
		payload->fuel_gauge_internal = data.bits.fuel_gauge_internal;
		payload->pdo_volt = data.bits.pdo_volt * 1000;
		payload->pdo_cur = data.bits.pdo_cur * 100;
		payload->qph2_en = data.bits.qph2_en;
	}
	else
	{
		deckard_adsp_state_clear( payload );
	}

	if (payload->valid) {
		ADSP_ChargerPayloadOut_t cdata;
		cdata.u32 = readl(DECKARD_ADSP_CHARGER_OUT_ADDR);

		if ( cdata.bits.magic == DECKARD_ADSP_CHARGER_OUT_MAGIC )
		{
			payload->valid = true;
			payload->avg_current = sign_extend32(cdata.bits.avg_current, 5) * 100;
			payload->die_temp = cdata.bits.die_temp;
			payload->qph_state = cdata.bits.qph_state;
		}
	}
}

ADSPLite_RunState_t deckard_adsp_wait_for_runstate( ADSPLite_RunState_t expected_runstate, int max_wait_ms )
{
	static const int k_pollDelayMS = 50;

	deckard_log( "Wait for ADSP runstate: %d ... (max wait %d ms)\n", (int) expected_runstate, max_wait_ms );

	ADSP_State_t state = { 0 };

	ulong starttime = get_timer( 0 );

	do
	{
		deckard_adsp_read( &state );

		if ( state.runstate == expected_runstate )
		{
			deckard_log( "    completed ADSP wait in %d ms\n", (int) get_timer( starttime ) );
			return state.runstate;
		}

		mdelay( k_pollDelayMS );
	}
	while ( get_timer( starttime ) < max_wait_ms );

	deckard_log( "    ADSP wait timeout\n" );
	return state.runstate;
}
