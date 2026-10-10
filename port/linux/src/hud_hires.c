/*
HUD_HIRES.C

The high-res HUD's textures (hud_hires.h): which one stands for a bitmap being
uploaded, and each one's GL texture.

Which bitmap is at an address the game knows (from the loaded map's tags:
port/linux/game/hud_hires_tags.c). Each texture is decoded from its PNG when
first drawn and kept: up to 71 of the HUD's, about 227 MB with their mip
levels, though a game draws only some (the scopes' only when zoomed), and
the titles of the menus shown, about 3 MB each (11 MB for the carnage
report's, a whole panel), and their button icons, about 0.3 MB each (5.3
MB for the message icons' sheet).
They are drawn with linear filtering and their mip levels (d3d8_gl.c,
configure_sampler), as they are larger than they appear.

The embedded PNGs are the ones tools/hud_assets.py, title_assets.py and
button_assets.py write. A menus folder's PNGs can be anyone's. Only 8-bit
RGBA, non-interlaced PNGs are read (png_decode.c).
*/

#include "hud_hires.h"
#include "png_decode.h"
#include "platform.h"
#include "port_config.h"
#include "xgpu.h"

/* (crc32: the embedded textures' check) */
#include "zlib_prefixed.h"

#include <stdlib.h>
#include <string.h>

/* the game's (port/linux/game/hud_hires_tags.c) */
long hud_hires_asset_at(unsigned long address, long width, long height);

#define MAXIMUM_TEXTURES 128

static struct
{
	unsigned int texture;
	unsigned long levels;
	int failed;
	int other_pixels_logged;
} textures[MAXIMUM_TEXTURES];

long hud_hires_asset_count(void)
{
	return hud_hires_embedded_count < MAXIMUM_TEXTURES ? (long)hud_hires_embedded_count : MAXIMUM_TEXTURES;
}

char const *hud_hires_asset_tag(long asset)
{
	return hud_hires_embedded[asset].tag;
}

long hud_hires_asset_bitmap(long asset)
{
	return hud_hires_embedded[asset].bitmap;
}

/* whether the texture can stand for a bitmap of this size: a whole multiple
of it, the same both ways */
long hud_hires_asset_fits(long asset, long width, long height)
{
	const struct hud_hires_embedded *embedded = &hud_hires_embedded[asset];

	return width > 0 && height > 0 && embedded->width % width == 0 && embedded->height % height == 0 &&
		embedded->width / width == embedded->height / height && embedded->width / width > 1;
}

long hud_hires_override_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size)
{
	static int hud_enabled, titles_enabled;
	static unsigned long read_at = (unsigned long)-1;
	long asset;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (!hud_enabled && !titles_enabled)
		return -1;
	asset = hud_hires_asset_at(address, (long)width, (long)height);
	if (asset < 0 || asset >= hud_hires_asset_count())
		return -1;
	if (!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
		return -1;
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its texture was drawn for here "
				"(another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return -1;
	}
	return asset;
}

int hud_hires_override_coverage(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].coverage;
}

/* ---------- sprites */

/* the placeholders of the textures drawn for some of a bitmap's sprites
(port/linux/game/hud_hires_tags.c): their D3D textures' Data */
#define MAXIMUM_PLACEHOLDERS 8

static struct
{
	unsigned long data;
	long asset;
} placeholders[MAXIMUM_PLACEHOLDERS];
static long placeholder_count = 0;

/* the sequences whose sprites the texture is drawn for (hud_hires.h), or 0 */
unsigned long hud_hires_asset_sprites(long asset)
{
	return hud_hires_embedded[asset].sprites;
}

/* whether the texture drawn for some of a bitmap's sprites can be drawn for
the bitmap whose pixels are at address (guest virtual; its first mip level
level0_size bytes): its setting on, the pixels those it was drawn for, and
the texture decoded */
int hud_hires_sprites_drawable(long asset, unsigned long address, unsigned long level0_size)
{
	static int hud_enabled, titles_enabled;
	static unsigned long read_at = (unsigned long)-1;
	unsigned long levels;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		hud_enabled = config_boolean("display.high_res_hud");
		titles_enabled = config_boolean("display.high_res_text");
	}
	if (asset < 0 || asset >= hud_hires_asset_count() || !hud_hires_embedded[asset].sprites ||
		!(hud_hires_embedded[asset].title ? titles_enabled : hud_enabled))
	{
		return 0;
	}
	if (crc32(0L, (const Bytef *)address, (uInt)level0_size) != hud_hires_embedded[asset].crc)
	{
		if (!textures[asset].other_pixels_logged)
		{
			platform_log("high-res hud: %s bitmap %d is not the one its sprites' texture was drawn for "
				"here (another language's or a modified map): drawn as it is",
				hud_hires_embedded[asset].tag, hud_hires_embedded[asset].bitmap);
			textures[asset].other_pixels_logged = 1;
		}
		return 0;
	}
	return hud_hires_override_texture(asset, &levels) != 0;
}

/* the placeholder the game draws the texture's sprites from: its D3D
texture (NULL forgets it) */
void hud_hires_register_placeholder(long asset, const unsigned long *texture)
{
	long index;

	for (index = 0; index < placeholder_count && placeholders[index].asset != asset; index++)
		;
	if (texture && index == placeholder_count && placeholder_count < MAXIMUM_PLACEHOLDERS)
		placeholder_count++;
	if (index < placeholder_count)
	{
		placeholders[index].data = texture ? texture[1] : 0;
		placeholders[index].asset = asset;
	}
}

unsigned int hud_hires_placeholder_texture(unsigned long data, unsigned long *levels)
{
	long index;

	for (index = 0; data && index < placeholder_count; index++)
	{
		if (placeholders[index].data == data)
			return hud_hires_override_texture(placeholders[index].asset, levels);
	}
	return 0;
}

int hud_hires_override_point_threshold(long asset)
{
	return asset >= 0 && asset < hud_hires_asset_count() && hud_hires_embedded[asset].point_threshold;
}

/* ---------- decoding */

unsigned int hud_hires_png_texture(const void *png, unsigned long size, unsigned long *levels)
{
	unsigned long width = 0, height = 0, largest;
	unsigned char *pixels = png_decode(png, size, &width, &height);
	GLuint texture;

	if (!pixels)
		return 0;
	*levels = 1;
	for (largest = width > height ? width : height; largest > 1; largest >>= 1)
		(*levels)++;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	xgpu_gl_state_invalidate();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)*levels - 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glGenerateMipmap(GL_TEXTURE_2D);
	xgpu_gl_state_invalidate();
	free(pixels);
	return texture;
}

unsigned int hud_hires_override_texture(long asset, unsigned long *levels)
{
	const struct hud_hires_embedded *embedded;

	if (asset < 0 || asset >= hud_hires_asset_count() || textures[asset].failed)
		return 0;
	if (textures[asset].texture)
	{
		*levels = textures[asset].levels;
		return textures[asset].texture;
	}
	embedded = &hud_hires_embedded[asset];
	textures[asset].texture = hud_hires_png_texture(embedded->png, embedded->png_size, &textures[asset].levels);
	if (!textures[asset].texture)
	{
		platform_log("high-res hud: could not decode the texture for %s bitmap %d", embedded->tag, embedded->bitmap);
		textures[asset].failed = 1;
		return 0;
	}
	*levels = textures[asset].levels;
	return textures[asset].texture;
}
