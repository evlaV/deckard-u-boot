//========= Copyright Valve Corporation ============//

#include "vrterminal.h"
#include <string.h>
#include <ansi.h>
#include <linux/ctype.h>
#include <linux/kconfig.h>

#ifndef INT32_MAX
#define INT32_MAX 0x7FFFFFFF
#endif

#ifndef INT32_MIN
#define INT32_MIN 0x80000000
#endif

static inline int32_t s32_min(int32_t a, int32_t b)
{
    return (a < b) ? a : b;
}

static inline int32_t s32_max(int32_t a, int32_t b)
{
    return (a > b) ? a : b;
}

static inline uint16_t u16_min(uint16_t a, uint16_t b)
{
    return (a < b) ? a : b;
}

static inline uint16_t u16_max(uint16_t a, uint16_t b)
{
    return (a > b) ? a : b;
}

static inline uint8_t u8_min(uint8_t a, uint8_t b)
{
    return (a < b) ? a : b;
}

static inline uint8_t u8_max(uint8_t a, uint8_t b)
{
    return (a > b) ? a : b;
}

#pragma pack( push, 1 )

// These structs must match the filesystem on disk representation
// for format version "1"
// Do not change without compat considerations

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

int init_vrfont( VRFont_t * pFont )
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

	pFont->maxGlyphWidth = 0;
	pFont->maxGlyphHeight = 0;

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
			
			pFont->maxGlyphWidth = u8_max( pFont->maxGlyphWidth, pGlyph->nWidth );
			pFont->maxGlyphHeight = u8_max( pFont->maxGlyphHeight, pGlyph->nHeight );

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

	if ( pTerminal->width == 0 || pTerminal->height == 0 )
		return 7;

	pTerminal->cursor_x = 0;
	pTerminal->cursor_y = 0;
	memset( pTerminal->vCells, 0,
       pTerminal->width * pTerminal->height * sizeof(VRCell_t) );

	// Calc the bounds for all points
	for ( int neye=0; neye<2; ++neye)
	{
		pTerminal->terminalBounds.min_x[neye] = INT32_MAX;
		pTerminal->terminalBounds.min_y[neye] = INT32_MAX;
		pTerminal->terminalBounds.max_x[neye] = INT32_MIN;
		pTerminal->terminalBounds.max_y[neye] = INT32_MIN;
	}

	for (int ny=0; ny<pTerminal->height; ++ny )
	{
		for ( int nx=0; nx<pTerminal->width; ++nx )
		{
			VRTerminalPoint_t * pPt = get_vrterminal_point( pTerminal, nx, ny );
			if ( !pPt )
				continue;
			for ( int neye =0; neye < 2; ++neye )
			{
				pTerminal->terminalBounds.min_x[neye] = s32_min( pTerminal->terminalBounds.min_x[neye],
					s32_min( s32_min( pPt->blue_x[neye], pPt->red_x[neye] ), pPt->green_x[neye] ) );
				pTerminal->terminalBounds.min_y[neye] = s32_min( pTerminal->terminalBounds.min_y[neye],
					s32_min( s32_min( pPt->blue_y[neye], pPt->red_y[neye] ), pPt->green_y[neye] ) );

				pTerminal->terminalBounds.max_x[neye] = s32_max( pTerminal->terminalBounds.max_x[neye],
					s32_max( s32_max( pPt->blue_x[neye], pPt->red_x[neye] ), pPt->green_x[neye] ) );
				pTerminal->terminalBounds.max_y[neye] = s32_max( pTerminal->terminalBounds.max_y[neye],
					s32_max( s32_max( pPt->blue_y[neye], pPt->red_y[neye] ), pPt->green_y[neye] ) );
			}
		}
	}

	pTerminal->escape_len = 0;
	pTerminal->escape = 0;

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
	VRCell_t * pCellPtr = pTerminal->vCells + (pTerminal->height - 1) * pTerminal->width;
	memset( pCellPtr, 0, sizeof( VRCell_t ) * pTerminal->width );

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

