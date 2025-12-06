//========= Copyright Valve Corporation ============//

#include "vrterminal.h"
#include <string.h>


#pragma pack( push, 1 )

typedef struct VRFontHeader_t
{
	uint32_t nMagic;
	uint8_t nVersion;
	uint8_t nStartCodePoint;
	uint8_t nNumCodePoints;
	uint8_t reserved;
} VRFontHeader_t;

typedef struct VRTerminalHeader_t
{
	uint32_t nMagic;
	uint8_t nVersion;
	uint8_t reserved[3];
	uint16_t nTerminalSizeX;
	uint16_t nTerminalSizeY;
} VRTerminalHeader_t;

#pragma pack( pop )

#ifndef NULL
#define NULL 0
#endif

int sanity_check_vrfont( VRFont_t * pFont )
{
	if ( !pFont )
		return 1;

	if ( pFont->size < sizeof( VRFontHeader_t ) )
	{
		return 2;
	}

	VRFontHeader_t *pHeader = (VRFontHeader_t *) pFont->pData;
	if (pHeader->nMagic != 0x56525458 )
	{
		return 3;
	}
	if (pHeader->nVersion != 1 )
	{
		return 4;
	}

	// confirm all w/h are sane.  confirm all origins are contained in size
	int nMinSpace = 2 * pHeader->nNumCodePoints * sizeof( VRGlyph_t ) + sizeof( VRFontHeader_t );
	if ( pFont->size < nMinSpace )
	{
		return 5;
	}

	int nglyphs = 0;
	// int nlastoffset = 0;
	int nStartCodePoint = pHeader->nStartCodePoint;
	int nEndCodePoint = pHeader->nStartCodePoint + pHeader->nNumCodePoints;
	for ( int codepoint = nStartCodePoint; codepoint<=nEndCodePoint; ++codepoint )
	{
		for (int eye = 0; eye <2; ++eye )
		{
			VRGlyph_t * pGlyph = find_vrglyph( pFont, codepoint, eye );
			if (!pGlyph)
				continue;

			if ( pGlyph->nWidth > 64 || pGlyph->nHeight > 64 || 
				pGlyph->nOriginX >= pGlyph->nWidth || pGlyph->nOriginY >= pGlyph->nHeight )
			{
				return 6;
			}

			if ( pGlyph->nDataOffsetFromFileStart )
			{
				int nPixelDataEnd = pGlyph->nDataOffsetFromFileStart + pGlyph->nWidth * pGlyph->nHeight;
				if ( nPixelDataEnd > pFont->size )
					return 7;
			}

			nglyphs++;
		}
	}

	return 0;
}

VRGlyph_t * find_vrglyph( const VRFont_t * pFont, unsigned char codepoint, int neye )
{
	VRFontHeader_t *pHeader = (VRFontHeader_t *) pFont->pData;

	if ( codepoint < pHeader->nStartCodePoint || codepoint > ( pHeader->nStartCodePoint + pHeader->nNumCodePoints ) )
		return NULL;
	int nHeaderIndex = ( codepoint - pHeader->nStartCodePoint ) * 2 + neye;

	VRGlyph_t * pGlyph = (VRGlyph_t *) ( pFont->pData + sizeof( VRFontHeader_t ) + nHeaderIndex * sizeof( VRGlyph_t ) );
	if ( pGlyph->nDataOffsetFromFileStart == 0 )
	{
		return NULL;
	}

	return pGlyph;
}

const unsigned char * find_vrglyph_pixeldata( const VRFont_t * pFont, const VRGlyph_t * pGlyph )
{
	if ( !pFont || !pGlyph || !pGlyph->nDataOffsetFromFileStart )
		return NULL;

	int nPixelDataEnd = pGlyph->nDataOffsetFromFileStart + pGlyph->nWidth * pGlyph->nHeight;
	if ( nPixelDataEnd > pFont->size )
		return NULL;

	return pFont->pData + pGlyph->nDataOffsetFromFileStart;
}

int init_vrterminal( VRTerminal_t * pTerminal )
{
	if ( !pTerminal )
		return 1;

	if ( pTerminal->size < sizeof( VRTerminalHeader_t ) )
	{
		return 2;
	}

	VRTerminalHeader_t *pHeader = (VRTerminalHeader_t *) pTerminal->pData;
	if (pHeader->nMagic != 0x56525459 )
	{
		return 3;
	}
	if (pHeader->nVersion != 1 )
	{
		return 4;
	}

	int nCells = pHeader->nTerminalSizeX * pHeader->nTerminalSizeY;
	int nMinSpace = nCells * sizeof( VRTerminalPoint_t ) + sizeof( VRTerminalHeader_t );
	if ( pTerminal->size < nMinSpace )
	{
		return 5;
	}

	pTerminal->width = pHeader->nTerminalSizeX;
	pTerminal->height = pHeader->nTerminalSizeY;
	if ( pTerminal->width * pTerminal->height > TERMINAL_MAX_CELLS )
		return 6;

	pTerminal->cursor_x = 0;
	pTerminal->cursor_y = 0;
	memset( pTerminal->vCells, 0,
       pTerminal->width * pTerminal->height * sizeof(VRCell_t) );
	return 0;
}

