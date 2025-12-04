// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2015 Google, Inc
 * (C) Copyright 2015
 * Bernecker & Rainer Industrieelektronik GmbH - http://www.br-automation.com
 * (C) Copyright 2023 Dzmitry Sankouski <dsankouski@gmail.com>
 * (C) Copyright 2025 Valve
 */

#include <charset.h>
#include <dm.h>
#include <video.h>
#include <video_console.h>
#include <video_font.h>		/* Get font data, width and height */
#include <malloc.h>
#include <scsi.h>
#include <fs_loader.h>
#include <fs.h>
#include "vidconsole_internal.h"
#include <env.h>
#include <mapmem.h>
#include "console_vr.h"

struct vrconsole_priv {
	struct video_fontdata *fontdata; /* To be removed */
	VRFont_t vrfont;
	VRTerminal_t vrterminal;
};


#ifndef NULL
#define NULL 0
#endif

//#define DEBUG_RENDER

/*to enable in console
setenv keep_display 1
env save
reset

#now on each boot:

#old console:
setenv stdout serial,vidconsole

*/

/* -------- framebuffer pixbuf -------- */
typedef struct {
	uint8_t *px;   // RGBA8888
	int w, h, stride;
} Pixbuf;

static inline void pset_v( Pixbuf *pb, int x, int y, uint8_t value, int channel ) {
	if ((unsigned)x >= (unsigned)pb->w || (unsigned)y >= (unsigned)pb->h || x<0 || y < 0 ) return;
	uint8_t *p = pb->px + (size_t)y * pb->stride + (size_t)x*4;
	p[channel]=value;
}


void clear_pixbuf(Pixbuf *pb, int width, int height)
{
	for(int y=0; y < height; y++)
	{
		uint8_t *row = pb->px + y * pb->stride;
		for(int x=0;x < width;x++)
		{
			row[x*4+0]=0x00;
			row[x*4+1]=0x00;
			row[x*4+2]=0x00;
			row[x*4+3]=0xFF;
		}
	}
}

#ifdef DEBUG_RENDER
	//static const int k_nCrosshairsGain = 32;
	static const int k_nDotGain = 128;
#endif

static const uint8_t k_nMinValueToBlit = 1;

void draw_glyph( Pixbuf *pb, VRTerminalPoint_t * pTerminalPoint, VRFont_t * pFont, char codepoint, int nFrame )
{
	if ( !pTerminalPoint || !pFont )
		return;

	bool bDrawDot = false; //  nFrame % 10 < 3;
	bool bDrawChar = true; // nFrame % 10 >= 3;

	for ( int neye = 0; neye < 2; ++neye )
	{
		VRGlyph_t * pGlyph = find_vrglyph( pFont, codepoint, neye );
		if ( pGlyph == NULL )
			continue;

		// blit glyph
		if ( bDrawChar )
		{
			const unsigned char * pData = find_vrglyph_pixeldata( pFont, pGlyph );
			for ( int y=0; y<pGlyph->nHeight; ++y )
			{
				for ( int x=0; x<pGlyph->nWidth; ++x )
				{
					unsigned char nValue = pData[y*pGlyph->nWidth + x];
					if ( nValue > k_nMinValueToBlit )
					{
						int xPos[3] = { pTerminalPoint->red_x[neye], pTerminalPoint->green_x[neye], pTerminalPoint->blue_x[neye] };
						int yPos[3] = { pTerminalPoint->red_y[neye], pTerminalPoint->green_y[neye], pTerminalPoint->blue_y[neye] };
						for ( int nChannel = 0; nChannel<3; ++nChannel )
						{
							pset_v( pb,
								(xPos[nChannel]+5)/10 + x - pGlyph->nOriginX,
								(yPos[nChannel]+5)/10 + y - pGlyph->nOriginY,
								nValue, nChannel );
						}
					}
				}
			}
		}
	
#ifdef DEBUG_RENDER
		int nCx = (pTerminalPoint->green_x[neye]+5)/10;
		int nCy = (pTerminalPoint->green_y[neye]+5)/10;

		// cross-hairs
		/*
		for ( int y=0; y<pGlyph->nHeight; ++y )
		{
			pset_v( pb, nCx, nCy+ y - pGlyph->nHeight/2, k_nCrosshairsGain, 1 );
		}
		for ( int x=0; x<pGlyph->nWidth; ++x )
		{
			pset_v( pb, nCx + x - pGlyph->nWidth/2, nCy, k_nCrosshairsGain, 1 );
		}*/

		// center dot
		if ( bDrawDot )
		{
			for ( int dy = -1; dy<=1; ++dy)
			{
				for ( int dx = -1; dx<=1; ++dx)
				{
					pset_v(pb, nCx+dx, nCy+dy, 0, 1 );
				}
			}

			pset_v(pb, nCx, nCy, k_nDotGain, 1 );
		}
#endif
	}

	// TODO: handle offset
}

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
	if ( pTerminal->width * pTerminal->height > MAX_CELLS )
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

