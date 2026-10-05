/*
PNG_DECODE.C

The PNGs the port's tools write (8-bit RGBA, not interlaced: the high-res
HUD's, tools/hud_assets.py, and the menus', tools/ce_menus.py and
tools/xbox_menu_art.py), decoded with the port's zlib (port/third_party/zlib:
a menus folder's PNGs are anyone's) for the renderers (hud_hires.c, and the
original Xbox's port/xbox/src/d3d8_nv2a.c).
*/

#include "png_decode.h"
#include "platform.h"

#include "zlib_prefixed.h"

#include <stdlib.h>
#include <string.h>

/* a PNG's inflated rows and its texels, which are held at once: 128 MB for
a 4096 by 4096 sheet (the largest shipped, 2048 by 2048, takes 32 MB), and
no more for a small file that names a large size (a menus folder's) */
#define MAXIMUM_DECODED_SIZE (192UL << 20)

static unsigned long big_endian_long(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) |
		((unsigned long)bytes[2] << 8) | bytes[3];
}

static unsigned char paeth(unsigned char left, unsigned char up, unsigned char up_left)
{
	int estimate = (int)left + up - up_left;
	int to_left = abs(estimate - left), to_up = abs(estimate - up), to_up_left = abs(estimate - up_left);

	if (to_left <= to_up && to_left <= to_up_left)
		return left;
	return to_up <= to_up_left ? up : up_left;
}

unsigned char *png_decode(const unsigned char *data, unsigned long size, unsigned long *png_width,
	unsigned long *png_height)
{
	unsigned long position = 8;
	unsigned long width = size >= 33 ? big_endian_long(data + 16) : 0;
	unsigned long height = size >= 33 ? big_endian_long(data + 20) : 0;
	unsigned long stride = width * 4, filtered_size = height * (stride + 1);
	unsigned char *compressed = NULL, *filtered = NULL, *pixels = NULL;
	unsigned long compressed_size = 0, row, column;
	uLongf inflated_size = filtered_size;
	int result;

	if (size < 33 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) || memcmp(data + 12, "IHDR", 4) ||
		!width || !height || width > 8192 || height > 8192 ||
		data[24] != 8 || data[25] != 6 || data[28] != 0)
		return NULL;
	if (filtered_size + stride * height > MAXIMUM_DECODED_SIZE)
	{
		platform_log("png: %lux%lu is too large to decode (more than %lu MB)", width, height,
			MAXIMUM_DECODED_SIZE >> 20);
		return NULL;
	}
	*png_width = width;
	*png_height = height;
	compressed = malloc(size);
	while (compressed && position + 12 <= size)
	{
		unsigned long length = big_endian_long(data + position);

		if (length > size - position - 12)
			break;
		if (!memcmp(data + position + 4, "IDAT", 4))
		{
			memcpy(compressed + compressed_size, data + position + 8, length);
			compressed_size += length;
		}
		else if (!memcmp(data + position + 4, "IEND", 4))
		{
			break;
		}
		position += 12 + length;
	}
	filtered = compressed ? malloc(filtered_size) : NULL;
	pixels = filtered ? malloc(stride * height) : NULL;
	if (!pixels)
		goto failed;
	result = uncompress(filtered, &inflated_size, compressed, compressed_size);
	/* (the game's zlib is 1.1, which can stop short of saying the stream has
	ended when the output is exactly full: all of it is enough) */
	if ((result != Z_OK && result != Z_BUF_ERROR) || inflated_size != filtered_size)
		goto failed;
	/* (each row by its filter, the first pixel's 4 bytes, which have none to
	their left, apart; the first row has none above: zeroes) */
	for (row = 0; row < height; row++)
	{
		const unsigned char *line = filtered + row * (stride + 1) + 1;
		unsigned char filter = line[-1];
		unsigned char *out = pixels + row * stride;
		const unsigned char *above = row ? out - stride : NULL;

		if (filter > 4)
			goto failed;
		if (!above && filter == 2)
			filter = 0; /* (up: zero) */
		else if (!above && filter == 4)
			filter = 1; /* (Paeth of left, zero and zero: left) */
		switch (filter)
		{
		case 0:
			memcpy(out, line, stride);
			break;
		case 1:
			memcpy(out, line, 4);
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] + out[column - 4]);
			break;
		case 2:
			for (column = 0; column < stride; column++)
				out[column] = (unsigned char)(line[column] + above[column]);
			break;
		case 3:
			for (column = 0; column < 4; column++)
				out[column] = (unsigned char)(line[column] + (above ? above[column] : 0) / 2);
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] +
					((unsigned)out[column - 4] + (above ? above[column] : 0)) / 2);
			break;
		default:
			/* (Paeth of zero, up and zero: up) */
			for (column = 0; column < 4; column++)
				out[column] = (unsigned char)(line[column] + above[column]);
			for (column = 4; column < stride; column++)
				out[column] = (unsigned char)(line[column] + paeth(out[column - 4], above[column], above[column - 4]));
			break;
		}
	}
	free(compressed);
	free(filtered);
	return pixels;

failed:
	free(compressed);
	free(filtered);
	free(pixels);
	return NULL;
}
