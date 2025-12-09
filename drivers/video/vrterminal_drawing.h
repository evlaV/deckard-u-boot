#ifndef VRTERMINAL_DRAWING
#define VRTERMINAL_DRAWING

#include <stdint.h>

/* -------- framebuffer pixbuf -------- */
typedef struct {
	uint8_t *px;   // RGBA8888
	int w, h, stride;
} Pixbuf;

void clear_pixbuf( Pixbuf *pb, int width, int height );
void clear_stereo_bounds( Pixbuf *pb, VRStereoBounds_t *bounds );
void draw_glyph( Pixbuf *pb, VRTerminalPoint_t * pTerminalPoint, VRFont_t * pFont, char codepoint );

#endif