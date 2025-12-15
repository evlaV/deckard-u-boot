// SPDX-License-Identifier: GPL-2.0+
/*

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
#include "vrterminal.h"
#include "vrterminal_drawing.h"
#include "console_vr_debug.h"
#include <console.h>

struct vrconsole_priv {
	struct video_fontdata *fontdata; /* To be removed */
	VRFont_t vrfont;
	VRTerminal_t vrterminal;
};

#ifndef NULL
#define NULL 0
#endif

//#define USE_NON_VR_MODE

/*to enable in console
setenv keep_display 1
env save
reset

#now on each boot:

#old console:
setenv stdout serial,vidconsole
*/


static int consolevr_set_row(struct udevice *dev, uint row, int clr)
{
	return 0;
}

static int consolevr_puts(struct udevice *dev, const char * str)
{
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct video_fontdata *fontdata = priv->fontdata;
	VRFont_t * pFont = &priv->vrfont;
	VRTerminal_t * pTerminal = &priv->vrterminal;
	// int pbytes = VNBYTES(vid_priv->bpix);

	int H = vid_priv->xsize;
	int W = vid_priv->ysize * 4;
	Pixbuf pb; pb.w=W; pb.h=H; pb.stride=17280;
	pb.px = vid_priv->fb;
	
	int nTerminalSizeX = pTerminal->width;
	int nTerminalSizeY = pTerminal->height;

	static TerminalUpdate_t terminalUpdate;
	static VRStereoBounds_t terminalBounds;

	vrterminal_update_reset( &terminalUpdate );

	static int first_draw = 1;
	if ( first_draw )
	{
		first_draw = 0;
		terminalBounds = pTerminal->terminalBounds;

		// expand terminalBounds by the glyph size
		// this is overly conservative given a centered glyph (/2 would be better)
		// but this accounts for non-center offsets
		// the * 10 accounts for pixel -> subpixel
		vrbounds_pad( &terminalBounds,
			( (int) pFont->maxGlyphWidth ) * 10, ( (int) pFont->maxGlyphWidth ) * 10,
			( (int) pFont->maxGlyphHeight ) * 10, ( (int) pFont->maxGlyphHeight ) * 10 );

		// clear the full frame on first draw
		clear_pixbuf( &pb, W, H );
	}

	vrterminal_feed( &terminalUpdate, pTerminal, str, strlen( str ) );

	if ( terminalUpdate.full_update )
	{
		clear_stereo_bounds( &pb, &terminalBounds );
		for ( int row = 0; row < nTerminalSizeY ; ++row )
		{
			for ( int col = 0; col < nTerminalSizeX; ++col )
			{
				const VRCell_t *cell = &pTerminal->vCells[ row * nTerminalSizeX + col ];
				if (cell->ch == 0 || cell->ch == ' ')
					continue;
				VRTerminalPoint_t * pTerminalPoint = vrterminal_get_point( pTerminal, col, row );
				draw_glyph( &pb, pTerminalPoint, pFont, cell->ch );
			}
		}
	}
	else
	{
		// Otherwise add only the new characters
		for ( int nUpdate = 0; nUpdate < (int) terminalUpdate.update_count; ++nUpdate )
		{
			int row = terminalUpdate.vUpdates[nUpdate].y;
			int col = terminalUpdate.vUpdates[nUpdate].x;
			const VRCell_t *cell = &pTerminal->vCells[ row * nTerminalSizeX + col ];
			if (cell->ch == 0 || cell->ch == ' ')
				continue;
			VRTerminalPoint_t * pTerminalPoint = vrterminal_get_point( pTerminal, col, row );
			draw_glyph( &pb, pTerminalPoint, pFont, cell->ch );
		}
	}

	// TODO: only damage the actual region changed
	// (though this isnt used right now on steam frame)
	for ( int neye = 0; neye<2; ++neye )
	{
    	video_damage(vid, terminalBounds.min_x[neye]/10, terminalBounds.min_y[neye]/10,
			terminalBounds.max_x[neye]/10 - terminalBounds.min_x[neye]/10,
			terminalBounds.max_y[neye]/10 - terminalBounds.min_y[neye]/10);
	}

	return VID_TO_POS(fontdata->width);
}