VRTerminalPoint_t * get_vrterminal_point( VRTerminal_t * pTerminal, int idx, int idy )
{
	if ( !pTerminal || pTerminal->size < sizeof( VRTerminalHeader_t ) )
		return NULL;
	VRTerminalHeader_t *pHeader = (VRTerminalHeader_t *) pTerminal->pData;
	if ( idx < 0 || idx >= pHeader->nTerminalSizeX || idy < 0 || idy >= pHeader->nTerminalSizeY )
		return NULL;
	const int nTerminalPointIndex = pHeader->nTerminalSizeX * idy + idx;
	VRTerminalPoint_t * pTerminalPoint = ( VRTerminalPoint_t * ) &pTerminal->pData[ sizeof( VRTerminalHeader_t ) + nTerminalPointIndex * sizeof( VRTerminalPoint_t ) ];
	if ( !pTerminalPoint->nValid )
		return NULL;
	return pTerminalPoint;
}

static const VRCell_t s_empty_cell = { 0 };

static void vrterminal_scroll_up( TerminalUpdate_t * pUpdate, VRTerminal_t *pTerminal )
{
	// cells[0] is top left cell
	// Move all rows up by 1
	memmove( &pTerminal->vCells[0], &pTerminal->vCells[pTerminal->width],
			(pTerminal->height - 1) * pTerminal->width * sizeof(VRCell_t) );

	// clear the bottom row
	for ( int x = 0; x < pTerminal->width; ++x )
	{
		pTerminal->vCells[(pTerminal->height - 1) * pTerminal->width + x] = s_empty_cell;
	}

	if ( pUpdate )
	{
		pUpdate->full_update = 1;
	}
}

static void vrterminal_record_update( TerminalUpdate_t * pUpdate, uint8_t x, uint8_t y )
{
	if ( !pUpdate || pUpdate->full_update )
		return;

	if ( pUpdate->update_count >= ( TERMINAL_UPDATE_COUNT_MAX - 1 ) )
	{
		pUpdate->full_update = 1;
		return;
	}

	pUpdate->vUpdates[pUpdate->update_count].x = x;
	pUpdate->vUpdates[pUpdate->update_count].y = y;
	pUpdate->update_count++;
}


static void vrterminal_put_char( TerminalUpdate_t * pUpdate, VRTerminal_t * pTerminal, unsigned char c )
{
	// Wrap before writing
	if ( pTerminal->cursor_x >= pTerminal->width )
	{
		pTerminal->cursor_x = 0;
		pTerminal->cursor_y++;

		if ( pTerminal->cursor_y >= pTerminal->height )
		{
			vrterminal_scroll_up( pUpdate, pTerminal );
			pTerminal->cursor_y = pTerminal->height - 1;
		}
	}

	// Write into cell
	pTerminal->vCells[pTerminal->cursor_y * pTerminal->width + pTerminal->cursor_x].ch = c;
	vrterminal_record_update( pUpdate, pTerminal->cursor_x, pTerminal->cursor_y );

	// Advance cursor
	pTerminal->cursor_x++;

	if (pTerminal->cursor_x >= pTerminal->width)
	{
		pTerminal->cursor_x = 0;
		pTerminal->cursor_y++;

		if (pTerminal->cursor_y >= pTerminal->height) {

			vrterminal_scroll_up( pUpdate, pTerminal );
			pTerminal->cursor_y = pTerminal->height - 1;
		}
	}
}


void vrterminal_update_reset( TerminalUpdate_t * pUpdate )
{
	pUpdate->full_update = 0;
	pUpdate->update_count = 0;
}

void vrterminal_feed( TerminalUpdate_t * pUpdate, VRTerminal_t * pTerminal, const char * buf, int nSize )
{
	for ( int i = 0; i < nSize; ++i ) {
		unsigned char c = (unsigned char) buf[i];

		switch (c) {
		case '\n': // newline
			pTerminal->cursor_x = 0;
			pTerminal->cursor_y++;
			if (pTerminal->cursor_y >= pTerminal->height )
			{
				vrterminal_scroll_up( pUpdate, pTerminal );
				pTerminal->cursor_y = pTerminal->height - 1;
				if ( pUpdate )
				{
					pUpdate->full_update = 1;
				}
			}
			break;

		case '\r': // return to beginning of line
			pTerminal->cursor_x = 0;
			break;

		case '\b': // backspace
			if ( pTerminal->cursor_x > 0 ) {
				pTerminal->cursor_x--;
				pTerminal->vCells[pTerminal->cursor_y * pTerminal->width + pTerminal->cursor_x].ch = ' ';
				// TODO: should this mark an override in a different flag?
				vrterminal_record_update( pUpdate, pTerminal->cursor_x, pTerminal->cursor_y );
			}
			break;
		case '\t': // tab
		{
			static const int k_nTabWidth = 4;
			int col  = pTerminal->cursor_x;
			int next_tab = ( ( col / k_nTabWidth ) + 1 ) * k_nTabWidth;
			int spaces = next_tab - col;

			while (spaces-- > 0) {
				vrterminal_put_char( pUpdate, pTerminal, ' ' );
			}
			break;
		}
		default:
			if ( c >= 32 ) // printable
			{
				vrterminal_put_char( pUpdate, pTerminal, c );
			}
			break;
		}
	}
}