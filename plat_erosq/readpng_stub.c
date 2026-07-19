/* No-op readpng/writepng for the EROSQ build: the device has no libpng and
 * picoarch's menu only uses readpng to OPTIONALLY override the built-in 8x10
 * font (libpicofe/menu.c generates a working font from fontdata8x8 first, then
 * tries font.png/selector.png). Returning failure keeps the built-in font.
 * Screenshots go through plat_dump_screen's BMP writer, not writepng. */
#include "libpicofe/readpng.h"

int readpng(void *dest, const char *fname, readpng_what what, int w, int h)
{
	(void)dest; (void)fname; (void)what; (void)w; (void)h;
	return -1;
}

int writepng(const char *fname, unsigned short *src, int w, int h)
{
	(void)fname; (void)src; (void)w; (void)h;
	return -1;
}
