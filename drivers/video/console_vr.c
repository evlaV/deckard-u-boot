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

struct vrconsole_priv {
	struct video_fontdata *fontdata; /* To be removed */
	void *vrterminal;
	void *vrfont;
};

#if 0
struct console_simple_priv {
	struct video_fontdata *fontdata;
};
#endif
//static int console_set_row_1(struct udevice *dev, uint row, int clr)
//{
//	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	//struct vrconsole_priv *priv = dev_get_priv(dev);

static int console_set_row_2(struct udevice *dev, uint row, int clr)
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

static int console_move_rows_2(struct udevice *dev, uint rowdst, uint rowsrc,
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

static int console_putc_xy_2(struct udevice *dev, uint x_frac, uint y, int cp)
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


#define FIRMWARE_INTERFACE "scsi"
#define FIRMWARE_MAX_SIZE (1024 * 1024)
#define MAGIC_MAX_LEN 4

const char *deckard_get_fw_part(void);

int init_buffer(ulong addr, char *buf, const char *magic)
{
	char *data = (void*) map_sysmem(addr, 0);
	int filesize = env_get_hex("filesize", -1);
	int fileaddr = env_get_hex("fileaddr", -1);

	if(data)
	{
		printf("vrconsole: filesize = %d, fileaddr = 0x%0x\n",filesize,fileaddr);

		if(filesize >= 8)
		{
			printf("vrconsole: file contents \n");
			for(int i=0; i<8; i++)
			{
				printf("0x%0x ",data[i]);
			}
			printf("\n");
		}

		
		//assume magic is 4 bytes max
		if(filesize > MAGIC_MAX_LEN && magic)
		{
			if( strncmp(data, magic, MAGIC_MAX_LEN) == 0 )
			{
				printf("vrconsole: magic found!\n");
			}
			else
			{
				printf("vrconsole: magic not found!\n");
				return -1;
			}
		}

		printf("vrconsole: init buffer...");
		if(buf && filesize < FIRMWARE_MAX_SIZE)
		{
			memcpy(buf,data,filesize);
			printf("done\n");
		}
		else
		{
			printf("fail\n");
		}
	}
	else
	{
		return -1;
	}

	return 0;
}

static int vrconsole_load_erofs(char *filename, char *buf, const char *magic)
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


    ret = init_buffer(CONFIG_SYS_LOAD_ADDR,buf,magic);
	if (ret) return ret;

	return ret;
}

#if 0
static int vrconsole_load_erofs2(char *name, char *buf)
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
#endif

#if 0
static int console_set_font(struct udevice *dev, struct video_fontdata *fontdata)
{
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);

	debug("console_simple: setting %s font\n", fontdata->name);
	debug("width: %d\n", fontdata->width);
	debug("byte width: %d\n", fontdata->byte_width);
	debug("height: %d\n", fontdata->height);

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

	return 0;
}
#endif

int consolevr_probe(struct udevice *dev)
{
	//console_set_font(dev, fonts);
	struct vrconsole_priv *priv = dev_get_priv(dev);
	struct vidconsole_priv *vc_priv = dev_get_uclass_priv(dev);
	struct video_priv *vid_priv = dev_get_uclass_priv(dev->parent);
	struct video_fontdata *fontdata = fonts; //delete when we no longer need this
	int ret;
	const char vrterminal_metadata_file_magic[] = "YTRV";
	const char vrterminal_font_file_magic[]     = "XTRV";

	printf("consolevr probe\n");

#if 1
	//stuff to make console rot=2 mode work for debugging
	debug("console_simple: setting %s font\n", fontdata->name);
	debug("width: %d\n", fontdata->width);
	debug("byte width: %d\n", fontdata->byte_width);
	debug("height: %d\n", fontdata->height);

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
#endif

	priv->vrterminal = malloc(FIRMWARE_MAX_SIZE);
	if (!priv->vrterminal) return -ENOMEM;

	priv->vrfont = malloc(FIRMWARE_MAX_SIZE);
	if (!priv->vrfont) return -ENOMEM;

	ret = vrconsole_load_erofs("terminal_80x24.vrterminal", priv->vrterminal, vrterminal_metadata_file_magic);
	if (ret) return ret;

	ret = vrconsole_load_erofs("RobotoMono-Bold.vrfont", priv->vrfont, vrterminal_font_file_magic);
	if (ret) return ret;

	return 0;
}

struct vidconsole_ops consolevr_ops_1 = {
	.putc_xy	= console_putc_xy_2,
	.move_rows	= console_move_rows_2,
	.set_row	= console_set_row_2,
	.get_font_size	= console_simple_get_font_size,
	.get_font	= console_simple_get_font,
	.select_font	= console_simple_select_font,
};

U_BOOT_DRIVER(vidconsolevr_1) = {
	.name	= "vidconsolevr1",
	.id	= UCLASS_VIDEO_CONSOLE,
	.ops	= &consolevr_ops_1,
	.probe	= consolevr_probe,
	.priv_auto	= sizeof(struct vrconsole_priv),
};