static void vrterminal_scroll_up( VRTerminal_t *pTerminal )
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
}

static void vrterminal_put_char( VRTerminal_t * pTerminal, unsigned char c, int *needs_clear )
{
	// Wrap before writing
	if ( pTerminal->cursor_x >= pTerminal->width )
	{
		pTerminal->cursor_x = 0;
		pTerminal->cursor_y++;

		if ( pTerminal->cursor_y >= pTerminal->height )
		{
			vrterminal_scroll_up( pTerminal );
			pTerminal->cursor_y = pTerminal->height - 1;
			*needs_clear = 1;
		}
	}

	// Write into cell
	pTerminal->vCells[pTerminal->cursor_y * pTerminal->width + pTerminal->cursor_x].ch = c;

	// Advance cursor
	pTerminal->cursor_x++;

	if (pTerminal->cursor_x >= pTerminal->width)
	{
		pTerminal->cursor_x = 0;
		pTerminal->cursor_y++;
		if (pTerminal->cursor_y >= pTerminal->height) {
			vrterminal_scroll_up(pTerminal);
			pTerminal->cursor_y = pTerminal->height - 1;
			if (needs_clear) *needs_clear = 1;
		}
	}
}

int vrterminal_feed( VRTerminal_t * pTerminal, const char * buf, int nSize )
{
	int needs_clear = 0;

	for ( int i = 0; i < nSize; ++i ) {
		unsigned char c = (unsigned char) buf[i];

		switch (c) {
		case '\n': // newline
			pTerminal->cursor_x = 0;
			pTerminal->cursor_y++;
			if (pTerminal->cursor_y >= pTerminal->height )
			{
				vrterminal_scroll_up( pTerminal );
				pTerminal->cursor_y = pTerminal->height - 1;
				needs_clear = 1;
			}
			break;

		case '\r': // return to beginning of line
			pTerminal->cursor_x = 0;
			break;

		case '\b': // backspace
			if ( pTerminal->cursor_x > 0 ) {
				pTerminal->cursor_x--;
				pTerminal->vCells[pTerminal->cursor_y * pTerminal->width + pTerminal->cursor_x].ch = ' ';
			}
			break;
		case '\t': // tab
		{
			static const int k_nTabWidth = 4;
			int col  = pTerminal->cursor_x;
			int next_tab = ( ( col / k_nTabWidth ) + 1 ) * k_nTabWidth;
			int spaces = next_tab - col;

			while (spaces-- > 0) {
				vrterminal_put_char( pTerminal, ' ', &needs_clear );
			}
			break;
		}
		default:
			if ( c >= 32 ) // printable
			{
				vrterminal_put_char( pTerminal, c, &needs_clear );
			}
			break;
		}
	}

	return needs_clear;
}

static int consolevr_set_row(struct udevice *dev, uint row, int clr)
{
	return 0;
}

static int consolevr_move_rows(struct udevice *dev, uint rowdst, uint rowsrc,
			       uint count)
{
	return 0;
}

