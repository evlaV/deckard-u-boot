// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2015 Google, Inc
 * (C) Copyright 2015
 * Bernecker & Rainer Industrieelektronik GmbH - http://www.br-automation.com
 * (C) Copyright 2023 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <charset.h>
#include <dm.h>
#include <video.h>
#include <video_console.h>
#include <video_font.h>		/* Get font data, width and height */
#include "vidconsole_internal.h"

static int console_set_row_1(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	void *start, *dst, *line;
	int i, j;

	start = vid_priv->fb + vid_priv->line_length -
		(row + 1) * fontdata->height * pbytes;
	line = start;
	for (j = 0; j < vid_priv->ysize; j++) {
		dst = line;
		for (i = 0; i < fontdata->height; i++)
			fill_pixel_and_goto_next(&dst, clr, pbytes, pbytes);
		line += vid_priv->line_length;
	}

	video_damage(dev->parent,
		     vid_priv->xsize - ((row + 1) * fontdata->height),
		     0,
		     fontdata->height,
		     vid_priv->ysize);

	return 0;
}

static int console_move_rows_1(struct udevice *dev, uint rowdst, uint rowsrc,
				   uint count)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	void *dst;
	void *src;
	int j;

	dst = vid_priv->fb + vid_priv->line_length -
		(rowdst + count) * fontdata->height * pbytes;
	src = vid_priv->fb + vid_priv->line_length -
		(rowsrc + count) * fontdata->height * pbytes;

	for (j = 0; j < vid_priv->ysize; j++) {
		memmove(dst, src, fontdata->height * pbytes * count);
		src += vid_priv->line_length;
		dst += vid_priv->line_length;
	}

	video_damage(dev->parent,
		     vid_priv->xsize - ((rowdst + count) * fontdata->height),
		     0,
		     count * fontdata->height,
		     vid_priv->ysize);

	return 0;
}

