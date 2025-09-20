// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2015 Google, Inc
 * (C) Copyright 2015
 * Bernecker & Rainer Industrieelektronik GmbH - http://www.br-automation.com
 * (C) Copyright 2023 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <video.h>
#include <video_console.h>
#include <dm.h>
#include <video_font.h>
#include "vidconsole_internal.h"

/**
 * console_set_font() - prepare vidconsole for chosen font.
 *
 * @dev		vidconsole device
 * @fontdata	pointer to font data struct
 */
static int console_set_font(struct udevice *dev, struct video_fontdata *fontdata)
{
	struct console_simple_priv *priv = dev_get_priv(dev);
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

int check_bpix_support(int bpix)
{
	if (bpix == VIDEO_BPP8 && CONFIG_IS_ENABLED(VIDEO_BPP8))
		return 0;
	else if (bpix == VIDEO_BPP16 && CONFIG_IS_ENABLED(VIDEO_BPP16))
		return 0;
	else if (bpix == VIDEO_BPP32 && CONFIG_IS_ENABLED(VIDEO_BPP32))
		return 0;
	else
		return -ENOSYS;
}

inline void fill_pixel_and_goto_next(void **dstp, u32 value, int pbytes, int step)
{
	u8 *dst_byte = *dstp;

	if (pbytes == 4) {
		u32 *dst = *dstp;
		*dst = value;
	}
	if (pbytes == 2) {
		u16 *dst = *dstp;
		*dst = value;
	}
	if (pbytes == 1) {
		u8 *dst = *dstp;
		*dst = value;
	}
	*dstp = dst_byte + step;
}

int fill_char_vertically(uchar *pfont, void **line, struct video_priv *vid_priv,
			 struct video_fontdata *fontdata, bool direction)
{
	int step, line_step, pbytes, bitcount, width_remainder, ret;
	void *dst;

	ret = check_bpix_support(vid_priv->bpix);
	if (ret)
		return ret;

	pbytes = VNBYTES(vid_priv->bpix);
	if (direction) {
		step = -pbytes;
		line_step = -vid_priv->line_length;
	} else {
		step = pbytes;
		line_step = vid_priv->line_length;
	}

	width_remainder = fontdata->width % 8;
	for (int row = 0; row < fontdata->height; row++) {
		uchar bits;

		bitcount = 8;
		dst = *line;
		for (int col = 0; col < fontdata->byte_width; col++) {
			if (width_remainder) {
				bool is_last_col = (fontdata->byte_width - col == 1);

				if (is_last_col)
					bitcount = width_remainder;
			}
			bits = pfont[col];

			for (int bit = 0; bit < bitcount; bit++) {
				u32 value = (bits & 0x80) ?
					vid_priv->colour_fg :
					vid_priv->colour_bg;

				fill_pixel_and_goto_next(&dst,
							 value,
							 pbytes,
							 step
				);
				bits <<= 1;
			}
		}
		*line += line_step;
		pfont += fontdata->byte_width;
	}
	return ret;
}

int fill_char_horizontally(uchar *pfont, void **line, struct video_priv *vid_priv,
			   struct video_fontdata *fontdata, bool direction)
{
	int step, line_step, pbytes, bitcount = 8, width_remainder, ret;
	void *dst;
	u8 mask;

	ret = check_bpix_support(vid_priv->bpix);
	if (ret)
		return ret;

	pbytes = VNBYTES(vid_priv->bpix);
	if (direction) {
		step = -pbytes;
		line_step = vid_priv->line_length;
	} else {
		step = pbytes;
		line_step = -vid_priv->line_length;
	}

	width_remainder = fontdata->width % 8;
	for (int col = 0; col < fontdata->byte_width; col++) {
		mask = 0x80;
		if (width_remainder) {
			bool is_last_col = (fontdata->byte_width - col == 1);

			if (is_last_col)
				bitcount = width_remainder;
		}
		for (int bit = 0; bit < bitcount; bit++) {
			dst = *line;
			for (int row = 0; row < fontdata->height; row++) {
				u32 value = (pfont[row * fontdata->byte_width + col]
					     & mask) ? vid_priv->colour_fg : vid_priv->colour_bg;

				fill_pixel_and_goto_next(&dst,
							 value,
							 pbytes,
							 step
				);
			}
			*line += line_step;
			mask >>= 1;
		}
	}
	return ret;
}

int draw_cursor_vertically(void **line, struct video_priv *vid_priv,
			   uint height, bool direction)
{
	int step, line_step, pbytes, ret;
	uint value;
	void *dst;

	ret = check_bpix_support(vid_priv->bpix);
	if (ret)
		return ret;

	pbytes = VNBYTES(vid_priv->bpix);
	if (direction) {
		step = -pbytes;
		line_step = -vid_priv->line_length;
	} else {
		step = pbytes;
		line_step = vid_priv->line_length;
	}

	value = vid_priv->colour_fg;

	for (int row = 0; row < height; row++) {
		dst = *line;
		for (int col = 0; col < VIDCONSOLE_CURSOR_WIDTH; col++)
			fill_pixel_and_goto_next(&dst, value, pbytes, step);
		*line += line_step;
	}
	return ret;
}

/* -------- framebuffer pixbuf -------- */
typedef struct {
    uint8_t *px;   // RGBA8888
    int w, h, stride;
} Pixbuf;

/* -------- 5x7 font data -------- */
typedef struct {
    const uint8_t *bitmap;
    int w,h,stride_bytes,advance;
} Glyph;

static inline void pset(Pixbuf *pb, int x, int y,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((unsigned)x >= (unsigned)pb->w || (unsigned)y >= (unsigned)pb->h) return;
    uint8_t *p = pb->px + (size_t)y * pb->stride + (size_t)x*4;
    p[0]=r; p[1]=g; p[2]=b; p[3]=a;
}

/* --- Strict 5x7 font (bit4..bit0 = left..right), rows top->bottom --- */
static const uint8_t F5x7_AZ[26][7] = {
/* A */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
/* B */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
/* C */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
/* D */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
/* E */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
/* F */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
/* G */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
/* H */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
/* I */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
/* J */ {0x01,0x01,0x01,0x01,0x01,0x11,0x0E},
/* K */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
/* L */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
/* M */ {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
/* N */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
/* O */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
/* P */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
/* Q */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
/* R */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
/* S */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
/* T */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
/* U */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
/* V */ {0x11,0x11,0x11,0x0A,0x0A,0x04,0x04},
/* W */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
/* X */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
/* Y */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
/* Z */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
};

static const uint8_t F5x7_09[10][7] = {
/* 0 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
/* 1 */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
/* 2 */ {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F},
/* 3 */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
/* 4 */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
/* 5 */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
/* 6 */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
/* 7 */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
/* 8 */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
/* 9 */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
};

typedef enum { GLYPH_TOP_TO_BOTTOM = 0, GLYPH_BOTTOM_TO_TOP = 1 } GlyphYFlip;

// Put near your Glyph typedefs
typedef enum {
    GLYPH_FLIP_NONE = 0,
    GLYPH_FLIP_X    = 1 << 0,  // mirror left<->right
    GLYPH_FLIP_Y    = 1 << 1,  // flip top<->bottom
    GLYPH_FLIP_XY   = GLYPH_FLIP_X | GLYPH_FLIP_Y
} GlyphFlip;

/* LSB 5-bit rows, bit (w-1..0) = left..right */
static inline int glyph_bit_flip(const Glyph *g, int x, int y, GlyphYFlip yflip) {
    if (!g->bitmap) return 0;
    if ((unsigned)x >= (unsigned)g->w || (unsigned)y >= (unsigned)g->h) return 0;

    /* Map logical y to the font row depending on flip */
    int row_index = (yflip == GLYPH_BOTTOM_TO_TOP) ? (g->h - 1 - y) : y;

    const uint8_t row = g->bitmap[row_index] & 0x1F;   /* keep 5 bits */
    const int bit_index = g->w - 1 - x;                /* 4..0 for x=0..4 */
    return (row >> bit_index) & 1;
}


static inline int glyph_lookup(unsigned ch, Glyph *out) {
    if (ch >= 'A' && ch <= 'Z') {
        out->bitmap = F5x7_AZ[ch - 'A'];
        out->w=5; out->h=7; out->stride_bytes=1; out->advance=6; return 1;
    }
    if (ch >= 'a' && ch <= 'z') {
        out->bitmap = F5x7_AZ[ch - 'a'];
        out->w=5; out->h=7; out->stride_bytes=1; out->advance=6; return 1;
    }
    if (ch >= '0' && ch <= '9') {
        out->bitmap = F5x7_09[ch - '0'];
        out->w=5; out->h=7; out->stride_bytes=1; out->advance=6; return 1;
    }
    if (ch == ' ') {
        out->bitmap = NULL; out->w=5; out->h=7; out->stride_bytes=1; out->advance=6; return 1;
    }
    return 0;
}

// Q16 helpers unchanged
#define Q16(x)   ((int32_t)((x) << 16))
#define MULQ(a,b) ((int32_t)((int64_t)(a)*(int64_t)(b) >> 16))

// cos/sin(deg) for deg in [-89, +89], all Q16.16. No libm.
static void cordic_cos_sin_deg_q16_signed(int deg, int32_t *c, int32_t *s)
{
    // Clamp to the safe convergence range we care about
    if (deg < -89) deg = -89;
    if (deg >  89) deg =  89;

    // arctan(2^-i) in DEGREES, Q16
    static const int32_t atan_deg_q16[16] = {
        2949120,1740960,918120,467280,234960,117504,58752,29376,
        14688, 7344, 3672, 1836,   918,  459,  230,  115
    };

    // CORDIC gain K ≈ 0.607252935 in Q16.16
    const int32_t K_q16 = 39797;

    int32_t z = Q16(deg);
    int32_t x = K_q16;  // start already gain-corrected on x-axis
    int32_t y = 0;

    // Standard rotation-mode CORDIC; negative z naturally rotates clockwise
    for (int i = 0; i < 16; ++i) {
        int32_t sigma = (z >= 0) ? 1 : -1;
        int32_t x_new = x - sigma * (y >> i);
        int32_t y_new = y + sigma * (x >> i);
        x = x_new;
        y = y_new;
        z -= sigma * atan_deg_q16[i];
    }

    *c = x;   // cos(deg) in Q16.16
    *s = y;   // sin(deg) in Q16.16
}

// LSB 5-bit rows, bit (w-1..0) = left..right
static inline int glyph_bit_flipxy(const Glyph *g, int x, int y, GlyphFlip flip)
{
    if (!g->bitmap) return 0;
    if ((unsigned)x >= (unsigned)g->w || (unsigned)y >= (unsigned)g->h) return 0;

    // Map logical (x,y) to source row/col with requested flips
    int sx = (flip & GLYPH_FLIP_X) ? (g->w - 1 - x) : x;
    int sy = (flip & GLYPH_FLIP_Y) ? (g->h - 1 - y) : y;

    const uint8_t row = g->bitmap[sy] & 0x1F;  // use low 5 bits
    const int bit_index = g->w - 1 - sx;       // 4..0 for sx=0..4
    return (row >> bit_index) & 1;
}

static void draw_glyph_rot_deg_scaled(
    Pixbuf *pb, const Glyph *g, int ox, int oy, int deg, int S, GlyphFlip flip,
    uint8_t r, uint8_t g8, uint8_t b, uint8_t a)
{
    if (S < 1) S = 1;
    int32_t c, s; cordic_cos_sin_deg_q16_signed(deg, &c, &s);
    const int32_t SQ = Q16(S), halfQ = SQ >> 1;

    for (int sy = 0; sy < g->h; ++sy) {
        for (int sx = 0; sx < g->w; ++sx) {
            if (!glyph_bit_flipxy(g, sx, sy, flip)) continue;

            for (int iy = 0; iy < S; ++iy) {
                for (int ix = 0; ix < S; ++ix) {
                    int32_t xq = MULQ(Q16(sx), SQ) + Q16(ix) + halfQ;
                    int32_t yq = MULQ(Q16(sy), SQ) + Q16(iy) + halfQ;
                    int32_t xr = MULQ(xq, c) - MULQ(yq, s);
                    int32_t yr = MULQ(xq, s) + MULQ(yq, c);
                    pset(pb, ox + (xr >> 16), oy + (yr >> 16), r, g8, b, a);
                }
            }
        }
    }
}

typedef enum {
    TEXT_DIR_LTR = 0,
    TEXT_DIR_RTL = 1
} TextDir;

void draw_text_rot_deg_scaled_dir(Pixbuf *pb, int x, int y, const char *s, int deg, int S, 
				   GlyphFlip flip, TextDir dir, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (S < 1) S = 1;

    int32_t c, sQ;
    cordic_cos_sin_deg_q16_signed(deg, &c, &sQ);   // Q16 cos/sin

    const int32_t SQ = Q16(S);
    int32_t penx = Q16(x);
    int32_t peny = Q16(y);

    Glyph gph;
    while (*s) {
        unsigned ch = (unsigned char)*s++;

        if (!glyph_lookup(ch, &gph)) {
            int32_t advQ = MULQ(Q16(6), SQ);
            if (dir == TEXT_DIR_RTL) { penx -= MULQ(advQ, c); peny -= MULQ(advQ, sQ); }
            else                      { penx += MULQ(advQ, c); peny += MULQ(advQ, sQ); }
            continue;
        }

        // draw at current pen
        int ox = penx >> 16, oy = peny >> 16;
        if (gph.bitmap) draw_glyph_rot_deg_scaled(pb, &gph, ox, oy, deg, S, flip, r, g, b, a);

        int adv = (gph.advance > 0 ? gph.advance : gph.w);
        int32_t advQ = MULQ(Q16(adv), SQ);

        // advance forward or backward along baseline
        if (dir == TEXT_DIR_RTL) { penx -= MULQ(advQ, c); peny -= MULQ(advQ, sQ); }
        else                      { penx += MULQ(advQ, c); peny += MULQ(advQ, sQ); }
    }
}

void draw_text_fb( struct video_priv *vid_priv, int x, int y, const char *s )
{
	Pixbuf pb; 
	pb.w=vid_priv->xsize; 
	pb.h=vid_priv->ysize; 
	pb.stride=2160*4; //hack -- should we use the stride value from the framebuffer setting?
	pb.px = vid_priv->fb;

	int offset = 200;
	draw_text_rot_deg_scaled_dir(&pb, 1080/2 - offset      , 500, "HELLO", -10, 2, GLYPH_FLIP_XY, TEXT_DIR_RTL, 0x00,0xFF,0x00,0x00);
	draw_text_rot_deg_scaled_dir(&pb, 1080/2 + 1080 + offset, 500, "HELLO", 10, 2, GLYPH_FLIP_XY, TEXT_DIR_RTL, 0x00,0xFF,0x00,0x00);
}

int console_probe(struct udevice *dev)
{
	return console_set_font(dev, fonts);
}

const char *console_simple_get_font_size(struct udevice *dev, uint *sizep)
{
	struct console_simple_priv *priv = dev_get_priv(dev);

	*sizep = priv->fontdata->width;

	return priv->fontdata->name;
}

int console_simple_get_font(struct udevice *dev, int seq, struct vidfont_info *info)
{
	info->name = fonts[seq].name;

	return info->name ? 0 : -ENOENT;
}

int console_simple_select_font(struct udevice *dev, const char *name, uint size)
{
	struct video_fontdata *font;

	if (!name) {
		if (fonts->name)
			console_set_font(dev, fonts);
		return 0;
	}

	for (font = fonts; font->name; font++) {
		if (!strcmp(name, font->name)) {
			console_set_font(dev, font);
			return 0;
		}
	};
	printf("no such font: %s, make sure it's name has <width>x<height> format\n", name);
	return -ENOENT;
}