static int needs_pixbuf_clear = 1;

static int consolevr_putc_xy(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct video_fontdata *fontdata = priv->fontdata;
	VRFont_t * pFont = &priv->vrfont;
	VRTerminal_t * pTerminal = &priv->vrterminal;
	int nTerminalSizeX = pTerminal->width;
	int nTerminalSizeY = pTerminal->height;
	char buf[2]={0};
	int pbytes = VNBYTES(vid_priv->bpix);
	int n, H, W;
	Pixbuf pb; 
	int nFrame = 0;

	H = vid_priv->xsize;
	W = vid_priv->ysize;
	pb.w=W; 
	pb.h=H; 
	pb.stride=17280; //is this right??
	pb.px=vid_priv->fb;

	n = 1;
	buf[0] = console_utf_to_cp437(cp);

	if ( vrterminal_feed( pTerminal, buf, (size_t) n  ) || needs_pixbuf_clear)
	{
		clear_pixbuf( &pb, W, H );
		needs_pixbuf_clear = 0;
	}

	for ( int row = 0; row < nTerminalSizeY ; ++row )
	{
		for ( int col = 0; col < nTerminalSizeX; ++col )
		{
			const VRCell_t *cell = &pTerminal->vCells[ row * nTerminalSizeX + col ];
			if (cell->ch == 0 || cell->ch == ' ')
				continue;
			VRTerminalPoint_t * pTerminalPoint = get_vrterminal_point( pTerminal, col, row );
			draw_glyph( &pb, pTerminalPoint, pFont, cell->ch, nFrame );
		}
	}

/*
 * @x:	        Upper left X coordinate of the damaged rectangle
 * @y:	        Upper left Y coordinate of the damaged rectangle
 * @width:	Width of the damaged rectangle
 * @height:	Height of the damaged rectangle
	video_damage
*/

//todo can we use damage?
	//video_damage(vid, 0, 0, 4320,2160);

	return VID_TO_POS(fontdata->width);
}

const char *consolevr_simple_get_font_size(struct udevice *dev, uint *sizep)
{
	struct console_simple_priv *priv = dev_get_priv(dev);

	*sizep = priv->fontdata->width;

	return priv->fontdata->name;
}

int consolevr_simple_get_font(struct udevice *dev, int seq, struct vidfont_info *info)
{
	info->name = fonts[seq].name;

	return info->name ? 0 : -ENOENT;
}

int consolevr_simple_select_font(struct udevice *dev, const char *name, uint size)
{
	return 0;
}


#define FIRMWARE_INTERFACE "scsi"
#define FIRMWARE_MAX_SIZE (1024 * 1024)

const char *deckard_get_fw_part(void);

static int vrconsole_load_erofs(const char *filename)
{
	#define DEVPARTMAXLEN 16
	#define LOADADDRMAXLEN 16
	#define LOGO_PATHMAX 256

	int  argc = 5;
	char *argv[5];
	char arg0[] = "do_the_dew";
	char part_arg[DEVPARTMAXLEN] = { 0 };  //e.g. "1:d"
	char loadaddr_arg[LOADADDRMAXLEN] = { 0 }; //e.g. "0xa7000000"
	char file_arg[LOGO_PATHMAX + 1] = { 0 };
	int ret = 0;

	argv[0] = arg0;
	argv[1] = "scsi";
	argv[2] = part_arg;
	argv[3] = loadaddr_arg;
	argv[4] = file_arg;

	/* Setup load address */
	snprintf(loadaddr_arg, LOADADDRMAXLEN, "0x%x", CONFIG_SYS_LOAD_ADDR);
	snprintf(part_arg, DEVPARTMAXLEN, "%s", deckard_get_fw_part()); //this returns 1:d for example
	snprintf(file_arg, LOGO_PATHMAX, "/%s", filename);

	ret = do_load(NULL, 0, argc, argv, FS_TYPE_ANY);
	
	printf("[%s] read=%ld addr=0x%0lx\n",
				filename, 
				env_get_hex("filesize", -1),
				env_get_hex("fileaddr", -1) );


	if (ret) return ret;

	return ret;
}

