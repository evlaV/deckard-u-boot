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

struct vrconsole_priv {
	struct video_fontdata *fontdata; /* To be removed */
	void *vrterminal;
	void *vrfont;
};

static int console_set_row_1(struct udevice *dev, uint row, int clr)
{
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct vrconsole_priv *priv = dev_get_priv(dev);
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
	struct vrconsole_priv *priv = dev_get_priv(dev);
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
	struct vrconsole_priv *priv = dev_get_priv(dev);
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

#define FIRMWARE_INTERFACE "scsi"
#define FIRMWARE_MAX_SIZE (500 * 1024)

static int vrconsole_load_erofs(char *name, char *buf)
{
	const char *dev_and_part = fs_firmware_get_partition();
	const char *ifname = FIRMWARE_INTERFACE;
	loff_t len_read = 0;
	int ret;

	printf("Loading firmware from partition: %s\n", dev_and_part);

	scsi_scan(true);

	ret = fs_set_blk_dev(ifname, dev_and_part, FS_TYPE_EROFS);
	if (ret < 0) {
		printf("%s: Unable to set blkdev %s %s\n", __func__, ifname,
			dev_and_part);
		return ret;
	}

	ret = fs_read(name, (ulong)buf, 0, FIRMWARE_MAX_SIZE,
			&len_read);
	if (ret < 0) {
		printf("%s: fail to read %s %s\n", __func__, ifname,
			dev_and_part);
		return ret;
	}

	return len_read;
}

int consolevr_probe(struct udevice *dev)
{
	struct vrconsole_priv *priv = dev_get_priv(dev);
	int ret;

	printf("consolevr probe\n");

	priv->vrterminal = malloc(FIRMWARE_MAX_SIZE);
	if (!priv->vrterminal)
		return -ENOMEM;

	priv->vrfont = malloc(FIRMWARE_MAX_SIZE);
	if (!priv->vrfont)
		return -ENOMEM;

	ret = vrconsole_load_erofs("terminal_80x24.vrterminal", priv->vrterminal);
	if (ret)
		return ret;

	ret = vrconsole_load_erofs("RobotoMono-Bold.vrfont", priv->vrterminal);
	if (ret)
		return ret;

	return 0;
}

struct vidconsole_ops consolevr_ops_1 = {
	.putc_xy	= console_putc_xy_1,
	.move_rows	= console_move_rows_1,
	.set_row	= console_set_row_1,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

U_BOOT_DRIVER(vidconsolevr_0) = {
	.name	= "vidconsolevr0",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &consolevr_ops_1,
	.probe	= consolevr_probe,
	.priv_auto	= sizeof(struct vrconsole_priv),
};