static int consolevr_putc_xy(struct udevice *dev, uint x_frac, uint y, int cp)
{
	return 0;
}

static int consolevr_move_rows(struct udevice *dev, uint rowdst, uint rowsrc, uint count)
{
	return 0;
}

const char *consolevr_simple_get_font_size(struct udevice *dev, uint *sizep)
{
	struct vrconsole_priv *priv = dev_get_priv(dev);
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

// TODO: find a better place to land global deckard utils that makes these not needed
// (and probably not deckard specific)
int deckard_get_dev_part_str_buf( const char *part_name, char *out, int outsize );
const char *deckard_get_booted_firmware_slotname(void);

static int vrconsole_load_erofs(const char *filename)
{
	#define DEVPARTMAXLEN 16
	#define LOADADDRMAXLEN 20
	#define LOGO_PATHMAX 256

	const char * deckard_firmware_slotname = deckard_get_booted_firmware_slotname();
	char deckard_firmware_part[DEVPARTMAXLEN] = { 0 };
	if ( deckard_get_dev_part_str_buf( deckard_firmware_slotname, &deckard_firmware_part[0], sizeof(deckard_firmware_part ) ) != 0 )
	{
		return 1;
	}

	char loadaddr_arg[LOADADDRMAXLEN] = { 0 }; //e.g. "0xa7000000"
	char file_arg[LOGO_PATHMAX + 1] = { 0 };
	int ret = 0;

	/* Setup load address */
	snprintf(loadaddr_arg, LOADADDRMAXLEN, "0x%x", CONFIG_SYS_LOAD_ADDR);
	snprintf(file_arg, LOGO_PATHMAX, "/%s", filename);

	int argc = 5;
	char * argv[5] = { "do_the_dew", "scsi", deckard_firmware_part, loadaddr_arg, file_arg };
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
	// int ret;
	
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

	// Init the font and the terminal
	int nerr = vrfont_init( &priv->vrfont, priv->vrfont.pData, priv->vrfont.size );
	if ( nerr != 0 )
	{
		printf("Font %s failed init (%d) \n", vrfontfilename, nerr );
		goto out_close;
	}

	nerr = vrterminal_init( &priv->vrterminal, priv->vrterminal.pData, priv->vrterminal.size );
	if ( nerr != 0 )
	{
		printf("Terminal %s failed init (%d) \n", vrterminalfilename, nerr );
		goto out_close;
	}

out_close:
	return 0;
}

#ifdef USE_NON_VR_MODE
struct vidconsole_ops consolevr_ops_1 = {
	.putc_xy	= consolevr_debug_putc_xy_2,
	.move_rows	= consolevr_debug_move_rows_2,
	.set_row	= consolevr_debug_set_row_2,
	.get_font_size	= consolevr_simple_get_font_size,
	.get_font	= consolevr_simple_get_font,
	.select_font	= consolevr_simple_select_font,
};
#else
struct vidconsole_ops consolevr_ops_1 = {
	.put_string = consolevr_puts,
	.putc_xy	= consolevr_putc_xy,
	.move_rows	= consolevr_move_rows,
	.set_row	= consolevr_set_row,
	.get_font_size	= consolevr_simple_get_font_size,
	.get_font	= consolevr_simple_get_font,
	.select_font	= consolevr_simple_select_font,
};
#endif

U_BOOT_DRIVER(vidconsolevr_1) = {
	.name	= "vidconsolevr1",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &consolevr_ops_1,
	.probe	= consolevr_probe,
	.priv_auto	= sizeof(struct vrconsole_priv),
};