uint8_t * load_data_fromdisk( const char * pchFilename, uint32_t *pBytes, uint32_t n_max_size )
{
	int ret = 0;
	int filesize = -1;
	int fileaddr = -1;
	char *datamap;
	uint8_t * pData;

	/////////////////////////////////////////////
	ret = vrconsole_load_erofs(pchFilename);
	//side effect of this vrconsole_load_erofs() is to set these env vars
	filesize = env_get_hex("filesize", -1);
	fileaddr = env_get_hex("fileaddr", -1);
	datamap = (void*) map_sysmem(CONFIG_SYS_LOAD_ADDR, 0);
	printf("vrconsole: filesize = %d, fileaddr = 0x%0x\n",filesize,fileaddr);
	/////////////////////////////////////////////

	if ( filesize < 0 || filesize > n_max_size )
    {
		return NULL;
    }

	pData = (unsigned char *) malloc( filesize ); //for consistency, maybe we should always malloc the same max size?
	if ( !pData ) 
	{
		return NULL;
	}

	memset( pData, 0, filesize ); //not sure we need to do this?
	memcpy( pData, datamap, filesize );
    *pBytes = filesize;

	return pData;
}


int consolevr_probe(struct udevice *dev)
{
	//console_set_font(dev, fonts);
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct video_fontdata *fontdata = fonts; //delete when we no longer need this
	int ret;
	
	char vrfontfilename[] = "RobotoMono-Bold.vrfont";
	char vrterminalfilename[] = "terminal_80x24.vrterminal";

	printf("consolevr probe\n");

	priv->fontdata = fontdata;
	vc_priv->x_charsize = fontdata->width;
	vc_priv->y_charsize = fontdata->height;
	if (vid_priv->rot % 2) {
		vc_priv->cols = vid_priv->ysize / fontdata->width;
		vc_priv->rows = vid_priv->xsize / fontdata->height;
		vc_priv->xsize_frac = VID_TO_POS(vid_priv->ysize);
	} else {
		vc_priv->cols = vid_priv->xsize / fontdata->width;
		vc_priv->rows = vid_priv->ysize / fontdata->height;
	}
	
	// Load the font from disk/memory
	priv->vrfont.pData = load_data_fromdisk( vrfontfilename, &priv->vrfont.size, FIRMWARE_MAX_SIZE );
	if ( !priv->vrfont.pData )
	{
		printf("Cannot load font %s\n", vrfontfilename );
		goto out_close;
	}

	// Load the terminal from disk/memory
	priv->vrterminal.pData = load_data_fromdisk( vrterminalfilename, &priv->vrterminal.size, FIRMWARE_MAX_SIZE );
	if ( !priv->vrterminal.pData )
	{
		printf("Cannot load terminal %s\n", vrterminalfilename );
		goto out_close;
	}

	// Dont forget to validate it!
	int nerr = sanity_check_vrfont( &priv->vrfont );
	if ( nerr != 0 )
	{
		printf("Font %s failed sanity check (%d) \n", vrfontfilename, nerr );
		goto out_close;
	}

	nerr = init_vrterminal( &priv->vrterminal );
	if ( nerr != 0 )
	{
		printf("Terminal %s failed init (%d) \n", vrterminalfilename, nerr );
		goto out_close;
	}

out_close:
	return 0;
}
struct vidconsole_ops consolevr_ops_1 = {
	.putc_xy	= consolevr_putc_xy,
	.move_rows	= consolevr_move_rows,
	.set_row	= consolevr_set_row,
	.get_font_size	= consolevr_simple_get_font_size,
	.get_font	= consolevr_simple_get_font,
	.select_font	= consolevr_simple_select_font,
};

U_BOOT_DRIVER(vidconsolevr_1) = {
	.name	= "vidconsolevr1",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &consolevr_ops_1,
	.probe	= consolevr_probe,
	.priv_auto	= sizeof(struct vrconsole_priv),
};
