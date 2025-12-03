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
#include "console_vr_debug.h"

struct vrconsole_priv {
	struct video_fontdata *fontdata; /* To be removed */
	VRFont_t vrfont;
	VRTerminal_t vrterminal;
};

int consolevr_debug_set_row_2(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	void *start, *line, *dst, *end;
	int pixels = fontdata->height * vid_priv->xsize;
	int i;
	int pbytes = VNBYTES(vid_priv->bpix);

	start = vid_priv->fb + vid_priv->ysize * vid_priv->line_length -
		(row + 1) * fontdata->height * vid_priv->line_length;
	line = start;
	dst = line;
	for (i = 0; i < pixels; i++)
		fill_pixel_and_goto_next(&dst, clr, pbytes, pbytes);
	end = dst;

	video_damage(dev->parent,
		     0,
		     vid_priv->ysize - (row + 1) * fontdata->height,
		     vid_priv->xsize,
		     fontdata->height);

	return 0;
}

int consolevr_debug_move_rows_2(struct udevice *dev, uint rowdst, uint rowsrc,
			       uint count)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	void *dst;
	void *src;
	void *end;

	end = vid_priv->fb + vid_priv->ysize * vid_priv->line_length;
	dst = end - (rowdst + count) * fontdata->height *
		vid_priv->line_length;
	src = end - (rowsrc + count) * fontdata->height *
		vid_priv->line_length;
	memmove(dst, src, fontdata->height * vid_priv->line_length * count);

	video_damage(dev->parent,
		     0,
		     vid_priv->ysize - (rowdst + count) * fontdata->height,
		     vid_priv->xsize,
		     count * fontdata->height);

	return 0;
}

int consolevr_debug_putc_xy_2(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	int linenum, x, ret;
	void *start, *line;
	u8 ch = console_utf_to_cp437(cp);
	uchar *pfont = fontdata->video_fontdata +
			ch * fontdata->char_pixel_bytes;
	
	int x_frac_gutter = 540;
	x_frac = x_frac + VID_TO_POS(x_frac_gutter) ;

	if (x_frac + VID_TO_POS(vc_priv->x_charsize) > (vc_priv->xsize_frac / 2 - VID_TO_POS(x_frac_gutter)))
		return -EAGAIN;
	linenum = vid_priv->ysize - y - 1;
	x = vid_priv->xsize - VID_TO_PIXEL(x_frac) - 1;
	start = vid_priv->fb + linenum * vid_priv->line_length + x * pbytes;
	line = start;

	ret = fill_char_vertically(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	if (ret)
		return ret;

	video_damage(dev->parent,
		     x - fontdata->width + 1,
		     linenum - fontdata->height + 1,
		     fontdata->width,
		     fontdata->height);

	//draw console a second time on other display
	line = vid_priv->fb + linenum * vid_priv->line_length + (x + 2160) * pbytes;
	ret = fill_char_vertically(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	if (ret)
		return ret;

	video_damage(dev->parent,
		     (x + 2160) - fontdata->width + 1,
		     linenum - fontdata->height + 1,
		     fontdata->width,
		     fontdata->height);

	return VID_TO_POS(fontdata->width);
}


