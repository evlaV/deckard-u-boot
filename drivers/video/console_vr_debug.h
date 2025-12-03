#include <video.h>
#include <video_console.h>
#include <video_font.h>		/* Get font data, width and height */
#include <malloc.h>
#include <scsi.h>
#include <fs_loader.h>
#include <fs.h>

int consolevr_debug_putc_xy_2(struct udevice *dev, uint x_frac, uint y, int cp);
int consolevr_debug_move_rows_2(struct udevice *dev, uint rowdst, uint rowsrc,uint count);
int consolevr_debug_set_row_2(struct udevice *dev, uint row, int clr);