/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 ADSP functionality
 *
 */


#ifndef __DECKARD_ADSP_H__
#define __DECKARD_ADSP_H__

int deckard_adsp_start( void );
int deckard_adsp_stop( void );

void deckard_adsp_write( bool btn_power, bool btn_select, bool btn_vol_down, bool btn_vol_up );

// Return 0 on success
// int deckard_adsp_read( ADSP_PayloadOut_t * payload );

#endif /* __DECKARD_ADSP_H__ */
