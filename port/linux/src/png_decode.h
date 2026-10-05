/*
PNG_DECODE.H

PNGs of the port's own assets, decoded (png_decode.c).
*/

#ifndef PNG_DECODE_H
#define PNG_DECODE_H

/* the PNG's texels, RGBA in rows top first, and its size, in memory the
caller frees; NULL if it is not one the port's tools write (8-bit RGBA,
not interlaced) */
unsigned char *png_decode(const unsigned char *data, unsigned long size, unsigned long *png_width,
	unsigned long *png_height);

#endif
