/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 ADSP functionality
 *
 */


#ifndef __DECKARD_ADSP_H__
#define __DECKARD_ADSP_H__

int deckard_adsp_start( void );
int deckard_adsp_stop( void );

// Send data to the adsp
// This is just a volatile register write; non-blocking
void deckard_adsp_write( bool dsp_exit_request, bool btn_power );

// This must match the definition in ADSP code, as it's send 'across the wire'
// dont expand these states without changing bit allocation in the implementation
typedef enum
{
    RUNSTATE_UNKNOWN = 0,
    RUNSTATE_STARTING = 1,
    RUNSTATE_RUNNING = 2,
    RUNSTATE_STOPPED = 3,
} ADSPLite_RunState_t;

typedef struct ADSP_State_t
{
    bool valid;
    ADSPLite_RunState_t runstate;
    int sequence_count;
    bool battery_ok_for_userspace;
    bool inhibit_poweroff;
    int fuel_gauge_internal;
    unsigned pdo_volt; // V
    unsigned pdo_cur; // mA
} ADSP_State_t;

void deckard_adsp_state_clear( ADSP_State_t * payload );

// Return uint32_t hash on success, 0 on failure
// This is just a volatile register read; non-blocking
void deckard_adsp_read( ADSP_State_t * payload );

// wait up to max_wait_ms for the specified adsp_state
ADSPLite_RunState_t deckard_adsp_wait_for_runstate( ADSPLite_RunState_t expected_runstate, int max_wait_ms );

static inline const char *ADSPLite_RunState_to_string( ADSPLite_RunState_t s )
{
    switch (s)
    {
        case RUNSTATE_UNKNOWN:
            return "Unknown";
        case RUNSTATE_STARTING:
            return "Starting";
        case RUNSTATE_RUNNING:
            return "Running";
        case RUNSTATE_STOPPED:
            return "Stopped";
        default:
            return "<Invalid>";
    }
}

#endif /* __DECKARD_ADSP_H__ */