static int console_putc_xy_1(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	int x, linenum, ret;
	void *start, *line;
	u8 ch = console_utf_to_cp437(cp);
	uchar *pfont = fontdata->video_fontdata +
			ch * fontdata->char_pixel_bytes;

	if (x_frac + VID_TO_POS(vc_priv->x_charsize) > vc_priv->xsize_frac)
		return -EAGAIN;
	linenum = VID_TO_PIXEL(x_frac) + 1;
	x = y + 1;
	start = vid_priv->fb + linenum * vid_priv->line_length - x * pbytes;
	line = start;

	ret = fill_char_horizontally(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	if (ret)
		return ret;

	/* We draw backwards from 'start, so account for the first line */
	video_damage(dev->parent,
		     vid_priv->xsize - y - fontdata->height,
		     linenum - 1,
		     fontdata->height,
		     fontdata->width);

	return VID_TO_POS(fontdata->width);
}

static int console_set_row_2(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
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

static int console_move_rows_2(struct udevice *dev, uint rowdst, uint rowsrc,
			       uint count)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
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

static int console_putc_xy_2(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	int linenum, x, ret;
	void *start, *line;
	u8 ch = console_utf_to_cp437(cp);
	uchar *pfont = fontdata->video_fontdata +
			ch * fontdata->char_pixel_bytes;

	if (x_frac + VID_TO_POS(vc_priv->x_charsize) > vc_priv->xsize_frac)
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

	return VID_TO_POS(fontdata->width);
}

static int console_set_row_3(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	void *start, *dst, *line;
	int i, j;

	start = vid_priv->fb + row * fontdata->height * pbytes;
	line = start;
	for (j = 0; j < vid_priv->ysize; j++) {
		dst = line;
		for (i = 0; i < fontdata->height; i++)
			fill_pixel_and_goto_next(&dst, clr, pbytes, pbytes);
		line += vid_priv->line_length;
	}

	video_damage(dev->parent,
		     row * fontdata->height,
		     0,
		     fontdata->height,
		     vid_priv->ysize);

	return 0;
}

static int console_move_rows_3(struct udevice *dev, uint rowdst, uint rowsrc,
			       uint count)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	void *dst;
	void *src;
	int j;

	dst = vid_priv->fb + rowdst * fontdata->height * pbytes;
	src = vid_priv->fb + rowsrc * fontdata->height * pbytes;

	for (j = 0; j < vid_priv->ysize; j++) {
		memmove(dst, src, fontdata->height * pbytes * count);
		src += vid_priv->line_length;
		dst += vid_priv->line_length;
	}

	video_damage(dev->parent,
		     rowdst * fontdata->height,
		     0,
		     count * fontdata->height,
		     vid_priv->ysize);

	return 0;
}

static int console_putc_xy_3(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	int linenum, x, ret;
	void *start, *line;
	u8 ch = console_utf_to_cp437(cp);
	uchar *pfont = fontdata->video_fontdata +
			ch * fontdata->char_pixel_bytes;

	if (x_frac + VID_TO_POS(vc_priv->x_charsize) > vc_priv->xsize_frac)
		return -EAGAIN;
	x = y;
	linenum = vid_priv->ysize - VID_TO_PIXEL(x_frac) - 1;
	start = vid_priv->fb + linenum * vid_priv->line_length + y * pbytes;
	line = start;

	ret = fill_char_horizontally(pfont, &line, vid_priv, fontdata, NORMAL_DIRECTION);
	if (ret)
		return ret;

	video_damage(dev->parent,
		     y,
		     linenum - fontdata->width + 1,
		     fontdata->height,
		     fontdata->width);

	return VID_TO_POS(fontdata->width);
}

/* -------- framebuffer pixbuf -------- */
typedef struct {
    uint8_t *px;   // RGBA8888
    int w, h, stride;
} Pixbuf;

typedef enum { GLYPH_TOP_TO_BOTTOM = 0, GLYPH_BOTTOM_TO_TOP = 1 } GlyphYFlip;

// Put near your Glyph typedefs
typedef enum {
    GLYPH_FLIP_NONE = 0,
    GLYPH_FLIP_X    = 1 << 0,  // mirror left<->right
    GLYPH_FLIP_Y    = 1 << 1,  // flip top<->bottom
    GLYPH_FLIP_XY   = GLYPH_FLIP_X | GLYPH_FLIP_Y
} GlyphFlip;

typedef enum {
    TEXT_DIR_LTR = 0,
    TEXT_DIR_RTL = 1
} TextDir;

void draw_text_fb( struct video_priv *vid_priv, int x, int y, int line, const char *s );

static int console_set_row_4(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
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

static int console_move_rows_4(struct udevice *dev, uint rowdst, uint rowsrc,
			       uint count)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct console_simple_priv *priv = dev_get_priv(dev);
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

static int console_putc_xy_4(struct udevice *dev, uint x_frac, uint y, int cp)
{
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct udevice *vid = dev->parent;
	struct video_priv *vid_priv = dev_get_uclass_priv(vid);
	struct console_simple_priv *priv = dev_get_priv(dev);
	struct video_fontdata *fontdata = priv->fontdata;
	int pbytes = VNBYTES(vid_priv->bpix);
	int linenum, x, ret;
	void *start, *line;

	u8 ch = console_utf_to_cp437(cp);
	uchar *pfont = fontdata->video_fontdata +
			ch * fontdata->char_pixel_bytes;

	if (x_frac + VID_TO_POS(vc_priv->x_charsize) > (vc_priv->xsize_frac / 2)) //only use half of the display to avoid stomping
		return -EAGAIN;
	linenum = vid_priv->ysize - y - 1;
	x = vid_priv->xsize - VID_TO_PIXEL(x_frac) - 1;
	start = vid_priv->fb + linenum * vid_priv->line_length + x * pbytes;
	line = start;

#if 1
	ret = fill_char_vertically(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	if (ret)
		return ret;
#else
	draw_text_fb(vid_priv, x, y, linenum, pfont);
#endif

#if 1
	//draw second copy of character on 2nd display
	line = vid_priv->fb + linenum * vid_priv->line_length + ( x + 1080 ) * pbytes;
	ret = fill_char_vertically(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	//ret = fill_char_vertically(pfont, &line, vid_priv, fontdata, FLIPPED_DIRECTION);
	//ret = fill_char_vertically4(pfont, x, linenum, vid_priv, fontdata, FLIPPED_DIRECTION);
	if (ret)
		return ret;
#endif

	int dmg_extra_x=2;
	int dmg_extra_y=2;
#if 0	
	video_damage(dev->parent, x - fontdata->width + 1 - dmg_extra_x, linenum - fontdata->height + 1 - dmg_extra_y, 
					fontdata->width + dmg_extra_x,  fontdata->height + dmg_extra_y);
#else
	video_damage(dev->parent, 0, 0, 2160,2160);
#endif
	return VID_TO_POS(fontdata->width);
}

struct vidconsole_ops console_ops_1 = {
	.putc_xy	= console_putc_xy_1,
	.move_rows	= console_move_rows_1,
	.set_row	= console_set_row_1,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

struct vidconsole_ops console_ops_2 = {
	.putc_xy	= console_putc_xy_2,
	.move_rows	= console_move_rows_2,
	.set_row	= console_set_row_2,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

struct vidconsole_ops console_ops_3 = {
	.putc_xy	= console_putc_xy_3,
	.move_rows	= console_move_rows_3,
	.set_row	= console_set_row_3,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

struct vidconsole_ops console_ops_4 = {
	.putc_xy	= console_putc_xy_4,
	.move_rows	= console_move_rows_4,
	.set_row	= console_set_row_4,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

U_BOOT_DRIVER(vidconsole_1) = {
	.name	= "vidconsole1",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &console_ops_1,
	.probe	= console_probe,
	.priv_auto	= sizeof(struct console_simple_priv),
};

U_BOOT_DRIVER(vidconsole_2) = {
	.name	= "vidconsole2",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &console_ops_2,
	.probe	= console_probe,
	.priv_auto	= sizeof(struct console_simple_priv),
};

U_BOOT_DRIVER(vidconsole_3) = {
	.name	= "vidconsole3",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &console_ops_3,
	.probe	= console_probe,
	.priv_auto	= sizeof(struct console_simple_priv),
};

U_BOOT_DRIVER(vidconsole_4) = {
	.name	= "vidconsole4",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &console_ops_4,
	.probe	= console_probe,
	.priv_auto	= sizeof(struct console_simple_priv),
};
