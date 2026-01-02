/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 ADSP functionality
 *
 */


#ifndef __DECKARD_ADSP_H__
#define __DECKARD_ADSP_H__


/*
	This is our bidirectional communications mechanism between uboot and adsp.
*/
#pragma pack(push, 1)


typedef union ADSP_PayloadOut_t {
    uint32_t u32;
    struct {
        uint32_t magic  : 5;  // DECKARD_ADSP_OUT_MAGIC
        uint32_t count  : 5;
        uint32_t rsvd   : 2; // sender should clear if unused
    } bits;
} ADSP_PayloadOut_t;

typedef union ADSP_PayloadIn_t {
    uint32_t u32;
    struct {
        uint32_t magic  : 5; // DECKARD_ADSP_IN_MAGIC
		uint32_t btn_power : 1;
        uint32_t btn_select : 1;
        uint32_t btn_vol_down : 1;
        uint32_t btn_vol_up : 1;
        uint32_t rsvd   : 23; // sender should clear if unused
    } bits;
} ADSP_PayloadIn_t;

#define DECKARD_ADSP_IN_MAGIC  0b10101
#define DECKARD_ADSP_OUT_MAGIC 0b01010

#pragma pack(pop)

int deckard_adsp_start( void );
int deckard_adsp_stop( void );

void deckard_adsp_write( ADSP_PayloadIn_t * payload );

// Return 0 on success
int deckard_adsp_read( ADSP_PayloadOut_t * payload );

#endif /* __DECKARD_ADSP_H__ */