void handle_escape_char( TerminalUpdate_t * pUpdate, VRTerminal_t * pTerminal, char ch)
{
	if (!IS_ENABLED(CONFIG_VIDEO_ANSI))
		goto error;

	/* Sanity checking for bogus ESC sequences: */
	if (pTerminal->escape_len >= sizeof(pTerminal->escape_buf))
		goto error;
	if (pTerminal->escape_len == 0) {
		switch (ch) {
		case '7':
			/* Save cursor position */
			//TODO implement
#if 0			
			get_cursor_position(pTerminal, &pTerminal->row_saved, &pTerminal->col_saved);
#endif			
			pTerminal->escape = 0;

			return;
		case '8': {
			/* Restore cursor position */
			//TODO implement
#if 0
			int row = pTerminal->row_saved;
			int col = pTerminal->col_saved;

			set_cursor_position(pTerminal, row, col);
#endif			
			pTerminal->escape = 0;
			return;
		}
		case '[':
			break;
		default:
			goto error;
		}
	}

	pTerminal->escape_buf[pTerminal->escape_len++] = ch;

	/*
	 * Escape sequences are terminated by a letter, so keep
	 * accumulating until we get one:
	 */
	if (!isalpha(ch))
		return;

	/*
	 * clear escape mode first, otherwise things will get highly
	 * surprising if you hit any debug prints that come back to
	 * this console.
	 */
	pTerminal->escape = 0;

	switch (ch) {
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'E':
	case 'F': {
		int row, col, num;
		char *s = pTerminal->escape_buf;
//TODO implement		
#if 0
		/*
		 * Cursor up/down: [%dA, [%dB, [%dE, [%dF
		 * Cursor left/right: [%dD, [%dC
		 */
		s++;    /* [ */
		s = parsenum(s, &num);
		if (num == 0)			/* No digit in sequence ... */
			num = 1;		/* ... means "move by 1". */

		get_cursor_position(priv, &row, &col);
		if (ch == 'A' || ch == 'F')
			row -= num;
		if (ch == 'C')
			col += num;
		if (ch == 'D')
			col -= num;
		if (ch == 'B' || ch == 'E')
			row += num;
		if (ch == 'E' || ch == 'F')
			col = 0;
		if (col < 0)
			col = 0;
		if (row < 0)
			row = 0;
		/* Right and bottom overflows are handled in the callee. */
		set_cursor_position(dev, row, col);
#endif		
		break;
	}
	case 'H':
	case 'f': {
		int row, col;
		char *s = pTerminal->escape_buf;

//TODO implement
#if 0
		/*
		 * Set cursor position: [%d;%df or [%d;%dH
		 */
		s++;    /* [ */
		s = parsenum(s, &row);
		s++;    /* ; */
		s = parsenum(s, &col);

		/*
		 * Video origin is [0, 0], terminal origin is [1, 1].
		 */
		if (row)
			--row;
		if (col)
			--col;

		set_cursor_position(dev, row, col);
#endif
		break;
	}
	case 'J': {
		int mode;
//TODO implement
#if 0
		/*
		 * Clear part/all screen:
		 *   [J or [0J - clear screen from cursor down
		 *   [1J       - clear screen from cursor up
		 *   [2J       - clear entire screen
		 *
		 * TODO we really only handle entire-screen case, others
		 * probably require some additions to video-uclass (and
		 * are not really needed yet by efi_console)
		 */
		parsenum(pTerminal->escape_buf + 1, &mode);

		if (mode == 2) {
			int ret;

			video_clear(dev->parent);
			ret = video_sync(dev->parent, false);
			if (ret) {
#ifdef DEBUG
				console_puts_select_stderr(true, "[vc err: video_sync]");
#endif
			}
			priv->ycur = 0;
			priv->xcur_frac = priv->xstart_frac;
		
		} else {
			debug("unsupported clear mode: %d\n", mode);
		}
#endif				
		break;
	}
	case 'K': {
//TODO implement
#if 0		
		struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
		int mode;

		/*
		 * Clear (parts of) current line
		 *   [0K       - clear line to end
		 *   [2K       - clear entire line
		 */
		parsenum(priv->escape_buf + 1, &mode);

		if (mode == 2) {
			int row, col;

			get_cursor_position(priv, &row, &col);
			vidconsole_set_row(dev, row, vid_priv->colour_bg);
		}
#endif			
		break;
	}
	case 'm': {
//TODO implement
#if 0		
		struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
		char *s = priv->escape_buf;
		char *end = &priv->escape_buf[priv->escape_len];

		/*
		 * Set graphics mode: [%d;...;%dm
		 *
		 * Currently only supports the color attributes:
		 *
		 * Foreground Colors:
		 *
		 *   30	Black
		 *   31	Red
		 *   32	Green
		 *   33	Yellow
		 *   34	Blue
		 *   35	Magenta
		 *   36	Cyan
		 *   37	White
		 *
		 * Background Colors:
		 *
		 *   40	Black
		 *   41	Red
		 *   42	Green
		 *   43	Yellow
		 *   44	Blue
		 *   45	Magenta
		 *   46	Cyan
		 *   47	White
		 */

		s++;    /* [ */
		while (s < end) {
			int val;

			s = parsenum(s, &val);
			s++;

			switch (val) {
			case 0:
				/* all attributes off */
				video_set_default_colors(dev->parent, false);
				break;
			case 1:
				/* bold */
				vid_priv->fg_col_idx |= 8;
				vid_priv->colour_fg = video_index_to_colour(
						vid_priv, vid_priv->fg_col_idx);
				break;
			case 7:
				/* reverse video */
				vid_priv->colour_fg = video_index_to_colour(
						vid_priv, vid_priv->bg_col_idx);
				vid_priv->colour_bg = video_index_to_colour(
						vid_priv, vid_priv->fg_col_idx);
				break;
			case 30 ... 37:
				/* foreground color */
				vid_priv->fg_col_idx &= ~7;
				vid_priv->fg_col_idx |= val - 30;
				vid_priv->colour_fg = video_index_to_colour(
						vid_priv, vid_priv->fg_col_idx);
				break;
			case 40 ... 47:
				/* background color, also mask the bold bit */
				vid_priv->bg_col_idx &= ~0xf;
				vid_priv->bg_col_idx |= val - 40;
				vid_priv->colour_bg = video_index_to_colour(
						vid_priv, vid_priv->bg_col_idx);
				break;
			default:
				/* ignore unsupported SGR parameter */
				break;
			}
		}
#endif
		break;
	}
	default:
		debug("unrecognized escape sequence: %*s\n",
		      pTerminal->escape_len, pTerminal->escape_buf);
	}

	return;

error:
	/* something went wrong, just revert to normal mode: */
	pTerminal->escape = 0;
}

void vrterminal_feed( TerminalUpdate_t * pUpdate, VRTerminal_t * pTerminal, const char * buf, int nSize )
{


	for ( int i = 0; i < nSize; ++i ) {
		unsigned char c = (unsigned char) buf[i];

		if (pTerminal->escape) {
			handle_escape_char(pUpdate,pTerminal,c);
			return;
		}

		switch (c) {
		case '\x1b':
			pTerminal->escape_len = 0;
			pTerminal->escape = 1;
			break;

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