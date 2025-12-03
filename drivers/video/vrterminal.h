//========= Copyright Valve Corporation ============//

#ifndef VRTERMINAL
#define VRTERMINAL

#include <charset.h>
#include <stdint.h>

// FontView -- a view into loaded memory for the font
// Does not imply data ownership

typedef struct VRFont_t
{
	uint8_t *pData;
	uint32_t size;
} VRFont_t;

int sanity_check_vrfont( VRFont_t * pFont );


#pragma pack( push, 1 )

typedef struct VRFontHeader_t
{
	uint32_t nMagic;
	uint8_t nVersion;
	uint8_t nStartCodePoint;
	uint8_t nNumCodePoints;
	uint8_t reserved;
} VRFontHeader_t;

typedef struct VRGlyph_t
{
	uint32_t nDataOffsetFromFileStart;
	uint8_t nWidth;
	uint8_t nHeight;
	uint8_t nOriginX;
	uint8_t nOriginY;
} VRGlyph_t;

typedef struct VRTerminalHeader_t
{
	uint32_t nMagic;
	uint8_t nVersion;
	uint8_t reserved[3];
	uint16_t nTerminalSizeX;
	uint16_t nTerminalSizeY;
} VRTerminalHeader_t;


typedef struct VRTerminalPoint_t
{
	uint16_t red_x[2];  // L,R. in pixel TENTHS
	uint16_t red_y[2];  // L,R. in pixel TENTHS
	uint16_t green_x[2]; // L,R. in pixel TENTHS
	uint16_t green_y[2]; // L,R. in pixel TENTHS
	uint16_t blue_x[2]; // L,R. in pixel TENTHS
	uint16_t blue_y[2]; // L,R. in pixel TENTHS
	uint8_t nValid;
	uint8_t reserved[3];
} VRTerminalPoint_t;

// TerminalView -- a view into loaded memory for the terminal
// Does not imply data ownership
#define MAX_CELLS (128 * 128)

typedef struct VRCell_t {
	uint8_t ch;  // UTF-8. todo: unicode?
	// uint8_t attrs; // bold, underline, etc
} VRCell_t;

typedef struct VRTerminal_t
{
	// no ownership over data
	uint8_t *pData;
	uint32_t size;

	int width;
	int height;
    int cursor_x;
    int cursor_y;
	VRCell_t vCells[MAX_CELLS]; // visible cells
} VRTerminal_t;

#pragma pack( pop )

VRGlyph_t * find_vrglyph( const VRFont_t * pFont, unsigned char codepoint, int neye );

const unsigned char * find_vrglyph_pixeldata( const VRFont_t * pFont, const VRGlyph_t * pGlyph );

VRTerminalPoint_t * get_vrterminal_point( VRTerminal_t * pTerminal, int idx, int idy );

int init_vrterminal( VRTerminal_t * pTerminal );

// return non-zero if clear is needed
int vrterminal_feed( VRTerminal_t * pTerminal, const char * buf, int nSize );

#endif